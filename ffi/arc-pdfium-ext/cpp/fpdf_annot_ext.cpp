#include "fpdf_annot_ext.h"
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "constants/annotation_common.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fpdfapi/page/cpdf_pathobject.h"
#include "core/fpdfapi/page/cpdf_colorstate.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/render/cpdf_imagerenderer.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_generateap.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfdoc/cpdf_defaultappearance.h"
#include "core/fpdfdoc/cpvt_fontmap.h"
#include "core/fpdfdoc/cpvt_variabletext.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxcrt/fx_coordinates.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_edit.h"
#include "core/fxge/cfx_color.h"
#include "core/fpdfdoc/cpdf_color_utils.h"

namespace {

// CPDFAnnotContextFromFPDFAnnotation is a plain reinterpret_cast and performs
// no validation; callers must null-check the result before dereferencing.
CPDF_AnnotContext* GetAnnotContext(FPDF_ANNOTATION annot) {
  return annot ? CPDFAnnotContextFromFPDFAnnotation(annot) : nullptr;
}

// Returns the mutable annotation dictionary for |annot|, or null if |annot|
// is null / has no context / has no dict.
RetainPtr<CPDF_Dictionary> GetMutableAnnotDictSafe(FPDF_ANNOTATION annot) {
  CPDF_AnnotContext* ctx = GetAnnotContext(annot);
  return ctx ? ctx->GetMutableAnnotDict() : nullptr;
}

}  // namespace

RetainPtr<CPDF_Dictionary> SetExtGStateInResourceDict(
    CPDF_Document* pDoc,
    const CPDF_Dictionary* pAnnotDict,
    const ByteString& sBlendMode) {
  auto pGSDict =
      pdfium::MakeRetain<CPDF_Dictionary>(pAnnotDict->GetByteStringPool());

  // ExtGState represents a graphics state parameter dictionary.
  pGSDict->SetNewFor<CPDF_Name>("Type", "ExtGState");

  // CA respresents current stroking alpha specifying constant opacity
  // value that should be used in transparent imaging model.
  float fOpacity = pAnnotDict->GetFloatFor("CA");

  pGSDict->SetNewFor<CPDF_Number>("CA", fOpacity);

  // ca represents fill color alpha specifying constant opacity
  // value that should be used in transparent imaging model.
  pGSDict->SetNewFor<CPDF_Number>("ca", fOpacity);

  // AIS represents alpha source flag specifying whether current alpha
  // constant shall be interpreted as shape value (true) or opacity value
  // (false).
  pGSDict->SetNewFor<CPDF_Boolean>("AIS", false);

  // BM represents Blend Mode
  pGSDict->SetNewFor<CPDF_Name>("BM", sBlendMode);

  auto pExtGStateDict =
      pdfium::MakeRetain<CPDF_Dictionary>(pAnnotDict->GetByteStringPool());

  pExtGStateDict->SetFor("GS", pGSDict);

  auto pResourceDict = pDoc->New<CPDF_Dictionary>();
  pResourceDict->SetFor("ExtGState", pExtGStateDict);
  return pResourceDict;
}

namespace {

// Builds the stamp annotation (RGBA -> RGB image + SMask -> Form XObject ->
// Stamp annot) appended to |pPage|. Caller has already validated |pPage|/|doc|
// and that |data| holds at least imgW*imgH*4 bytes.
FPDF_ANNOTATION CreateStampAnnotCore(CPDF_Page* pPage,
                                      CPDF_Document* doc,
                                      size_t imgW,
                                      size_t imgH,
                                      float rect_left,
                                      float rect_bottom,
                                      float rect_right,
                                      float rect_top,
                                      const uint8_t* data) {
    // 分离 RGB 和 Alpha，处理预乘 alpha
    DataVector<uint8_t> rgbBuf;
    DataVector<uint8_t> alphaBuf;
    rgbBuf.reserve(imgW * imgH * 3);
    alphaBuf.reserve(imgW * imgH);

    for (size_t i = 0; i < imgW * imgH; i++) {
        uint8_t r = data[i * 4 + 0];
        uint8_t g = data[i * 4 + 1];
        uint8_t b = data[i * 4 + 2];
        uint8_t a = data[i * 4 + 3];

        // 处理预乘 alpha：如果 alpha > 0，需要反预乘
        if (a > 0) {
            float alpha = a / 255.0f;
            r = static_cast<uint8_t>(r / alpha);
            g = static_cast<uint8_t>(g / alpha);
            b = static_cast<uint8_t>(b / alpha);
        }

        rgbBuf.push_back(r);
        rgbBuf.push_back(g);
        rgbBuf.push_back(b);
        alphaBuf.push_back(a);
    }

    // 创建 Alpha SMask
    RetainPtr<CPDF_Dictionary> smaskDict = pdfium::MakeRetain<CPDF_Dictionary>();
    smaskDict->SetNewFor<CPDF_Name>("Type", "XObject");
    smaskDict->SetNewFor<CPDF_Name>("Subtype", "Image");
    smaskDict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceGray");
    smaskDict->SetNewFor<CPDF_Number>("Width", static_cast<int>(imgW));
    smaskDict->SetNewFor<CPDF_Number>("Height", static_cast<int>(imgH));
    smaskDict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
    RetainPtr<CPDF_Stream> smaskStream =
        doc->NewIndirect<CPDF_Stream>(std::move(alphaBuf), smaskDict);
    const uint32_t smaskObjNum = smaskStream->GetObjNum();

    // 创建 Image XObject
    RetainPtr<CPDF_Dictionary> imgDict = pdfium::MakeRetain<CPDF_Dictionary>();
    imgDict->SetNewFor<CPDF_Name>("Type", "XObject");
    imgDict->SetNewFor<CPDF_Name>("Subtype", "Image");
    imgDict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
    imgDict->SetNewFor<CPDF_Number>("Width", static_cast<int>(imgW));
    imgDict->SetNewFor<CPDF_Number>("Height", static_cast<int>(imgH));
    imgDict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
    imgDict->SetNewFor<CPDF_Reference>("SMask", doc, smaskObjNum);

    RetainPtr<CPDF_Stream> imgStream =
        doc->NewIndirect<CPDF_Stream>(std::move(rgbBuf), imgDict);
    const uint32_t imgObjNum = imgStream->GetObjNum();

    // 创建 Form XObject
    RetainPtr<CPDF_Dictionary> formDict = pdfium::MakeRetain<CPDF_Dictionary>();
    formDict->SetNewFor<CPDF_Name>("Type", "XObject");
    formDict->SetNewFor<CPDF_Name>("Subtype", "Form");
    RetainPtr<CPDF_Array> bbox = formDict->SetNewFor<CPDF_Array>("BBox");
    bbox->AppendNew<CPDF_Number>(0);
    bbox->AppendNew<CPDF_Number>(0);
    bbox->AppendNew<CPDF_Number>(rect_right - rect_left);
    bbox->AppendNew<CPDF_Number>(rect_top - rect_bottom);

    RetainPtr<CPDF_Dictionary> res =
        formDict->SetNewFor<CPDF_Dictionary>("Resources");
    RetainPtr<CPDF_Dictionary> xobjs = res->SetNewFor<CPDF_Dictionary>("XObject");
    xobjs->SetNewFor<CPDF_Reference>("Im0", doc, imgObjNum);

    RetainPtr<CPDF_Array> proc = res->SetNewFor<CPDF_Array>("ProcSet");
    proc->AppendNew<CPDF_Name>("PDF");
    proc->AppendNew<CPDF_Name>("ImageC");

    std::string apContent;
    apContent += "q\n";
    apContent += std::to_string(rect_right - rect_left) + " 0 0 " +
                 std::to_string(rect_top - rect_bottom) + " 0 0 cm\n";
    apContent += "/Im0 Do\n";
    apContent += "Q\n";
    DataVector<uint8_t> apData(apContent.begin(), apContent.end());

    RetainPtr<CPDF_Stream> formStream =
        doc->NewIndirect<CPDF_Stream>(std::move(apData), formDict);
    const uint32_t formObjNum = formStream->GetObjNum();

    // 创建 Stamp 注释
    RetainPtr<CPDF_Dictionary> annotDict = doc->NewIndirect<CPDF_Dictionary>();
    annotDict->SetNewFor<CPDF_Name>("Type", "Annot");
    annotDict->SetNewFor<CPDF_Name>("Subtype", "Stamp");

    RetainPtr<CPDF_Array> rectArr = annotDict->SetNewFor<CPDF_Array>("Rect");
    rectArr->AppendNew<CPDF_Number>(rect_left);
    rectArr->AppendNew<CPDF_Number>(rect_bottom);
    rectArr->AppendNew<CPDF_Number>(rect_right);
    rectArr->AppendNew<CPDF_Number>(rect_top);

    RetainPtr<CPDF_Dictionary> ap = annotDict->SetNewFor<CPDF_Dictionary>("AP");
    ap->SetNewFor<CPDF_Reference>("N", doc, formObjNum);

    if (pPage->GetDict() && !pPage->GetDict()->IsInline()) {
        annotDict->SetNewFor<CPDF_Reference>("P", doc,
                                             pPage->GetDict()->GetObjNum());
    }

    CPDF_Dictionary* pageDict = pPage->GetMutableDict();
    RetainPtr<CPDF_Array> annots = pageDict->GetMutableArrayFor("Annots");
    if (!annots)
        annots = pageDict->SetNewFor<CPDF_Array>("Annots");
    annots->AppendNew<CPDF_Reference>(doc, annotDict->GetObjNum());

    // CPDF_Page is-an IPDF_Page; pass pPage directly as the annot context page.
    auto pNewAnnot = std::make_unique<CPDF_AnnotContext>(annotDict, pPage);
    // 返回创建的注释
    return FPDFAnnotationFromCPDFAnnotContext(pNewAnnot.release());
}

}  // namespace

FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateStampAnnotWithImageData(FPDF_PAGE page,
                                   int imageWidth,
                                   int imageHeight,
                                   float rect_left,
                                   float rect_bottom,
                                   float rect_right,
                                   float rect_top,
                                   const void* rawData) {
    if (!page || !rawData || imageWidth <= 0 || imageHeight <= 0) {
        return nullptr;
    }

    CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
    if (!pPage) {
        return nullptr;
    }

    CPDF_Document* doc = pPage->GetDocument();
    if (!doc) {
        return nullptr;
    }

    // NOTE: this legacy API has no buffer-length parameter and cannot validate
    // that |rawData| is actually imageWidth*imageHeight*4 bytes. Prefer the
    // FPDF_CreateStampAnnotWithImageData2 variant. Kept for ABI compatibility.
    return CreateStampAnnotCore(pPage, doc,
                                static_cast<size_t>(imageWidth),
                                static_cast<size_t>(imageHeight),
                                rect_left, rect_bottom, rect_right, rect_top,
                                static_cast<const uint8_t*>(rawData));
}

