
#include "fpdf_formfill_ext.h"

#include <memory>
#include <utility>
#include <variant>

#include "constants/form_fields.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_occontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "fpdfsdk/cpdfsdk_annot.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_interactiveform.h"
#include "fpdfsdk/cpdfsdk_pageview.h"
#include "fpdfsdk/cpdfsdk_widget.h"
#include "public/fpdf_annot.h"
#include "public/fpdfview.h"

// 逐像素把原生位图(BGRA)的 RGB 乘以高亮色：白底×高亮色=高亮灰(灰底)，黑字/深描边×高亮色≈黑(保持)。
// 用于 combo/list 的自带白底(/MK /BG)——比半透明 wash(源叠加)更干净，不会把黑文字洗灰。
static void MultiplyBitmapByColor(FPDF_BITMAP bitmap, uint32_t colorref) {
  if (!bitmap)
    return;
  const int width = FPDFBitmap_GetWidth(bitmap);
  const int height = FPDFBitmap_GetHeight(bitmap);
  const int stride = FPDFBitmap_GetStride(bitmap);
  uint8_t* buf = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
  if (!buf || width <= 0 || height <= 0 || stride <= 0)
    return;
  const uint8_t r = colorref & 0xFF;
  const uint8_t g = (colorref >> 8) & 0xFF;
  const uint8_t b = (colorref >> 16) & 0xFF;
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf + y * stride;
    for (int x = 0; x < width; ++x) {
      uint8_t* p = row + x * 4;  // 原生位图 BGRA：byte0=B,byte1=G,byte2=R,byte3=A
      p[0] = static_cast<uint8_t>((p[0] * b) >> 8);
      p[1] = static_cast<uint8_t>((p[1] * g) >> 8);
      p[2] = static_cast<uint8_t>((p[2] * r) >> 8);
    }
  }
}

CPDFSDK_PageView* FormHandleToPageView(FPDF_FORMHANDLE hHandle,
                                       FPDF_PAGE fpdf_page) {
  IPDF_Page* pPage = IPDFPageFromFPDFPage(fpdf_page);
  if (!pPage) {
    return nullptr;
  }

  CPDFSDK_FormFillEnvironment* pFormFillEnv =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle);
  return pFormFillEnv ? pFormFillEnv->GetOrCreatePageView(pPage) : nullptr;
}

void FPDF_CALLCONV FPDF_FFLDrawWithMatrix(FPDF_FORMHANDLE hHandle,
                                          FPDF_BITMAP bitmap,
                                          FPDF_PAGE page,
                                          const FS_MATRIX* matrix,
                                          const FS_RECTF* clipping,
                                          int flags)
{
  if (!hHandle)
    return;

  if (!bitmap)
    return;

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage)
    return;

  CPDF_Document* pPDFDoc = pPage->GetDocument();
  CPDFSDK_PageView* pPageView = FormHandleToPageView(hHandle, page);

  RetainPtr<CFX_DIBitmap> holder(CFXDIBitmapFromFPDFBitmap(bitmap));
  auto pDevice = CFX_RenderDevice::CreateForBitmap(
      holder, nullptr, !!(flags & FPDF_REVERSE_BYTE_ORDER));

  CFX_FloatRect clipping_rect;
  if (clipping)
    clipping_rect = CFXFloatRectFromFSRectF(*clipping);
  FX_RECT clip_rect = clipping_rect.ToFxRect();

  const FX_RECT rect(0, 0, pPage->GetPageWidth(), pPage->GetPageHeight());
  CFX_Matrix transform_matrix = pPage->GetDisplayMatrixForRect(rect, 0);
  if (matrix)
    transform_matrix *= CFXMatrixFromFSMatrix(*matrix);

  {
    CFX_RenderDevice::StateRestorer restorer(pDevice.get());
    pDevice->SetClip_Rect(clip_rect);

    CPDF_RenderOptions options;
    options.GetOptions().bClearType = !!(flags & FPDF_LCD_TEXT);

    // Grayscale output
    if (flags & FPDF_GRAYSCALE)
      options.SetColorMode(CPDF_RenderOptions::kGray);

    options.SetDrawAnnots(flags & FPDF_ANNOT);
    options.SetOCContext(
        pdfium::MakeRetain<CPDF_OCContext>(pPDFDoc, CPDF_OCContext::kView));

    if (pPageView)
      pPageView->PageView_OnDraw(pDevice.get(), transform_matrix, &options, clip_rect);
  }
}