// Safe variant of FPDF_CreateStampAnnotWithImageData: takes |rawDataSize| and
// validates (with overflow checks) that the buffer holds at least
// imageWidth*imageHeight*4 bytes before reading. Returns NULL on any
// validation failure.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateStampAnnotWithImageData2(FPDF_PAGE page,
                                    int imageWidth,
                                    int imageHeight,
                                    float rect_left,
                                    float rect_bottom,
                                    float rect_right,
                                    float rect_top,
                                    const void* rawData,
                                    unsigned long rawDataSize) {
    if (!page || !rawData || rawDataSize == 0 || imageWidth <= 0 ||
        imageHeight <= 0) {
        return nullptr;
    }

    CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
    if (!pPage) {
        return nullptr;
    }

    CPDF_Document* doc = pPage->GetDocument();
    if (!doc) {
        return nullptr;
    }

    const size_t imgW = static_cast<size_t>(imageWidth);
    const size_t imgH = static_cast<size_t>(imageHeight);

    // Guard against overflow when computing the required byte count.
    if (imgW > SIZE_MAX / imgH) {
        return nullptr;
    }
    size_t pixel_count = imgW * imgH;
    if (pixel_count > SIZE_MAX / 4) {
        return nullptr;
    }
    size_t required = pixel_count * 4;
    if (rawDataSize < required) {
        return nullptr;
    }

    return CreateStampAnnotCore(pPage, doc, imgW, imgH,
                                rect_left, rect_bottom, rect_right, rect_top,
                                static_cast<const uint8_t*>(rawData));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetSubtype(FPDF_ANNOTATION annot, FPDF_ANNOTATION_SUBTYPE subtype) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict)
    return false;

  pAnnotDict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, CPDF_Annot::AnnotSubtypeToString(static_cast<CPDF_Annot::Subtype>(subtype)));

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAppearance(FPDF_ANNOTATION annot,
                        FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                        FPDF_WIDESTRING value,
                        FPDF_BYTESTRING blendeMode) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);

  if (!pAnnotDict) {
    return false;
  }

  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return false;
  }

  static constexpr auto kModeKeyForMode =
      std::to_array<const char*>({"N", "R", "D"});
  static_assert(kModeKeyForMode.size() == FPDF_ANNOT_APPEARANCEMODE_COUNT,
                "length of kModeKeyForMode should be equal to "
                "FPDF_ANNOT_APPEARANCEMODE_COUNT");

  const char* mode_key = kModeKeyForMode[appearanceMode];

  RetainPtr<CPDF_Dictionary> ap_dict =
      pAnnotDict->GetMutableDictFor(pdfium::annotation::kAP);

  // If `value` is null, then the action is to remove.
  if (!value) {
    if (ap_dict) {
      if (appearanceMode == FPDF_ANNOT_APPEARANCEMODE_NORMAL) {
        pAnnotDict->RemoveFor(pdfium::annotation::kAP);
      } else {
        ap_dict->RemoveFor(mode_key);
      }
    }
    return true;
  }

  // Otherwise, add/update when `value` is non-null.
  //
  // Annotation object's non-empty bounding rect will be used as the /BBox
  // for the associated /XObject object
  CFX_FloatRect rect = pAnnotDict->GetRectFor(pdfium::annotation::kRect);
  static constexpr float kMinSize = 0.000001f;
  if (rect.Width() < kMinSize || rect.Height() < kMinSize) {
    return false;
  }

  CPDF_AnnotContext* pAnnotContext = GetAnnotContext(annot);
  if (!pAnnotContext) {
    return false;
  }

  IPDF_Page* page = pAnnotContext->GetPage();
  if (!page) {
    return false;
  }
  CPDF_Document* doc = page->GetDocument();
  if (!doc) {
    return false;
  }

  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  stream_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");
  stream_dict->SetRectFor("BBox", rect);
  stream_dict->SetFor("Resources", SetExtGStateInResourceDict(doc, pAnnotDict.Get(), blendeMode));
    
  // SAFETY: required from caller.
  ByteString new_stream_data = PDF_EncodeText(
      UNSAFE_BUFFERS(WideStringFromFPDFWideString(value).AsStringView()));
  auto new_stream = doc->NewIndirect<CPDF_Stream>(std::move(stream_dict));
  new_stream->SetData(new_stream_data.unsigned_span());

  // Storing reference to indirect object in annotation's AP
  if (!ap_dict) {
    ap_dict = pAnnotDict->SetNewFor<CPDF_Dictionary>(pdfium::annotation::kAP);
  }
  ap_dict->SetNewFor<CPDF_Reference>(mode_key, doc, new_stream->GetObjNum());

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RenderToBitmap(FPDF_BITMAP bitmap,
                         FPDF_PAGE page,
                         FPDF_ANNOTATION annot,
                         const FS_MATRIX* matrix) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || !bitmap || !annot)
    return false;

  CPDF_AnnotContext* pAnnotContext = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnotContext)
    return false;

  // 1. 创建 CPDF_Annot 对象
  auto pAnnot = std::make_unique<CPDF_Annot>(
      pAnnotContext->GetMutableAnnotDict(),
      pPage->GetDocument());

  // 2. 设置渲染设备
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  auto device = CFX_RenderDevice::CreateForBitmap(pBitmap);

  // 3. 计算变换矩阵 - 关键！
  // 使用页面的 GetDisplayMatrix() 作为基础
  CFX_Matrix transform_matrix = pPage->GetDisplayMatrix();
  
  // 如果用户提供了额外的矩阵，组合它
  if (matrix) {
    transform_matrix *= CFXMatrixFromFSMatrix(*matrix);
  }

  // 4. 创建渲染上下文
  CPDF_RenderContext context(
      pPage->GetDocument(),
      pPage->GetMutablePageResources(),
      pPage->GetPageImageCache());

  // 5. 调用 DrawInContext - 这会内部调用 AnnotGetMatrix
  bool success = pAnnot->DrawInContext(
      pPage,
      &context,
      transform_matrix,  // 传入完整的变换矩阵
      CPDF_Annot::AppearanceMode::kNormal);

  if (!success) {
    return false;
  }

  // 6. 渲染上下文到设备
  context.Render(device.get(), nullptr, nullptr, nullptr);

  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetAppearanceBlendMode(FPDF_ANNOTATION annot,
                                 FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                 void* buffer,
                                 unsigned long buflen) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);

  if (!pAnnotDict) {
    return 0;
  }

  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return 0;
  }

  static constexpr auto kModeKeyForMode =
      std::to_array<const char*>({"N", "R", "D"});
  const char* mode_key = kModeKeyForMode[appearanceMode];

  // 获取 AP 字典
  RetainPtr<CPDF_Dictionary> ap_dict =
      pAnnotDict->GetMutableDictFor(pdfium::annotation::kAP);
  if (!ap_dict) {
    return 0;
  }

  // 获取指定模式的外观流
  RetainPtr<CPDF_Stream> ap_stream = ap_dict->GetMutableStreamFor(mode_key);
  if (!ap_stream) {
    return 0;
  }

  // 获取外观流的字典
  RetainPtr<CPDF_Dictionary> stream_dict = ap_stream->GetMutableDict();
  if (!stream_dict) {
    return 0;
  }

  // 获取 Resources 字典
  RetainPtr<CPDF_Dictionary> resources_dict = 
      stream_dict->GetMutableDictFor("Resources");
  if (!resources_dict) {
    return 0;
  }

  // 获取 ExtGState 字典
  RetainPtr<CPDF_Dictionary> extgstate_dict = 
      resources_dict->GetMutableDictFor("ExtGState");
  if (!extgstate_dict) {
    return 0;
  }

  // 获取 GS 字典（第一个 ExtGState）
  RetainPtr<CPDF_Dictionary> gs_dict = nullptr;
  CPDF_DictionaryLocker locker(extgstate_dict.Get());
  for (const auto& it : locker) {
    gs_dict = extgstate_dict->GetMutableDictFor(it.first.AsStringView());
    if (gs_dict) {
      break;
    }
  }

  if (!gs_dict) {
    return 0;
  }

  // 获取 BM (Blend Mode)
  ByteString blend_mode = gs_dict->GetByteStringFor("BM");
  if (blend_mode.IsEmpty()) {
    return 0;
  }

  // 计算需要的缓冲区大小（包括 null 终止符）
  unsigned long required_size = blend_mode.GetLength() + 1;

  // 如果 buffer 为 NULL，返回所需大小
  if (!buffer) {
    return required_size;
  }

  // 如果缓冲区太小，返回 0
  if (buflen < required_size) {
    return 0;
  }

  // 复制数据到缓冲区
  memcpy(buffer, blend_mode.c_str(), required_size);

  return required_size;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetAppearanceBBox(FPDF_ANNOTATION annot,
                            FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                            FS_RECTF* rect) {
  if (!rect) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);

  if (!pAnnotDict) {
    return false;
  }

  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return false;
  }

  static constexpr auto kModeKeyForMode =
      std::to_array<const char*>({"N", "R", "D"});
  const char* mode_key = kModeKeyForMode[appearanceMode];

  // Get AP dictionary
  RetainPtr<CPDF_Dictionary> ap_dict =
      pAnnotDict->GetMutableDictFor(pdfium::annotation::kAP);
  if (!ap_dict) {
    return false;
  }

  // Get appearance stream for the specified mode
  RetainPtr<CPDF_Stream> ap_stream = ap_dict->GetMutableStreamFor(mode_key);
  if (!ap_stream) {
    return false;
  }

  // Get appearance stream dictionary
  RetainPtr<CPDF_Dictionary> stream_dict = ap_stream->GetMutableDict();
  if (!stream_dict) {
    return false;
  }

  // Get BBox array
  RetainPtr<const CPDF_Array> bbox_array = stream_dict->GetArrayFor("BBox");
  if (!bbox_array || bbox_array->size() != 4) {
    return false;
  }

  // Extract BBox values
  rect->left = bbox_array->GetFloatAt(0);
  rect->bottom = bbox_array->GetFloatAt(1);
  rect->right = bbox_array->GetFloatAt(2);
  rect->top = bbox_array->GetFloatAt(3);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetColorFromDict(FPDF_ANNOTATION annot,
                            FPDFANNOT_COLORTYPE type,
                            unsigned int* R,
                            unsigned int* G,
                            unsigned int* B,
                            unsigned int* A) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);

  if (!pAnnotDict || !R || !G || !B || !A) {
    return false;
  }

  // Get color array from annotation dictionary
  // Use "IC" for interior color, "C" for border color
  RetainPtr<const CPDF_Array> pColor = pAnnotDict->GetArrayFor(
      type == FPDFANNOT_COLORTYPE_InteriorColor ? "IC" : "C");
  
  // Get alpha value from CA key (default to 1.0 if not present)
  *A = (pAnnotDict->KeyExist("CA") ? pAnnotDict->GetFloatFor("CA") : 1.0f) * 255.0f;
  
  if (!pColor) {
    // Interior color (/IC) 缺席意味着批注没有显式填充（透明），返回 false 让上层
    // 按“未配置填充”处理；绝不能像边框那样合成默认黑，否则无填充的矢量图形会被画成黑心。
    if (type == FPDFANNOT_COLORTYPE_InteriorColor) {
      return false;
    }
    // Use default border color. The defaults must be consistent with the ones
    // used to generate AP. See calls to GetColorStringWithDefault() in
    // CPDF_GenerateAP::Generate*AP().
    if (pAnnotDict->GetNameFor(pdfium::annotation::kSubtype) == "Highlight") {
      *R = 255;
      *G = 255;
      *B = 0;
    } else {
      *R = 0;
      *G = 0;
      *B = 0;
    }
    return true;
  }

  CFX_Color color = fpdfdoc::CFXColorFromArray(*pColor);
  switch (color.nColorType) {
    case CFX_Color::Type::kRGB:
      *R = color.fColor1 * 255.0f;
      *G = color.fColor2 * 255.0f;
      *B = color.fColor3 * 255.0f;
      break;
    case CFX_Color::Type::kGray:
      *R = 255.0f * color.fColor1;
      *G = 255.0f * color.fColor1;
      *B = 255.0f * color.fColor1;
      break;
    case CFX_Color::Type::kCMYK:
      *R = 255.0f * (1 - color.fColor1) * (1 - color.fColor4);
      *G = 255.0f * (1 - color.fColor2) * (1 - color.fColor4);
      *B = 255.0f * (1 - color.fColor3) * (1 - color.fColor4);
      break;
    case CFX_Color::Type::kTransparent:
      *R = 0;
      *G = 0;
      *B = 0;
      break;
  }
  
  return true;
}

// 读 annot 字典里任意颜色数组键(如 /QLFillColor、/QLBorderColor、/C)为 0..255 RGBA。
// 供 FillSign 矢量图章"删时快照 → 撤销重建"读取样式;key 取 ByteString(name) 的 CPDF 查找。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetColorArr(FPDF_ANNOTATION annot,
                      const char* key,
                      unsigned int* R,
                      unsigned int* G,
                      unsigned int* B) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict || !key || !R || !G || !B) {
    return false;
  }
  RetainPtr<const CPDF_Array> pColor = pAnnotDict->GetArrayFor(key);
  if (!pColor || pColor->size() < 3) {
    return false;
  }
  CFX_Color color = fpdfdoc::CFXColorFromArray(*pColor);
  switch (color.nColorType) {
    case CFX_Color::Type::kRGB:
      *R = color.fColor1 * 255.0f;
      *G = color.fColor2 * 255.0f;
      *B = color.fColor3 * 255.0f;
      break;
    case CFX_Color::Type::kGray:
      *R = 255.0f * color.fColor1;
      *G = 255.0f * color.fColor1;
      *B = 255.0f * color.fColor1;
      break;
    case CFX_Color::Type::kCMYK:
      *R = 255.0f * (1 - color.fColor1) * (1 - color.fColor4);
      *G = 255.0f * (1 - color.fColor2) * (1 - color.fColor4);
      *B = 255.0f * (1 - color.fColor3) * (1 - color.fColor4);
      break;
    default:
      *R = 0;
      *G = 0;
      *B = 0;
      break;
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_KeyExists(FPDF_ANNOTATION annot,
                    const char* key) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict || !key) {
    return false;
  }
  return pAnnotDict->KeyExist(key);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetColorToDict(FPDF_ANNOTATION annot,
                          FPDFANNOT_COLORTYPE type,
                          unsigned int R,
                          unsigned int G,
                          unsigned int B,
                          unsigned int A) {
  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);

  if (!pAnnotDict || R > 255 || G > 255 || B > 255 || A > 255) {
    return false;
  }

  // Set the opacity of the annotation
  pAnnotDict->SetNewFor<CPDF_Number>("CA", A / 255.0f);

  // Set the color of the annotation
  ByteStringView key = type == FPDFANNOT_COLORTYPE_InteriorColor ? "IC" : "C";
  RetainPtr<CPDF_Array> pColor = pAnnotDict->GetMutableArrayFor(key);
  if (pColor) {
    pColor->Clear();
  } else {
    pColor = pAnnotDict->SetNewFor<CPDF_Array>(ByteString(key));
  }
  
  pColor->AppendNew<CPDF_Number>(R / 255.0f);
  pColor->AppendNew<CPDF_Number>(G / 255.0f);
  pColor->AppendNew<CPDF_Number>(B / 255.0f);

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_UpdateStampAP(FPDF_ANNOTATION annot,
                        float rect_left,
                        float rect_bottom,
                        float rect_right,
                        float rect_top,
                        float matrix_a,
                        float matrix_b,
                        float matrix_c,
                        float matrix_d,
                        float matrix_e,
                        float matrix_f) {
  if (!annot) {
    return false;
  }

  float width = rect_right - rect_left;
  float height = rect_top - rect_bottom;
  if (width < 0.000001f || height < 0.000001f) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict) {
    return false;
  }

  // Get the AP dictionary
  RetainPtr<CPDF_Dictionary> ap_dict =
      pAnnotDict->GetMutableDictFor(pdfium::annotation::kAP);
  if (!ap_dict) {
    return false;
  }

  // Get the Normal appearance stream
  RetainPtr<CPDF_Stream> ap_stream = ap_dict->GetMutableStreamFor("N");
  if (!ap_stream) {
    return false;
  }

  // Update BBox to local coordinates [0, 0, width, height]
  RetainPtr<CPDF_Dictionary> stream_dict = ap_stream->GetMutableDict();
  if (!stream_dict) {
    return false;
  }

  // 记录旧 BBox 尺寸：矢量图章（AP 无图片引用）回退要据此把旧流等比缩放进新 BBox。
  float old_w = width, old_h = height;
  if (RetainPtr<const CPDF_Array> old_bbox = stream_dict->GetArrayFor("BBox");
      old_bbox && old_bbox->size() >= 4) {
    old_w = old_bbox->GetFloatAt(2) - old_bbox->GetFloatAt(0);
    old_h = old_bbox->GetFloatAt(3) - old_bbox->GetFloatAt(1);
  }
  if (old_w <= 0.000001f) old_w = width;
  if (old_h <= 0.000001f) old_h = height;

  RetainPtr<CPDF_Array> bbox = stream_dict->SetNewFor<CPDF_Array>("BBox");
  bbox->AppendNew<CPDF_Number>(0);
  bbox->AppendNew<CPDF_Number>(0);
  bbox->AppendNew<CPDF_Number>(width);
  bbox->AppendNew<CPDF_Number>(height);

  // Update the stream content: position image at (0,0) with new dimensions
  // The stream format is: q <width> 0 0 <height> 0 0 cm /ImX Do Q
  // We need to find the XObject name used in the existing stream.
  // Read the existing stream data to extract the XObject name. The /N stream may
  // be file-based (PDFium keeps large streams on disk instead of in memory), and
  // GetInMemoryRawData() DCHECKs on file-based streams — use CPDF_StreamAcc so
  // both memory- and file-based streams are read safely.
  auto stream_acc = pdfium::MakeRetain<CPDF_StreamAcc>(ap_stream);
  // 用 Filtered 解码读取：AP 流存盘重开后可能是 FlateDecode 压缩流，LoadAllDataRaw 拿到的
  // 是压缩字节（x\234...），若当算子包进 q..cm..Q 会写出非法 AP。Filtered 解出真实算子，
  // 图片分支的 " Do" 检索与矢量分支的算子包装才正确。
  stream_acc->LoadAllDataFiltered();
  pdfium::span<const uint8_t> existing_data = stream_acc->GetSpan();
  std::string existing_str(existing_data.begin(), existing_data.end());

  // Find the XObject reference in the stream (e.g., "/FXX1 Do" or "/Im0 Do")
  std::string xobject_name;
  size_t do_pos = existing_str.find(" Do");
  if (do_pos != std::string::npos) {
    // Scan backwards from " Do" to find the "/" that starts the name
    size_t slash_pos = existing_str.rfind('/', do_pos);
    if (slash_pos != std::string::npos) {
      xobject_name = existing_str.substr(slash_pos, do_pos - slash_pos);
    }
  }

  if (xobject_name.empty()) {
    // Fallback: try to find XObject name from Resources
    RetainPtr<CPDF_Dictionary> resources =
        stream_dict->GetMutableDictFor("Resources");
    if (resources) {
      RetainPtr<CPDF_Dictionary> xobjects =
          resources->GetMutableDictFor("XObject");
      if (xobjects) {
        CPDF_DictionaryLocker locker(xobjects.Get());
        auto it = locker.begin();
        if (it != locker.end()) {
          xobject_name = "/" + std::string(it->first.AsStringView().begin(),
                                            it->first.AsStringView().end());
        }
      }
    }
  }

  if (xobject_name.empty()) {
    // 矢量图章（文字/勾叉图章 AP 无图片引用，如 "q 0.9 rg 0 0 180 40 re f Q"）：
    // 不能重写图片 cm。改为把旧矢量流整体等比缩放 + 居中映射进新 BBox，保留
    // 填充/描边/字形样式（对齐签名图片图章的缩放行为，避免残留旧尺寸内容被裁切）。
    float sx = width / old_w;
    float sy = height / old_h;
    float s = sx < sy ? sx : sy;              // 等比，不拉伸变形
    if (s <= 0.000001f) s = 1.0f;
    float tx = (width - old_w * s) / 2.0f;
    float ty = (height - old_h * s) / 2.0f;

    std::string scaled;
    scaled += "q\n";
    scaled += std::to_string(s) + " 0 0 " + std::to_string(s) + " " +
              std::to_string(tx) + " " + std::to_string(ty) + " cm\n";
    scaled += existing_str;
    if (!existing_str.empty() && existing_str.back() != '\n') scaled += "\n";
    scaled += "Q\n";

    ap_stream->SetData(ByteString(scaled.c_str(), scaled.size()).unsigned_span());
    // 写入的是已解码明文算子，须去掉原 /Filter 否则 PDFium 会再 Flate 解一遍明文 → 垃圾。
    stream_dict->RemoveFor("Filter");
    stream_dict->RemoveFor("DecodeParms");
    return true;
  }

  // Build new stream content with local coordinates.
  // Bake the image's rotation matrix into the AP's cm here: the AP is what
  // FPDFAnnot_RenderToBitmap / a re-open render from, and the image XObject
  // (/Im0) carries no /Matrix of its own — so this cm is the ONLY place the
  // rotation survives. Express e/f relative to the /Rect origin (local AP space).
  std::string new_content;
  new_content += "q\n";
  new_content += std::to_string(matrix_a) + " " + std::to_string(matrix_b) + " " +
                 std::to_string(matrix_c) + " " + std::to_string(matrix_d) + " " +
                 std::to_string(matrix_e - rect_left) + " " +
                 std::to_string(matrix_f - rect_bottom) + " cm\n";
  new_content += xobject_name + " Do\n";
  new_content += "Q\n";

  ByteString new_stream_data(new_content.c_str(), new_content.size());
  ap_stream->SetData(new_stream_data.unsigned_span());
  // 写入的是明文算子，去掉原 /Filter 避免二次 Flate 解码。
  stream_dict->RemoveFor("Filter");
  stream_dict->RemoveFor("DecodeParms");

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetGotoDestination(FPDF_ANNOTATION annot, int page_index) {
  if (!annot || page_index < 0) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict) {
    return false;
  }

  // Verify this is a Link annotation
  ByteString subtype = pAnnotDict->GetNameFor(pdfium::annotation::kSubtype);
  if (subtype != "Link") {
    return false;
  }

  // Create the /A (Action) dictionary:
  //   /A << /S /GoTo /D [pageIndex /Fit] >>
  RetainPtr<CPDF_Dictionary> actionDict =
      pAnnotDict->SetNewFor<CPDF_Dictionary>("A");
  actionDict->SetNewFor<CPDF_Name>("S", "GoTo");

  // Create the /D (Destination) array: [pageIndex /Fit]
  RetainPtr<CPDF_Array> destArray = actionDict->SetNewFor<CPDF_Array>("D");
  destArray->AppendNew<CPDF_Number>(page_index);
  destArray->AppendNew<CPDF_Name>("Fit");

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetGotoDestinationXYZ(FPDF_ANNOTATION annot,
                                 int page_index,
                                 float x,
                                 float y,
                                 float zoom) {
  if (!annot || page_index < 0) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict) {
    return false;
  }

  // Verify this is a Link annotation
  ByteString subtype = pAnnotDict->GetNameFor(pdfium::annotation::kSubtype);
  if (subtype != "Link") {
    return false;
  }

  // Create the /A (Action) dictionary:
  //   /A << /S /GoTo /D [pageIndex /XYZ x y zoom] >>
  RetainPtr<CPDF_Dictionary> actionDict =
      pAnnotDict->SetNewFor<CPDF_Dictionary>("A");
  actionDict->SetNewFor<CPDF_Name>("S", "GoTo");

  // Create the /D (Destination) array: [pageIndex /XYZ x y zoom]
  RetainPtr<CPDF_Array> destArray = actionDict->SetNewFor<CPDF_Array>("D");
  destArray->AppendNew<CPDF_Number>(page_index);
  destArray->AppendNew<CPDF_Name>("XYZ");
  destArray->AppendNew<CPDF_Number>(x);
  destArray->AppendNew<CPDF_Number>(y);
  destArray->AppendNew<CPDF_Number>(zoom);

  return true;
}