// 单字段渲染：镜像 FPDF_FFLDrawWithMatrix，但只画目标 widget。入参对齐
// FPDFAnnot_RenderToBitmap（bitmap/page/annot/matrix）+ 表单句柄。调用方按 renderRect 的
// FFL 姿势传字段裁剪矩阵 + 字段矩形 clip，放大时重算 matrix 重渲染——与批注批注解渲染同逻辑。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Widget_RenderToBitmap(FPDF_FORMHANDLE hHandle,
                           FPDF_BITMAP bitmap,
                           FPDF_PAGE page,
                           FPDF_ANNOTATION widgetAnnot,
                           const FS_MATRIX* matrix,
                           const FS_RECTF* clipping,
                           int flags)
{
  if (!hHandle || !bitmap || !widgetAnnot || !page)
    return false;

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage)
    return false;

  CPDF_Document* pPDFDoc = pPage->GetDocument();
  CPDFSDK_PageView* pPageView = FormHandleToPageView(hHandle, page);
  if (!pPageView)
    return false;

  // 目标 widget 的 SDK annot（须经 form-fill 页面视图，才能走 CPDFSDK_Widget::OnDraw）。
  CPDF_AnnotContext* targetCtx = CPDFAnnotContextFromFPDFAnnotation(widgetAnnot);
  if (!targetCtx)
    return false;
  ObservedPtr<CPDFSDK_Annot> sdkAnnot(
      pPageView->GetAnnotByDict(targetCtx->GetMutableAnnotDict().Get()));
  if (!sdkAnnot)
    return false;

  RetainPtr<CFX_DIBitmap> holder(CFXDIBitmapFromFPDFBitmap(bitmap));
  auto pDevice = CFX_RenderDevice::CreateForBitmap(
      holder, nullptr, !!(flags & FPDF_REVERSE_BYTE_ORDER));
  if (!pDevice)
    return false;

  FX_RECT clip_rect;
  if (clipping) {
    clip_rect = CFXFloatRectFromFSRectF(*clipping).ToFxRect();
  } else {
    clip_rect = FX_RECT(0, 0, FPDFBitmap_GetWidth(bitmap), FPDFBitmap_GetHeight(bitmap));
  }

  // 调用方已按"字段矩形 → 位图"直接构造 user 矩阵（对齐 FPDFAnnot_RenderToBitmap），
  // 不再预乘整页 GetDisplayMatrixForRect：位图只承载单个 widget，基应为字段矩形而非整页。
  CFX_Matrix transform_matrix =
      matrix ? CFXMatrixFromFSMatrix(*matrix) : CFX_Matrix();

  {
    CFX_RenderDevice::StateRestorer restorer(pDevice.get());
    pDevice->SetClip_Rect(clip_rect);

    CPDF_RenderOptions options;
    options.GetOptions().bClearType = !!(flags & FPDF_LCD_TEXT);
    if (flags & FPDF_GRAYSCALE)
      options.SetColorMode(CPDF_RenderOptions::kGray);
    options.SetDrawAnnots(!!(flags & FPDF_ANNOT));
    options.SetOCContext(
        pdfium::MakeRetain<CPDF_OCContext>(pPDFDoc, CPDF_OCContext::kView));

    // 直接渲染外观流（AP），对齐缩略图 FFLDraw 的 deactive 分支（OnDrawDeactive→DrawAppearance）：
    // 绕开交互式 filler OnDraw 的激活分支（CFFL_FormField::OnDraw 编辑态/GetCurMatrix），
    // 无焦点独立渲染时背景/文本由 AP 供给。DrawAppearance 在 BAAnnot/Widget 层，须 downcast。
    CPDFSDK_Widget* pWidget = static_cast<CPDFSDK_Widget*>(sdkAnnot.Get());

    CPDFSDK_InteractiveForm* pInteractiveForm =
        pPageView->GetFormFillEnv()->GetInteractiveForm();
    FormFieldType field_type = pWidget->GetFieldType();

    // 灰色背景 = 表单环境的字段高亮（Document 侧 FPDF_SetFormFieldHighlightColor 置 0xFFE4E4E4）。
    // 缩略图的整页 FFLDraw 会经 CPDFSDK_Widget::DrawShadow 画这层高亮；单字段路径直接用本函数
    // 收到的 transform_matrix（字段→位图）定位，不依赖 pageview 的 GetCurrentMatrix（那只在
    // PageView_OnDraw 里被写）。画在 AP 之下作底板：给出"灰底"又不盖掉复选框框线/字段文字。
    // alpha 用 GetHighlightAlpha()（当前 Document 设为 100，整页缩放下偏近白，可调
    // FPDF_SetFormFieldHighlightAlpha 加深）。
    if (pInteractiveForm->IsNeedHighLight(field_type)) {
      // 字段位图整幅 = /Rect（FormFieldRenderer 按 rectW*scale 建位图），直接填满即灰底==/Rect==边框。
      // 不用 GetRect→transform→Normalize→ToFxRect 那套：y-down 设备 rect 塞进 CFX_FloatRect 再
      // Normalize(按 y-up)会把 top/bottom 颠倒 → 负高度 FX_RECT → FillRect 判空不画（"checkbox 没灰底"根因）。
      FX_RECT fill_rect(0, 0, FPDFBitmap_GetWidth(bitmap),
                        FPDFBitmap_GetHeight(bitmap));
      uint32_t argb = AlphaAndColorRefToArgb(
          /* 灰底做成不透明实心（灰底==字段位图==/Rect）。上下关系由 UI 层 AnnotationLayer 的
             compositingFilter=multiplyBlendMode 决定：灰×页→白变灰(灰可见)、暗色边框×灰仍暗
             (边框透出在上)。文本/复选框外观(AP)空或透明，灰底透出；combo/list 自带白的继续下方单独 wash。 */
          0xFF, pInteractiveForm->GetHighlightColor(field_type));
      pDevice->FillRect(fill_rect, argb);
    }

    pWidget->DrawAppearance(pDevice.get(), transform_matrix,
                            CPDF_Annot::AppearanceMode::kNormal);

    // combo/list 外观常自带不透明白底（/MK /BG，如 obj58 `1 g ... re f`），盖掉下方灰底。
    // 不能简单 wash 覆盖（会把黑文字洗灰）：改为逐像素乘灰——白底×高亮色=灰底、黑字×高亮色≈黑。
    if (pInteractiveForm->IsNeedHighLight(field_type) &&
        (field_type == FormFieldType::kComboBox ||
         field_type == FormFieldType::kListBox)) {
      MultiplyBitmapByColor(bitmap, pInteractiveForm->GetHighlightColor(field_type));
    }
  }
  return true;
}