namespace {

// ============================================================================
// Fill & Sign 支持：FreeText 文本注解 & 文本图章
// （参考 PDF Expert Fill & Sign 的文本/签名/勾叉/图章类型）
// ============================================================================

// 确保目录存在 AcroForm + /DR + 标准 Helvetica。FreeText 的 AP 生成
// （CPDF_GenerateAP::GenerateFreeTextAP）强依赖 /DR 字体资源，缺它直接返回
// false。与 fpdf_form_ext.cpp::GetOrCreateAcroForm 逻辑一致，两个扩展单元
// 各自独立链接，互不共享 static，故在此复制一份。
// 幂等：无论 AcroForm 之前是否已存在，都保证 /DR/Font 含 Helv 与全部 base14 变体，
// 这样 FreeText 改字型名(DA)后用对应变体重画，粗/斜体不至于回退丢失。
CPDF_Dictionary* GetOrCreateAcroFormDR(CPDF_Document* doc) {
  if (!doc) {
    return nullptr;
  }
  CPDF_Dictionary* root = doc->GetMutableRoot();
  if (!root) {
    return nullptr;
  }
  CPDF_Dictionary* acro = nullptr;
  if (auto existing = root->GetMutableDictFor("AcroForm")) {
    acro = existing.Get();
  } else {
    acro = doc->NewIndirect<CPDF_Dictionary>();
    root->SetNewFor<CPDF_Reference>("AcroForm", doc, acro->GetObjNum());
  }

  auto dr = acro->GetOrCreateDictFor("DR");
  auto font_dict = dr->GetOrCreateDictFor("Font");

  // 默认别名 Helv → Helvetica（保证既有 /DA `/Helv ... Tf` 命中）。
  if (!font_dict->KeyExist("Helv")) {
    auto helv = doc->NewIndirect<CPDF_Dictionary>();
    helv->SetNewFor<CPDF_Name>("Type", "Font");
    helv->SetNewFor<CPDF_Name>("Subtype", "Type1");
    helv->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
    font_dict->SetNewFor<CPDF_Reference>("Helv", doc, helv->GetObjNum());
  }

  // 全部 base14(含粗/斜变体)：key = 完整字型名，DA 里的名字须与其一致。
  static const char* kBase14Names[] = {
      "Helvetica",         "Helvetica-Bold",         "Helvetica-Oblique",
      "Helvetica-BoldOblique", "Times-Roman",        "Times-Bold",
      "Times-Italic",       "Times-BoldItalic",      "Courier",
      "Courier-Bold",       "Courier-Oblique",       "Courier-BoldOblique",
      "Symbol",             "ZapfDingbats",
  };
  for (const char* name : kBase14Names) {
    if (font_dict->KeyExist(name)) {
      continue;
    }
    auto f = doc->NewIndirect<CPDF_Dictionary>();
    f->SetNewFor<CPDF_Name>("Type", "Font");
    f->SetNewFor<CPDF_Name>("Subtype", "Type1");
    f->SetNewFor<CPDF_Name>("BaseFont", name);
    font_dict->SetNewFor<CPDF_Reference>(name, doc, f->GetObjNum());
  }

  if (!acro->KeyExist("DA")) {
    acro->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
  }
  return acro;
}

// 组装 FreeText 的 /DA：`/Helv <size> Tf <r> <g> <b> rg`。颜色为 0..1。
ByteString ComposeFreeTextDA(float size, float r, float g, float b) {
  if (size <= 0) {
    size = 12.0f;
  }
  return ByteString::Format("/Helv %g Tf %g %g %g rg", size, r, g, b);
}

// 只替换 /DA 里的字号，保留现有颜色。期望形如 `/Helv <size> Tf <r> <g> <b> rg`。
ByteString UpdateFreeTextFontSize(ByteString da, float newSize) {
  std::string s(da.c_str());
  auto tf = s.find(" Tf");
  if (tf == std::string::npos) {
    return da;
  }
  auto space = s.rfind(' ', tf - 1);
  size_t start = space == std::string::npos ? 0 : space + 1;
  char buf[64];
  snprintf(buf, sizeof(buf), "%g", newSize);
  s.replace(start, tf - start, buf);
  return ByteString(s.c_str());
}

// 把归一化 float 颜色写入 dict 数组键（先清空再写）。
void SetNormColor(CPDF_Dictionary* d, ByteStringView key, unsigned r, unsigned g,
                  unsigned b) {
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  RetainPtr<CPDF_Array> arr = d->GetMutableArrayFor(key);
  if (!arr) {
    arr = d->SetNewFor<CPDF_Array>(ByteString(key));
  }
  arr->Clear();
  arr->AppendNew<CPDF_Number>(r / 255.0f);
  arr->AppendNew<CPDF_Number>(g / 255.0f);
  arr->AppendNew<CPDF_Number>(b / 255.0f);
}

// 转义 PDF 字面量字符串里的 ( ) \。
std::string EscapePdfLiteral(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char ch : s) {
    if (ch == '(') {
      o += "\\(";
    } else if (ch == ')') {
      o += "\\)";
    } else if (ch == '\\') {
      o += "\\\\";
    } else {
      o += ch;
    }
  }
  return o;
}

// 生成文本图章外观：背景填充 + 可选边框 + 多行文本（按 /Contents 分隔、字号
// 自适应、按 /QLTextAlignment 对齐）。结果写入 /AP /N Form XObject。
// 样式全部从图章字典读取，供创建与 FormatPanel 改样式后重生成共用。
bool BuildTextStampAP(CPDF_Document* doc, CPDF_Dictionary* annot_dict) {
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  float w = rect.Width();
  float h = rect.Height();
  if (w < 0.000001f || h < 0.000001f) {
    return false;
  }

  ByteString bytes = annot_dict->GetUnicodeTextFor("Contents").ToUTF8();
  std::string utf8(bytes.c_str(), bytes.GetLength());
  std::vector<std::string> lines;
  {
    size_t s = 0;
    while (s < utf8.size()) {
      size_t e = utf8.find('\n', s);
      if (e == std::string::npos) {
        lines.push_back(utf8.substr(s));
        break;
      }
      lines.push_back(utf8.substr(s, e - s));
      s = e + 1;
    }
  }
  if (lines.empty()) {
    lines.push_back("");
  }

  float fontSize = annot_dict->GetFloatFor("QLFontSize");
  if (fontSize <= 0) {
    fontSize = 12.0f;
  }
  int align = static_cast<int>(annot_dict->GetIntegerFor("QLTextAlignment"));

  float tr = 0, tg = 0, tb = 0;
  if (RetainPtr<const CPDF_Array> c = annot_dict->GetArrayFor("C")) {
    if (c->size() >= 3) {
      tr = c->GetFloatAt(0);
      tg = c->GetFloatAt(1);
      tb = c->GetFloatAt(2);
    }
  }

  // 字号自适应：按最长行宽与总行高缩放到 bbox。
  size_t maxlen = 0;
  for (const auto& l : lines) {
    maxlen = std::max(maxlen, l.size());
  }
  float scale = 1.0f;
  float estW = maxlen * 0.5f * fontSize;
  if (estW > w) {
    scale = w / estW;
  }
  float fsize = fontSize * scale;
  float lineH = fsize * 1.2f;
  if (lineH * lines.size() > h) {
    float s2 = h / (lineH * lines.size());
    fsize *= s2;
    lineH *= s2;
  }
  float totalH = lineH * lines.size();

  std::string cmds;
  if (RetainPtr<const CPDF_Array> fill = annot_dict->GetArrayFor("QLFillColor")) {
    if (fill->size() >= 3) {
      char b[256];
      snprintf(b, sizeof(b), "q %g %g %g rg 0 0 %g %g re f Q ", fill->GetFloatAt(0),
               fill->GetFloatAt(1), fill->GetFloatAt(2), w, h);
      cmds += b;
    }
  }
  float bw = annot_dict->GetFloatFor("QLBorderWidth");
  if (bw > 0.0f) {
    if (RetainPtr<const CPDF_Array> bd = annot_dict->GetArrayFor("QLBorderColor")) {
      if (bd->size() >= 3) {
        char b[256];
        snprintf(b, sizeof(b), "q %g %g %g RG %g w 0.2 0.2 %g %g re S Q ",
                 bd->GetFloatAt(0), bd->GetFloatAt(1), bd->GetFloatAt(2), bw,
                 w - 0.4f, h - 0.4f);
        cmds += b;
      }
    }
  }

  // ✓(U+2713) / ✗(U+2717)：标准 Helvetica 无此字形，文本 Tj 会把 3 个 UTF-8 字节按字节映射成怪字符。
  // 与 PDF Expert 同机制：改用 ZapfDingbats 标准字形画单字符——'4'(0x34)=✔（PDF Expert 即用 4）、
  // '7'(0x37)=✗。识别对象是 Swift 传入的默认文本(U+2713/U+2717)，输出为 ZapfDingbats 字节。
  bool drewMark = false;
  float tyBase = (h + totalH) / 2.0f - fsize * 0.85f;
  if (lines.size() == 1) {
    const std::string& t = lines[0];
    bool isCheck = t.size() == 3 && (unsigned char)t[0] == 0xE2 &&
                   (unsigned char)t[1] == 0x9C && (unsigned char)t[2] == 0x93;
    bool isCross = t.size() == 3 && (unsigned char)t[0] == 0xE2 &&
                   (unsigned char)t[1] == 0x9C && (unsigned char)t[2] == 0x97;
    if (isCheck || isCross) {
      char byte = isCheck ? '4' : '7';
      float lw = fsize * 0.5f;
      float tx = 0.0f;
      if (align == 1) {
        tx = (w - lw) / 2.0f;
      } else if (align == 2) {
        tx = w - lw;
      }
      char b[256];
      snprintf(b, sizeof(b),
               "q BT /ZD %g Tf %g %g %g rg 1 0 0 1 %g %g Tm (%c) Tj ET Q ",
               fsize, tr, tg, tb, tx, tyBase, byte);
      cmds += b;
      drewMark = true;
    }
  }
  if (!drewMark) {
    {
      char hdr[128];
      snprintf(hdr, sizeof(hdr), "q BT /Helv %g Tf %g %g %g rg ", fsize, tr, tg, tb);
      cmds += hdr;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
      float lw = lines[i].size() * fsize * 0.5f;
      float tx = 0.0f;
      if (align == 1) {
        tx = (w - lw) / 2.0f;
      } else if (align == 2) {
        tx = w - lw;
      }
      char pos[128];
      snprintf(pos, sizeof(pos), "1 0 0 1 %g %g Tm ", tx, tyBase - i * lineH);
      cmds += pos;
      cmds += "(";
      cmds += EscapePdfLiteral(lines[i]);
      cmds += ") Tj ";
    }
  }
  cmds += "ET Q";

  // Form XObject：/Resources/Font/Helv = 标准 Helvetica。
  auto formDict = pdfium::MakeRetain<CPDF_Dictionary>();
  formDict->SetNewFor<CPDF_Name>("Type", "XObject");
  formDict->SetNewFor<CPDF_Name>("Subtype", "Form");
  formDict->SetRectFor("BBox", CFX_FloatRect(0, 0, w, h));
  auto res = formDict->SetNewFor<CPDF_Dictionary>("Resources");
  auto fonts = res->SetNewFor<CPDF_Dictionary>("Font");
  auto helv = fonts->SetNewFor<CPDF_Dictionary>("Helv");
  helv->SetNewFor<CPDF_Name>("Type", "Font");
  helv->SetNewFor<CPDF_Name>("Subtype", "Type1");
  helv->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
  // 对勾/叉（check/cross）用 ZapfDingbats 字形，同 PDF Expert。无 mark 时闲置无碍。
  auto zsig = fonts->SetNewFor<CPDF_Dictionary>("ZD");
  zsig->SetNewFor<CPDF_Name>("Type", "Font");
  zsig->SetNewFor<CPDF_Name>("Subtype", "Type1");
  zsig->SetNewFor<CPDF_Name>("BaseFont", "ZapfDingbats");

  DataVector<uint8_t> apData(cmds.begin(), cmds.end());
  auto formStream =
      doc->NewIndirect<CPDF_Stream>(std::move(apData), std::move(formDict));

  auto apDict = annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);
  apDict->SetNewFor<CPDF_Reference>("N", doc, formStream->GetObjNum());
  return true;
}

// PDF Expert 式图章外观：圆角矩形底色 + 描边 + Helvetica-Bold 粗体字。与 /SBApproved
// 同机制（圆角 path + 标准粗体字体），非同 Scratch 字形文本。圆角弧度 /QLCorner，
// 底色 /QLFillColor、描边 /QLBorderColor(+Width)、文字 /C、字号 /QLFontSize。
// 渐变底纹为增强项（当前纯色填充逼近）。
bool BuildExpertStampAP(CPDF_Document* doc, CPDF_Dictionary* annot_dict) {
  CFX_FloatRect rect = annot_dict->GetRectFor(pdfium::annotation::kRect);
  float w = rect.Width();
  float h = rect.Height();
  if (w < 0.000001f || h < 0.000001f) {
    return false;
  }

  std::string text(annot_dict->GetUnicodeTextFor("Contents").ToUTF8().c_str());
  std::vector<std::string> lines;
  {
    size_t s = 0;
    while (s < text.size()) {
      size_t e = text.find('\n', s);
      if (e == std::string::npos) {
        lines.push_back(text.substr(s));
        break;
      }
      lines.push_back(text.substr(s, e - s));
      s = e + 1;
    }
  }
  if (lines.empty()) {
    lines.push_back("");
  }

  float fsize = annot_dict->GetFloatFor("QLFontSize");
  if (fsize <= 0) {
    fsize = 12.0f;
  }
  float corner = annot_dict->GetFloatFor("QLCorner");

  auto readColor = [&](ByteStringView key, float* out) {
    if (RetainPtr<const CPDF_Array> a = annot_dict->GetArrayFor(key)) {
      out[0] = a->GetFloatAt(0);
      out[1] = a->GetFloatAt(1);
      out[2] = a->GetFloatAt(2);
    }
  };
  float tr[3] = {0, 0, 0};
  float fill[3] = {1, 1, 1};
  float stroke[3] = {0, 0, 0};
  readColor("C", tr);
  readColor("QLFillColor", fill);
  readColor("QLBorderColor", stroke);
  float strokeW = annot_dict->GetFloatFor("QLBorderWidth");

  // 边框整体内缩 strokeW：描边居中落在路径上会向矩形外溢出 0.5*strokeW，而批注 AP
  // 渲染被钳制到 /Rect，外溢的一半被裁，边框显细、圆角被切。与 StampThumbnailRenderer
  // 预览一致：fill/stroke 共用内缩框，边框全落在框内不被裁。
  float inset = strokeW > 0.0f ? strokeW : 0.0f;
  float innerW = w - 2.0f * inset;
  float innerH = h - 2.0f * inset;
  if (innerW < 1.0f || innerH < 1.0f) {   // 极小框：内缩失效，退回不缩
    inset = 0.0f;
    innerW = w;
    innerH = h;
  }
  if (corner > 0.5f * std::min(innerW, innerH)) {
    corner = 0.5f * std::min(innerW, innerH);
  }

  // 用真实 Helvetica-Bold 字形宽测每行宽度（与 /Hb 同字体），实现水平居中——旧的行宽
  // =字符数*0.5em 估计（大写约 0.72em/字）明显低估，把文字整体推右、不居中。
  RetainPtr<CPDF_Font> measure_font =
      CPDF_Font::GetStockFont(doc, "Helvetica-Bold");
  std::vector<float> lineW;
  lineW.reserve(lines.size());
  float maxW = 0.0f;
  for (const auto& l : lines) {
    float lw;
    if (measure_font) {
      lw = measure_font->GetStringWidth(ByteStringView(l.data(), l.size()))
               * fsize / 1000.0f;
    } else {
      lw = l.size() * fsize * 0.5f;
    }
    lineW.push_back(lw);
    maxW = std::max(maxW, lw);
  }
  // 字号自适应：按最长行实测宽缩放到内框。
  if (maxW > innerW) {
    float sc = innerW / maxW;
    fsize *= sc;
    for (auto& lw : lineW) {
      lw *= sc;
    }
  }
  float lineH = fsize * 1.2f;
  if (lineH * lines.size() > innerH) {
    float s2 = innerH / (lineH * lines.size());
    fsize *= s2;
    lineH *= s2;
    for (auto& lw : lineW) {
      lw *= s2;
    }
  }

  // 在 (bx,by,bw,bh) 内构建圆角矩形路径（可内缩，非固定 0,0 起点）。
  auto roundRect = [&](std::string& out, float bx, float by, float bw, float bh,
                       float r) {
    if (r <= 0) {
      char b[64];
      snprintf(b, sizeof(b), "%g %g %g %g re ", bx, by, bw, bh);
      out += b;
      return;
    }
    char b[320];
    snprintf(b, sizeof(b),
             "%g %g m %g %g l %g %g %g %g v "
             "%g %g l %g %g %g %g v "
             "%g %g l %g %g %g %g v "
             "%g %g l %g %g %g %g v h ",
             bx + r, by,   bx + bw - r, by,   bx + bw, by,   bx + bw, by + r,
             bx + bw, by + bh - r,   bx + bw, by + bh,   bx + bw - r, by + bh,
             bx + r, by + bh,   bx, by + bh,   bx, by + bh - r,
             bx, by + r,   bx, by,   bx + r, by);
    out += b;
  };

  std::string cmds;
  {
    char b[96];
    snprintf(b, sizeof(b), "q %g %g %g rg ", fill[0], fill[1], fill[2]);
    cmds += b;
    std::string p;
    roundRect(p, inset, inset, innerW, innerH, corner);
    cmds += p;
    cmds += "f Q ";
  }
  if (strokeW > 0) {
    char b[96];
    snprintf(b, sizeof(b), "q %g %g %g RG %g w 1 J 1 j ", stroke[0], stroke[1],
             stroke[2], strokeW);
    cmds += b;
    std::string p;
    roundRect(p, inset, inset, innerW, innerH, corner);
    cmds += p;
    cmds += "S Q ";
  }
  {
    char hdr[96];
    // TyBaseline：垂直居中粗体大写（cap≈0.72em 中心落在内框中线），与预览一致。
    snprintf(hdr, sizeof(hdr), "q BT /Hb %g Tf %g %g %g rg ", fsize, tr[0],
             tr[1], tr[2]);
    cmds += hdr;
    float tyBase = inset + innerH / 2.0f - fsize * 0.36f;
    for (size_t i = 0; i < lines.size(); ++i) {
      float tx = inset + (innerW - lineW[i]) / 2.0f;   // 水平居中：实测行宽
      char pos[128];
      snprintf(pos, sizeof(pos), "1 0 0 1 %g %g Tm ", tx, tyBase - i * lineH);
      cmds += pos;
      cmds += "(";
      cmds += EscapePdfLiteral(lines[i]);
      cmds += ") Tj ";
    }
    cmds += "ET Q";
  }

  auto formDict = pdfium::MakeRetain<CPDF_Dictionary>();
  formDict->SetNewFor<CPDF_Name>("Type", "XObject");
  formDict->SetNewFor<CPDF_Name>("Subtype", "Form");
  formDict->SetRectFor("BBox", CFX_FloatRect(0, 0, w, h));
  auto res = formDict->SetNewFor<CPDF_Dictionary>("Resources");
  auto fonts = res->SetNewFor<CPDF_Dictionary>("Font");
  auto hb = fonts->SetNewFor<CPDF_Dictionary>("Hb");
  hb->SetNewFor<CPDF_Name>("Type", "Font");
  hb->SetNewFor<CPDF_Name>("Subtype", "Type1");
  hb->SetNewFor<CPDF_Name>("BaseFont", "Helvetica-Bold");

  DataVector<uint8_t> apData(cmds.begin(), cmds.end());
  auto formStream =
      doc->NewIndirect<CPDF_Stream>(std::move(apData), std::move(formDict));

  auto apDict = annot_dict->GetOrCreateDictFor(pdfium::annotation::kAP);
  apDict->SetNewFor<CPDF_Reference>("N", doc, formStream->GetObjNum());
  return true;
}

}  // namespace

// 创建 FreeText 文本注解（对应 PDF Expert 的文本/日期）。rect 为 PDF 用户空间
// （y-up）。样式随后仍可经 FPDFAnnot_SetFontSize / SetTextAlignment / SetColorToDict
// 修改，再调 FPDFAnnot_GenerateFreeTextAP 重画。
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateFreeTextAnnot(FPDF_PAGE page,
                         const FS_RECTF* rect,
                         FPDF_WIDESTRING text,
                         float font_size,
                         unsigned int colorR,
                         unsigned int colorG,
                         unsigned int colorB,
                         unsigned int colorA,
                         int alignment) {
  if (!page || !rect || !text) {
    return nullptr;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return nullptr;
  }
  CPDF_Document* doc = pPage->GetDocument();
  if (!doc) {
    return nullptr;
  }
  // GenerateFreeTextAP 依赖 AcroForm/DR，先兜底。
  GetOrCreateAcroFormDR(doc);

  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
  if (!annot) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return annot;
  }
  d->SetRectFor(pdfium::annotation::kRect,
                CFX_FloatRect(rect->left, rect->bottom, rect->right, rect->top));
  if (alignment < 0) {
    alignment = 0;
  } else if (alignment > 2) {
    alignment = 2;
  }
  d->SetNewFor<CPDF_Number>("Q", alignment);
  d->SetNewFor<CPDF_Number>("F", 4);
  if (colorA <= 255) {
    d->SetNewFor<CPDF_Number>("CA", colorA / 255.0f);
  }
  d->SetNewFor<CPDF_String>(
      "DA", ComposeFreeTextDA(font_size, colorR / 255.0f, colorG / 255.0f,
                              colorB / 255.0f));
  d->SetNewFor<CPDF_String>("Contents",
                            WideStringFromFPDFWideString(text).AsStringView());
  // fill-sign 文本框默认无边框：显式 /BS /W 0。缺省时 GenerateFreeTextAP 会按
  // 默认 border width=1 派发黑框（GetBorderStyleInfo 无 /BS → width 默认 1）。
  auto bs = d->SetNewFor<CPDF_Dictionary>("BS");
  bs->SetNewFor<CPDF_Number>("W", 0);

  CPDF_GenerateAP::GenerateAnnotAP(doc, d.Get(), CPDF_Annot::Subtype::FREETEXT);
  return annot;
}

// 文本/日期样式已变（DA、Q、Contents、C）后重生成 FreeText 外观。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateFreeTextAP(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  CPDF_AnnotContext* ctx = GetAnnotContext(annot);
  if (!ctx) {
    return false;
  }
  CPDF_Document* doc = ctx->GetPage() ? ctx->GetPage()->GetDocument() : nullptr;
  if (!doc) {
    return false;
  }
  GetOrCreateAcroFormDR(doc);
  return CPDF_GenerateAP::GenerateAnnotAP(doc, d.Get(),
                                          CPDF_Annot::Subtype::FREETEXT);
}

// 创建文本图章（对应 PDF Expert 的签名 / 勾 / 叉 / "APPROVED" 图章等）。
// 文本色用 /C 存；背景/边框/对齐/字号存扩展键（/QLFillColor /QLFillAlpha
// /QLBorderColor /QLBorderWidth /QLTextAlignment /QLFontSize），后续 FormatPanel
// 改样式后调 FPDFAnnot_GenerateTextStampAP 重画。
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateTextStampAnnot(FPDF_PAGE page,
                          const FS_RECTF* rect,
                          FPDF_WIDESTRING text,
                          float font_size,
                          unsigned int textR,
                          unsigned int textG,
                          unsigned int textB,
                          unsigned int textA,
                          FPDF_BOOL has_background,
                          unsigned int bgR,
                          unsigned int bgG,
                          unsigned int bgB,
                          unsigned int bgA,
                          unsigned int borderR,
                          unsigned int borderG,
                          unsigned int borderB,
                          float border_width) {
  if (!page || !rect || !text) {
    return nullptr;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return nullptr;
  }
  CPDF_Document* doc = pPage->GetDocument();
  if (!doc) {
    return nullptr;
  }
  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP);
  if (!annot) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return annot;
  }
  d->SetRectFor(pdfium::annotation::kRect,
                CFX_FloatRect(rect->left, rect->bottom, rect->right, rect->top));
  d->SetNewFor<CPDF_Number>("F", 4);
  d->SetNewFor<CPDF_String>("Contents",
                            WideStringFromFPDFWideString(text).AsStringView());
  d->SetNewFor<CPDF_Number>("QLFontSize", font_size > 0 ? font_size : 12.0f);
  d->SetNewFor<CPDF_Number>("QLTextAlignment", 1);
  if (textA <= 255) {
    d->SetNewFor<CPDF_Number>("CA", textA / 255.0f);
  }
  SetNormColor(d.Get(), "C", textR, textG, textB);
  if (has_background && bgA > 0) {
    SetNormColor(d.Get(), "QLFillColor", bgR, bgG, bgB);
    d->SetNewFor<CPDF_Number>("QLFillAlpha", bgA / 255.0f);
  }
  if (border_width > 0.0f) {
    SetNormColor(d.Get(), "QLBorderColor", borderR, borderG, borderB);
    d->SetNewFor<CPDF_Number>("QLBorderWidth", border_width);
  }
  BuildTextStampAP(doc, d.Get());
  return annot;
}

// 图章文本/样式已变后重生成外观。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateTextStampAP(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  CPDF_AnnotContext* ctx = GetAnnotContext(annot);
  if (!ctx) {
    return false;
  }
  CPDF_Document* doc = ctx->GetPage() ? ctx->GetPage()->GetDocument() : nullptr;
  if (!doc) {
    return false;
  }
  return BuildTextStampAP(doc, d.Get());
}

// PDF Expert 式矢量图章（圆角矩形底色 + Helvetica-Bold 粗体字）。rect 为 PDF 用户空间。
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateExpertStampAnnot(FPDF_PAGE page,
                            const FS_RECTF* rect,
                            FPDF_WIDESTRING text,
                            float font_size,
                            unsigned int textR,
                            unsigned int textG,
                            unsigned int textB,
                            unsigned int fillR,
                            unsigned int fillG,
                            unsigned int fillB,
                            unsigned int strokeR,
                            unsigned int strokeG,
                            unsigned int strokeB,
                            float corner_radius) {
  if (!page || !rect || !text) {
    return nullptr;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return nullptr;
  }
  CPDF_Document* doc = pPage->GetDocument();
  if (!doc) {
    return nullptr;
  }
  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP);
  if (!annot) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return annot;
  }
  d->SetRectFor(pdfium::annotation::kRect,
                CFX_FloatRect(rect->left, rect->bottom, rect->right, rect->top));
  d->SetNewFor<CPDF_Number>("F", 4);
  d->SetNewFor<CPDF_String>("Contents",
                            WideStringFromFPDFWideString(text).AsStringView());
  d->SetNewFor<CPDF_Number>("QLFontSize", font_size > 0 ? font_size : 12.0f);
  d->SetNewFor<CPDF_Number>("QLCorner", corner_radius);
  d->SetNewFor<CPDF_Number>("QLBorderWidth", 1.0f);
  d->SetNewFor<CPDF_Name>("Name", "/Stamp");
  SetNormColor(d.Get(), "C", textR, textG, textB);
  SetNormColor(d.Get(), "QLFillColor", fillR, fillG, fillB);
  SetNormColor(d.Get(), "QLBorderColor", strokeR, strokeG, strokeB);
  BuildExpertStampAP(doc, d.Get());
  return annot;
}

// PDF Expert 式图章样式已变后重生成外观。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateExpertStampAP(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  CPDF_AnnotContext* ctx = GetAnnotContext(annot);
  if (!ctx) {
    return false;
  }
  CPDF_Document* doc = ctx->GetPage() ? ctx->GetPage()->GetDocument() : nullptr;
  if (!doc) {
    return false;
  }
  return BuildExpertStampAP(doc, d.Get());
}

// 改字号：FreeText 改 /DA 字号，图章改 /QLFontSize。不重画，由调用方再 Generate。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFontSize(FPDF_ANNOTATION annot, float size) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d || size <= 0) {
    return false;
  }
  if (d->GetNameFor(pdfium::annotation::kSubtype) == "FreeText") {
    RetainPtr<const CPDF_String> da = d->GetStringFor("DA");
    ByteString daStr = da ? da->GetString() : ByteString("");
    d->SetNewFor<CPDF_String>("DA", UpdateFreeTextFontSize(daStr, size));
  } else {
    d->SetNewFor<CPDF_Number>("QLFontSize", size);
  }
  return true;
}

// 改对齐：FreeText 用 /Q，图章用 /QLTextAlignment。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetTextAlignment(FPDF_ANNOTATION annot, int q) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  if (q < 0) {
    q = 0;
  } else if (q > 2) {
    q = 2;
  }
  if (d->GetNameFor(pdfium::annotation::kSubtype) == "FreeText") {
    d->SetNewFor<CPDF_Number>("Q", q);
  } else {
    d->SetNewFor<CPDF_Number>("QLTextAlignment", q);
  }
  return true;
}

// 改背景：FreeText 用 /C（GenerateFreeTextAP 读它铺背景），图章用 /QLFillColor。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetBackground(FPDF_ANNOTATION annot,
                        unsigned int R,
                        unsigned int G,
                        unsigned int B,
                        unsigned int A) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  if (d->GetNameFor(pdfium::annotation::kSubtype) == "FreeText") {
    // FreeText 背景即 /C；透明度经 /CA。
    if (A <= 255) {
      d->SetNewFor<CPDF_Number>("CA", A / 255.0f);
    }
    SetNormColor(d.Get(), "C", R, G, B);
  } else {
    if (A > 0) {
      SetNormColor(d.Get(), "QLFillColor", R, G, B);
      d->SetNewFor<CPDF_Number>("QLFillAlpha", A / 255.0f);
    } else {
      d->RemoveFor("QLFillColor");
      d->RemoveFor("QLFillAlpha");
    }
  }
  return true;
}

// 改边框：图章用扩展键（/QLBorderColor + /QLBorderWidth）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetStampBorder(FPDF_ANNOTATION annot,
                         unsigned int R,
                         unsigned int G,
                         unsigned int B,
                         float width) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  if (width > 0.0f) {
    SetNormColor(d.Get(), "QLBorderColor", R, G, B);
    d->SetNewFor<CPDF_Number>("QLBorderWidth", width);
  } else {
    d->RemoveFor("QLBorderColor");
    d->RemoveFor("QLBorderWidth");
  }
  return true;
}

// 统一按 subtype 重生成外观（FreeText/Stamp），供 FormatPanel 改完样式后只调一个。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateAP(FPDF_ANNOTATION annot) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  ByteString st = d->GetNameFor(pdfium::annotation::kSubtype);
  if (st == "FreeText") {
    return FPDFAnnot_GenerateFreeTextAP(annot);
  }
  if (st == "Stamp") {
    return FPDFAnnot_GenerateTextStampAP(annot);
  }
  return false;
}

// 把已嵌入的字体对象注册进 AcroForm 的 /DR/Font/<font_name>。字体对象由
// FPDFFont_CreateType0FromData/FromSubset 创建，已是间接对象，此处仅挂引用。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RegisterFontToAcroFormDR(FPDF_DOCUMENT document,
                                   FPDF_FONT font,
                                   const char* font_name) {
  if (!document || !font || !font_name || !*font_name) {
    return false;
  }
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
  if (!doc || !pFont) {
    return false;
  }
  CPDF_Dictionary* acro = GetOrCreateAcroFormDR(doc);
  if (!acro) {
    return false;
  }
  auto dr = acro->GetMutableDictFor("DR");
  auto font_dict = dr->GetMutableDictFor("Font");
  if (!font_dict) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> font_obj = pFont->GetFontDict();
  if (!font_obj || font_obj->GetObjNum() == 0) {
    return false;
  }
  font_dict->SetNewFor<CPDF_Reference>(ByteString(font_name), doc,
                                       font_obj->GetObjNum());
  return true;
}

// 测 FreeText 批注的可编辑光标（caret）几何。复刻 GenerateFreeTextAP 的排版块
// （cpdf_generateap.cpp:1097-1109）：CPDF_DefaultAppearance 解析 /DA，CPVT_FontMap +
// CPVT_VariableText 排全文与前缀，返回的 content_rect 已是 annot 用户空间坐标
// （CPVT InToOut 锚定到绝对 /Rect plate），垂直居中 offset 另行补齐。
// caret = 全文内容左缘 + 前缀真实宽度（Q=0、单行/首行内像素级精确）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_MeasureFreeTextText(FPDF_ANNOTATION annot,
                              FPDF_WIDESTRING text,
                              int caretUTF16Len,
                              double* outCaretX,
                              double* outCaretBottomY,
                              double* outCaretTopY,
                              FPDF_BOOL* outAccurate) {
  if (outCaretX) *outCaretX = 0.0;
  if (outCaretBottomY) *outCaretBottomY = 0.0;
  if (outCaretTopY) *outCaretTopY = 0.0;
  if (outAccurate) *outAccurate = 0;
  if (!annot || !text) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  CPDF_AnnotContext* ctx = GetAnnotContext(annot);
  if (!d || !ctx) {
    return false;
  }
  CPDF_Document* doc = ctx->GetPage() ? ctx->GetPage()->GetDocument() : nullptr;
  if (!doc) {
    return false;
  }

  CPDF_Dictionary* acro = GetOrCreateAcroFormDR(doc);
  if (!acro) {
    return false;
  }
  CPDF_DefaultAppearance da(d.Get(), acro);
  std::optional<CPDF_DefaultAppearance::FontNameAndSize> fn = da.GetFont();
  if (!fn) {
    return false;
  }
  ByteString font_name = fn->name;
  float font_size = fn->size;

  // 同 GenerateFreeTextAP：从 /AcroForm/DR/Font 取字体，缺则回退 Type1 WinAnsi。
  auto dr = acro->GetMutableDictFor("DR");
  auto dr_font = dr->GetMutableDictFor("Font");
  RetainPtr<CPDF_Dictionary> font_dict =
      dr_font->GetMutableDictFor(font_name.AsStringView());
  if (!font_dict) {
    font_dict = doc->NewIndirect<CPDF_Dictionary>();
    font_dict->SetNewFor<CPDF_Name>("Type", "Font");
    font_dict->SetNewFor<CPDF_Name>("Subtype", "Type1");
    font_dict->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
    font_dict->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
    dr_font->SetNewFor<CPDF_Reference>(font_name, doc,
                                       font_dict->GetObjNum());
  }
  // 解析 /DA 得到的字体对象（含 AcroForm DR 里注册的嵌入子集），供 CPVT_FontMap 排版，与 /AP 同源。
  RetainPtr<CPDF_Font> font =
      CPDF_DocPageData::FromDocument(doc)->GetFont(font_dict);

  // body_rect = /Rect 四周各收缩 border width（默认 1；B/I 加倍）。
  CFX_FloatRect rect = d->GetRectFor(pdfium::annotation::kRect);
  float border_w = 1.0f;
  if (RetainPtr<const CPDF_Dictionary> bs = d->GetDictFor("BS")) {
    if (bs->KeyExist("W")) {
      border_w = bs->GetFloatFor("W");
    }
    const ByteString s = bs->GetByteStringFor("S");
    if (s.GetLength() && (s[0] == 'B' || s[0] == 'I')) {
      border_w *= 2.0f;
    }
  }
  CFX_FloatRect bg = rect;
  bg.Deflate(border_w / 2.0f, border_w / 2.0f);
  CFX_FloatRect body_rect = bg;
  body_rect.Deflate(border_w / 2.0f, border_w / 2.0f);

  CPVT_FontMap map(doc, nullptr, font, font_name);
  CPVT_VariableText::Provider provider(&map);
  CPVT_VariableText vt(&provider);
  vt.SetPlateRect(body_rect);
  vt.SetAlignment(CPVT_VariableText::ToAlignment(d->GetIntegerFor("Q")));
  if (font_size == 0.0f) {
    vt.SetAutoFontSize(true);
  } else {
    vt.SetFontSize(font_size);
  }
  vt.Initialize();

  size_t count = 0;
  const unsigned short* stop0 =
      reinterpret_cast<const unsigned short*>(text);
  while (stop0[count]) {
    ++count;
  }
  pdfium::span<const uint8_t> full_bytes(
      reinterpret_cast<const uint8_t*>(stop0), count * sizeof(unsigned short));
  vt.SetText(WideString::FromUTF16LE(full_bytes));
  vt.RearrangeAll();
  CFX_FloatRect fullC = vt.GetContentRect();
  const float offY = (fullC.Height() - body_rect.Height()) / 2.0f;

  float prefixW = 0.0f;
  const int pn = std::min(caretUTF16Len, static_cast<int>(count));
  if (pn > 0) {
    pdfium::span<const uint8_t> prefix_bytes(
        reinterpret_cast<const uint8_t*>(stop0),
        static_cast<size_t>(pn) * sizeof(unsigned short));
    vt.SetText(WideString::FromUTF16LE(prefix_bytes));
    vt.RearrangeAll();
    prefixW = vt.GetContentRect().Width();
  }

  if (outCaretX) *outCaretX = fullC.left + prefixW;
  if (outCaretBottomY) *outCaretBottomY = fullC.bottom + offY;
  if (outCaretTopY) *outCaretTopY = fullC.top + offY;
  if (outAccurate) *outAccurate = 1;
  return true;
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
FPDFAnnot_GetObjNum(FPDF_ANNOTATION annot) {
  if (!annot) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return 0;
  }
  return d->GetObjNum();
}

namespace {

FPDFExt_LineEnding LineEndingFromName(const RetainPtr<const CPDF_Object>& obj) {
  if (!obj || !obj->AsName()) {
    return FPDFExt_LineEnding_None;
  }
  ByteString s = obj->AsName()->GetString();
  if (s == "OpenArrow") return FPDFExt_LineEnding_OpenArrow;
  if (s == "ClosedArrow") return FPDFExt_LineEnding_ClosedArrow;
  if (s == "Square") return FPDFExt_LineEnding_Square;
  if (s == "Circle") return FPDFExt_LineEnding_Circle;
  if (s == "Diamond") return FPDFExt_LineEnding_Diamond;
  return FPDFExt_LineEnding_None;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetLineEndingStyle(FPDF_ANNOTATION annot,
                             FPDFExt_LineEnding* start_style,
                             FPDFExt_LineEnding* end_style) {
  if (!annot || !start_style || !end_style) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  RetainPtr<const CPDF_Array> le = d->GetArrayFor("LE");
  if (!le || le->size() < 2) {
    return false;
  }
  *start_style = LineEndingFromName(le->GetDirectObjectAt(0));
  *end_style = LineEndingFromName(le->GetDirectObjectAt(1));
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetLine(FPDF_ANNOTATION annot, FS_POINTF start, FS_POINTF end) {
  if (!annot) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  RetainPtr<CPDF_Array> l = d->SetNewFor<CPDF_Array>("L");
  l->AppendNew<CPDF_Number>(start.x);
  l->AppendNew<CPDF_Number>(start.y);
  l->AppendNew<CPDF_Number>(end.x);
  l->AppendNew<CPDF_Number>(end.y);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetBorderStyle(FPDF_ANNOTATION annot,
                         FPDFExt_BorderStyle* border_style,
                         float* line_width,
                         float* dash_array,
                         int* dash_count,
                         int* line_cap,
                         int* line_join) {
  if (!annot || !border_style || !line_width || !dash_count || !line_cap ||
      !line_join) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }

  *border_style = FPDFExt_BorderStyle_Solid;
  *line_width = 1.0f;
  *line_cap = 1;   // Round（PDF 标注边框默认圆头帽）
  *line_join = 1;  // Round
  // 调用方用 *dash_count 携带缓冲容量（dash_array 非空时）；须在清零前缓存，否则容量被吞。
  const int kDashCapacity = *dash_count;
  *dash_count = 0;

  RetainPtr<const CPDF_Dictionary> bs = d->GetDictFor("BS");
  if (bs) {
    // 某些生产者 /S 存成 "/D"（带 / 前缀的 Name），故两种形态都判。
    const ByteString s = bs->GetByteStringFor("S");
    const bool is_dashed = (s == "D") || (s == "/D");
    const bool is_none = (s == "N") || (s == "/N");
    const bool is_beveled = (s == "B") || (s == "/B");
    const bool is_inset = (s == "I") || (s == "/I");
    const bool is_underline = (s == "U") || (s == "/U");
    const bool is_solid = (s == "S") || (s == "/S");
    const bool is_cloud = (s == "C") || (s == "/C");
    // /S 缺省即 /S (Solid)。
    if (is_dashed) {
      *border_style = FPDFExt_BorderStyle_Dashed;
    } else if (is_none) {
      *border_style = FPDFExt_BorderStyle_None;
    } else if (is_beveled) {
      *border_style = FPDFExt_BorderStyle_Beveled;
    } else if (is_inset) {
      *border_style = FPDFExt_BorderStyle_Inset;
    } else if (is_underline) {
      *border_style = FPDFExt_BorderStyle_Underline;
    } else if (is_solid) {
      *border_style = FPDFExt_BorderStyle_Solid;
    } else if (is_cloud) {
      *border_style = FPDFExt_BorderStyle_Cloud;
    }

    // /BS /W 线宽。
    if (bs->KeyExist("W")) {
      *line_width = bs->GetFloatFor("W");
    }

    // /BS /LC 线帽、/LI 线接合（0 Butt/Miter, 1 Round, 2 Square/Bevel）。
    if (bs->KeyExist("LC")) {
      *line_cap = static_cast<int>(bs->GetIntegerFor("LC"));
    }
    if (bs->KeyExist("LI")) {
      *line_join = static_cast<int>(bs->GetIntegerFor("LI"));
    }

    // /BS /D 虚线序列：仅 /S == D 时有效。
    if (*border_style == FPDFExt_BorderStyle_Dashed) {
      RetainPtr<const CPDF_Array> dsh = bs->GetArrayFor("D");
      if (dsh && !dsh->IsEmpty()) {
        size_t n = dsh->size();
        if (dash_array) {
          size_t out = std::min<size_t>(n, static_cast<size_t>(kDashCapacity));
          for (size_t i = 0; i < out; ++i) {
            dash_array[i] = dsh->GetFloatAt(static_cast<size_t>(i));
          }
          *dash_count = static_cast<int>(out);
        } else {
          *dash_count = static_cast<int>(n);
        }
      }
    }
  } else {
    // 无 /BS：回退旧式 /Border。[H V W] 的第三个元素即线宽，缺省 1.0。
    RetainPtr<const CPDF_Array> border = d->GetArrayFor("Border");
    if (border && border->size() >= 3) {
      *line_width = border->GetFloatAt(2);
    }
  }

  return true;
}

// 写批注描边样式 /BS：/S 名称 + /D 虚线序列（实线时移除 /D），保留既有 /W 线宽。
// style_code 沿用 FPDFExt_BorderStyle（0 Solid, 1 Dashed, 2 None, ...）。dash_count<=0 或
// 实线时清 /D。供线型下拉持久化（App 矢量化 layer 读 /BS /S + /D）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetBorderStyle(FPDF_ANNOTATION annot,
                         int style_code,
                         const float* dash_array,
                         int dash_count) {
  RetainPtr<CPDF_Dictionary> d = GetMutableAnnotDictSafe(annot);
  if (!d) {
    return false;
  }
  const char* sname = nullptr;
  switch (style_code) {
    case 0: sname = "S"; break;    // Solid
    case 1: sname = "D"; break;    // Dashed
    case 2: sname = "N"; break;    // None
    case 3: sname = "B"; break;    // Beveled
    case 4: sname = "I"; break;    // Inset
    case 5: sname = "U"; break;    // Underline
    case 6: sname = "C"; break;    // Cloud
    default: return false;
  }
  RetainPtr<CPDF_Dictionary> bs = d->GetOrCreateDictFor("BS");
  bs->SetNewFor<CPDF_Name>("S", sname);
  if (style_code == 1 && dash_array && dash_count > 0) {
    CPDF_Array* arr = bs->SetNewFor<CPDF_Array>("D");
    for (int i = 0; i < dash_count; ++i) {
      arr->AppendNew<CPDF_Number>(dash_array[i]);
    }
  } else {
    bs->RemoveFor("D");
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAppearanceWithBBox(FPDF_ANNOTATION annot,
                                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                float bbox_width,
                                float bbox_height,
                                FPDF_WIDESTRING value,
                                FPDF_BYTESTRING blendMode) {
  if (!annot || !value || bbox_width <= 0.0f || bbox_height <= 0.0f) {
    return false;
  }
  if (appearanceMode < 0 || appearanceMode >= FPDF_ANNOT_APPEARANCEMODE_COUNT) {
    return false;
  }

  static constexpr auto kModeKeyForMode =
      std::to_array<const char*>({"N", "R", "D"});
  static_assert(kModeKeyForMode.size() == FPDF_ANNOT_APPEARANCEMODE_COUNT,
                "length of kModeKeyForMode should be equal to "
                "FPDF_ANNOT_APPEARANCEMODE_COUNT");

  RetainPtr<CPDF_Dictionary> pAnnotDict = GetMutableAnnotDictSafe(annot);
  if (!pAnnotDict) {
    return false;
  }
  const char* mode_key = kModeKeyForMode[appearanceMode];
  RetainPtr<CPDF_Dictionary> ap_dict =
      pAnnotDict->GetMutableDictFor(pdfium::annotation::kAP);

  CPDF_AnnotContext* pAnnotContext = GetAnnotContext(annot);
  if (!pAnnotContext) {
    return false;
  }
  IPDF_Page* page = pAnnotContext->GetPage();
  if (!page) {
    return false;
  }
  CPDF_Document* doc = page->GetDocument();
  if (!doc) {
    return false;
  }

  // BBox 固定为调用方给定的局部尺寸 [0, 0, w, h]，内容须为局部坐标。
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  stream_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kType, "XObject");
  stream_dict->SetNewFor<CPDF_Name>(pdfium::annotation::kSubtype, "Form");
  RetainPtr<CPDF_Array> bbox =
      stream_dict->SetNewFor<CPDF_Array>("BBox");
  bbox->AppendNew<CPDF_Number>(0);
  bbox->AppendNew<CPDF_Number>(0);
  bbox->AppendNew<CPDF_Number>(bbox_width);
  bbox->AppendNew<CPDF_Number>(bbox_height);
  stream_dict->SetFor("Resources", SetExtGStateInResourceDict(doc, pAnnotDict.Get(), blendMode));

  ByteString new_stream_data = PDF_EncodeText(
      UNSAFE_BUFFERS(WideStringFromFPDFWideString(value).AsStringView()));
  auto new_stream = doc->NewIndirect<CPDF_Stream>(std::move(stream_dict));
  new_stream->SetData(new_stream_data.unsigned_span());

  if (!ap_dict) {
    ap_dict = pAnnotDict->SetNewFor<CPDF_Dictionary>(pdfium::annotation::kAP);
  }
  ap_dict->SetNewFor<CPDF_Reference>(mode_key, doc, new_stream->GetObjNum());
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// AP 外观流矢量几何提取：完全复用 PDFium 内容解释器，逐算子还原批注矢量渲染。
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// 取批注所属页的 /Resources（AP 内 /CS1、/Font 等名称可能定义在页面级）。
RetainPtr<CPDF_Dictionary> GetAnnotPageResources(CPDF_Dictionary* annot_dict) {
  RetainPtr<CPDF_Dictionary> page = annot_dict->GetMutableDictFor("P");
  return page ? page->GetMutableDictFor("Resources") : nullptr;
}

// 把 CPDF_Path 的点序列压平成 {ops, points}。坐标已由 matrix 从内容流空间变换到
// form 本地空间。算子编码与内容流 m/l/c/h 一一对应；c 连续 3 个 kBezier 一组。
struct PathFlattener {
  std::vector<int> ops;
  std::vector<float> points;

  void MoveTo(float x, float y, bool close) {
    ops.push_back(FPDFExt_PathOp_Move);
    points.push_back(x);
    points.push_back(y);
    if (close) Close();
  }
  void LineTo(float x, float y, bool close) {
    ops.push_back(FPDFExt_PathOp_Line);
    points.push_back(x);
    points.push_back(y);
    if (close) Close();
  }
  void CurveTo(float x1, float y1, float x2, float y2, float x, float y,
               bool close) {
    ops.push_back(FPDFExt_PathOp_Curve);
    points.push_back(x1);
    points.push_back(y1);
    points.push_back(x2);
    points.push_back(y2);
    points.push_back(x);
    points.push_back(y);
    if (close) Close();
  }
  void Close() { ops.push_back(FPDFExt_PathOp_Close); }
};

void FlattenPath(const CPDF_Path& path,
                 const CFX_Matrix& matrix,
                 PathFlattener* flat) {
  const float a = matrix.a, b = matrix.b, c = matrix.c;
  const float d = matrix.d, e = matrix.e, f = matrix.f;
  const auto& pts = path.GetPoints();
  const size_t n = pts.size();
  size_t i = 0;
  while (i < n) {
    const CFX_Path::Point& p = pts[i];
    const float x = a * p.point_.x + c * p.point_.y + e;
    const float y = b * p.point_.x + d * p.point_.y + f;
    switch (p.type_) {
      case CFX_Path::Point::Type::kMove:
        flat->MoveTo(x, y, p.close_figure_);
        ++i;
        break;
      case CFX_Path::Point::Type::kLine:
        flat->LineTo(x, y, p.close_figure_);
        ++i;
        break;
      case CFX_Path::Point::Type::kBezier: {
        // 连续 kBezier 点每 3 个一组（一个 c 算子）。close 落在组末（h 闭合）。
        float bx[6] = {0};
        bool bclose = false;
        int cnt = 0;
        while (i < n && pts[i].type_ == CFX_Path::Point::Type::kBezier) {
          const CFX_Path::Point& q = pts[i];
          const float qx = a * q.point_.x + c * q.point_.y + e;
          const float qy = b * q.point_.x + d * q.point_.y + f;
          bx[cnt * 2] = qx;
          bx[cnt * 2 + 1] = qy;
          bclose = q.close_figure_;
          ++cnt;
          ++i;
          if (cnt == 3) {
            flat->CurveTo(bx[0], bx[1], bx[2], bx[3], bx[4], bx[5], bclose);
            cnt = 0;
            bclose = false;
          }
        }
      } break;
    }
  }
}

// 颜色解析：把 PathObject 的描边/填充色从任意颜色空间解析成 sRGB 0..1。
bool ResolveColorRGB(const CPDF_Color* color, float rgb[3]) {
  if (!color || color->IsNull()) {
    return false;
  }
  auto value = color->GetRGB();
  if (!value.has_value()) {
    return false;
  }
  rgb[0] = value->red;
  rgb[1] = value->green;
  rgb[2] = value->blue;
  return true;
}

}  // namespace

FPDF_EXPORT FPDFExt_ShapeAppearance* FPDF_CALLCONV
FPDFExt_ExtractShapeAppearance(FPDF_ANNOTATION annot, FPDF_DOCUMENT document) {
  if (!annot || !document) {
    return nullptr;
  }
  RetainPtr<CPDF_Dictionary> annot_dict = GetMutableAnnotDictSafe(annot);
  if (!annot_dict) {
    return nullptr;
  }
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return nullptr;
  }

  RetainPtr<CPDF_Stream> stream =
      GetAnnotAP(annot_dict.Get(), CPDF_Annot::AppearanceMode::kNormal);
  if (!stream) {
    // 无 /AP /N：App 侧回退按 key 生成外观。
    return nullptr;
  }

  RetainPtr<CPDF_Dictionary> page_resources =
      GetAnnotPageResources(annot_dict.Get());
  CPDF_Form form(doc, page_resources, stream);
  form.ParseContent();

  // 每个 PathObject 扁平化结果。
  struct Item {
    PathFlattener flat;
    int fill_type = 0;
    int has_fill = 0;
    int has_stroke = 0;
    float line_width = 1.0f;
    float miter_limit = 10.0f;
    int line_cap = 0;
    int line_join = 0;
    std::vector<float> dash;
    float dash_phase = 0.0f;
    int has_stroke_color = 0;
    float stroke_r = 0, stroke_g = 0, stroke_b = 0;
    int has_fill_color = 0;
    float fill_r = 0, fill_g = 0, fill_b = 0;
  };
  std::vector<Item> items;

  for (const auto& obj : form) {
    CPDF_PathObject* po = obj->AsPath();
    if (!po) {
      continue;  // 仅矢量路径参与 ShapeAnnotationLayer 重建。
    }

    Item item;
    FlattenPath(po->path(), po->matrix(), &item.flat);

    if (po->has_winding_filltype()) {
      item.fill_type = 1;
    } else if (po->has_alternate_filltype()) {
      item.fill_type = 2;
    }
    item.has_fill = item.fill_type != 0;
    item.has_stroke = po->stroke() ? 1 : 0;

    const CFX_GraphState& gs = po->graph_state();
    item.line_width = gs.GetLineWidth();
    item.miter_limit = gs.GetMiterLimit();
    item.line_cap = static_cast<int>(gs.GetLineCap());
    item.line_join = static_cast<int>(gs.GetLineJoin());
    item.dash = gs.GetLineDashArray();
    item.dash_phase = gs.GetLineDashPhase();

    float rgb[3] = {0, 0, 0};
    if (ResolveColorRGB(po->color_state().GetStrokeColor(), rgb)) {
      item.has_stroke_color = 1;
      item.stroke_r = rgb[0];
      item.stroke_g = rgb[1];
      item.stroke_b = rgb[2];
    }
    rgb[0] = rgb[1] = rgb[2] = 0;
    if (ResolveColorRGB(po->color_state().GetFillColor(), rgb)) {
      item.has_fill_color = 1;
      item.fill_r = rgb[0];
      item.fill_g = rgb[1];
      item.fill_b = rgb[2];
    }
    items.push_back(std::move(item));
  }

  if (items.empty()) {
    return nullptr;
  }

  auto* result =
      static_cast<FPDFExt_ShapeAppearance*>(calloc(1, sizeof(FPDFExt_ShapeAppearance)));
  if (!result) {
    return nullptr;
  }
  auto* paths = static_cast<FPDFExt_AppearancePath*>(
      calloc(items.size(), sizeof(FPDFExt_AppearancePath)));
  if (!paths) {
    free(result);
    return nullptr;
  }
  result->paths = paths;
  result->path_count = static_cast<int>(items.size());

  CFX_FloatRect bbox = form.GetDict()->GetRectFor("BBox");
  result->bbox_left = bbox.left;
  result->bbox_bottom = bbox.bottom;
  result->bbox_right = bbox.right;
  result->bbox_top = bbox.top;

  for (size_t idx = 0; idx < items.size(); ++idx) {
    Item& it = items[idx];
    FPDFExt_AppearancePath* dst = &paths[idx];
    dst->fill_type = it.fill_type;
    dst->has_fill = it.has_fill;
    dst->has_stroke = it.has_stroke;
    dst->line_width = it.line_width;
    dst->miter_limit = it.miter_limit;
    dst->line_cap = it.line_cap;
    dst->line_join = it.line_join;
    dst->dash_phase = it.dash_phase;
    dst->has_stroke_color = it.has_stroke_color;
    dst->stroke_r = it.stroke_r;
    dst->stroke_g = it.stroke_g;
    dst->stroke_b = it.stroke_b;
    dst->has_fill_color = it.has_fill_color;
    dst->fill_r = it.fill_r;
    dst->fill_g = it.fill_g;
    dst->fill_b = it.fill_b;

    // ops
    const size_t op_size = it.flat.ops.size() * sizeof(int);
    auto* ops = static_cast<int*>(malloc(op_size ? op_size : 1));
    if (op_size) {
      memcpy(ops, it.flat.ops.data(), op_size);
    }
    dst->ops = ops;
    dst->op_count = static_cast<int>(it.flat.ops.size());

    // points
    const size_t pt_size = it.flat.points.size() * sizeof(float);
    auto* points = static_cast<float*>(malloc(pt_size ? pt_size : 1));
    if (pt_size) {
      memcpy(points, it.flat.points.data(), pt_size);
    }
    dst->points = points;
    dst->point_count = static_cast<int>(it.flat.points.size() / 2);

    // dash
    const size_t dash_size = it.dash.size() * sizeof(float);
    if (dash_size) {
      auto* dash = static_cast<float*>(malloc(dash_size));
      memcpy(dash, it.dash.data(), dash_size);
      dst->dash = dash;
      dst->dash_count = static_cast<int>(it.dash.size());
    } else {
      dst->dash = nullptr;
      dst->dash_count = 0;
    }
  }
  return result;
}

FPDF_EXPORT void FPDF_CALLCONV
FPDFExt_FreeShapeAppearance(FPDFExt_ShapeAppearance* appearance) {
  if (!appearance) {
    return;
  }
  for (int i = 0; i < appearance->path_count; ++i) {
    const FPDFExt_AppearancePath& p = appearance->paths[i];
    free(const_cast<int*>(p.ops));
    free(const_cast<float*>(p.points));
    free(const_cast<float*>(p.dash));
  }
  free(const_cast<FPDFExt_AppearancePath*>(appearance->paths));
  free(appearance);
}
