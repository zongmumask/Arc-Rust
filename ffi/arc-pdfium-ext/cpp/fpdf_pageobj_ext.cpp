#include "fpdf_pageobj_ext.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <memory>
#include <vector>

#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/render/cpdf_imagerenderer.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/render/cpdf_renderstatus.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/fx_dib.h"
#include "core/fpdfapi/page/cpdf_contentmarkitem.h"
#include "core/fpdfapi/page/cpdf_contentmarks.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

std::vector<WideString> CollectActualTextMarks(CPDF_PageObject* pPageObj) {
  std::vector<WideString> marks;
  if (!pPageObj) {
    return marks;
  }

  const CPDF_ContentMarks* pMarks = pPageObj->GetContentMarks();
  if (!pMarks) {
    return marks;
  }

  const size_t nMarks = pMarks->CountItems();
  for (size_t i = 0; i < nMarks; ++i) {
    const CPDF_ContentMarkItem* item = pMarks->GetItem(i);
    if (!item) {
      continue;
    }
    RetainPtr<const CPDF_Dictionary> dict = item->GetParam();
    if (!dict) {
      continue;
    }
    WideString candidate = dict->GetUnicodeTextFor("ActualText");
    if (!candidate.IsEmpty()) {
      marks.push_back(std::move(candidate));
    }
  }

  return marks;
}

unsigned long CopyWideStringToUTF16LE(const WideString& text,
                                      void* buffer,
                                      unsigned long buflen) {
  const size_t char_count = text.GetLength();
  const unsigned long required =
      static_cast<unsigned long>((char_count + 1) * sizeof(uint16_t));

  if (!buffer || buflen < required) {
    return required;
  }

  uint16_t* out = static_cast<uint16_t*>(buffer);
  for (size_t i = 0; i < char_count; ++i) {
    wchar_t wc = text[i];
    out[i] = static_cast<uint16_t>(wc & 0xFFFF);
  }
  out[char_count] = 0;

  return required;
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetContentStream(FPDF_PAGEOBJECT page_object) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return -1;
  }
  
  return pPageObj->GetContentStream();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetContentStream(FPDF_PAGEOBJECT page_object, int content_stream_index) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj) {
    return false;
  }
  
  pPageObj->SetContentStream(content_stream_index);
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFPageObj_GetActualText(FPDF_PAGEOBJECT page_object,
                          void* buffer,
                          unsigned long buflen) {
    CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
    if (!pPageObj) {
        return 0;
    }

    std::vector<WideString> marks = CollectActualTextMarks(pPageObj);
    if (marks.empty()) {
        return 0;
    }

    // Preserve existing behaviour for callers that only want a single string:
    // the last ActualText wins, matching cpdf_textpage.cpp.
    return CopyWideStringToUTF16LE(marks.back(), buffer, buflen);
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetActualTextMarkCount(FPDF_PAGEOBJECT page_object) {
    CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
    if (!pPageObj) {
        return -1;
    }

    std::vector<WideString> marks = CollectActualTextMarks(pPageObj);
    return static_cast<int>(marks.size());
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFPageObj_GetActualTextMark(FPDF_PAGEOBJECT page_object,
                              int index,
                              void* buffer,
                              unsigned long buflen) {
    if (index < 0) {
        return 0;
    }

    CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
    if (!pPageObj) {
        return 0;
    }

    std::vector<WideString> marks = CollectActualTextMarks(pPageObj);
    if (static_cast<size_t>(index) >= marks.size()) {
        return 0;
    }

    return CopyWideStringToUTF16LE(marks[static_cast<size_t>(index)], buffer, buflen);
}

FPDF_EXPORT FPDF_BITMAP FPDF_CALLCONV
FPDFImageObj_GetRenderedBitmapWithTransparency(FPDF_DOCUMENT document,
                                               FPDF_PAGE page,
                                               FPDF_PAGEOBJECT image_object) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return nullptr;
  }

  CPDF_Page* optional_page = CPDFPageFromFPDFPage(page);
  if (optional_page && optional_page->GetDocument() != doc) {
    return nullptr;
  }

  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(image_object);
  if (!pPageObj || pPageObj->GetType() != CPDF_PageObject::Type::kImage) {
    return nullptr;
  }
  
  CPDF_ImageObject* image = pPageObj->AsImage();
  if (!image) {
    return nullptr;
  }

  // Get the actual image to check for transparency
  RetainPtr<CPDF_Image> pImg = image->GetImage();
  if (!pImg) {
    return nullptr;
  }

  // Use the image's native pixel dimensions (not the display matrix)
  // to preserve full resolution. The image_matrix gives PDF point dimensions
  // which are typically much smaller than the actual pixel data.
  int pixel_width = pImg->GetPixelWidth();
  int pixel_height = pImg->GetPixelHeight();

  // Validate dimensions
  if (pixel_width <= 0 || pixel_height <= 0 ||
      pixel_width > 65535 || pixel_height > 65535) {
    return nullptr;
  }

  // Get the image matrix for constructing the render transform
  const CFX_Matrix& image_matrix = image->matrix();
  float display_width = std::ceil(hypotf(image_matrix.a, image_matrix.c));
  float display_height = std::ceil(hypotf(image_matrix.b, image_matrix.d));

  // Create result bitmap at native pixel resolution with BGRA format
  auto result_bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!result_bitmap->Create(pixel_width, pixel_height,
                             FXDIB_Format::kBgra)) {
    return nullptr;
  }

  // CRITICAL: Initialize bitmap to fully transparent
  // This ensures the background is transparent instead of white/black
  result_bitmap->Clear(0x00000000);  // ARGB = fully transparent

  // Set up rendering context
  RetainPtr<CPDF_Dictionary> page_resources =
      optional_page ? optional_page->GetMutablePageResources() : nullptr;
  CPDF_RenderContext context(doc, std::move(page_resources),
                             /*pPageCache=*/nullptr);
  
  // Create render device
  auto device = CFX_RenderDevice::CreateForBitmap(result_bitmap);

  // CRITICAL: Create render status WITHOUT processing clip path
  // The clip path may fill the background, destroying transparency
  // We'll handle clipping differently
  CPDF_RenderStatus status(&context, device.get());
  
  // Create image renderer
  CPDF_ImageRenderer renderer(&status);

  // Set up transformation matrix.
  // The image renderer expects a matrix that maps the image from its unit space
  // [0,1]x[0,1] to the output device (bitmap) pixel coordinates.
  //
  // image_matrix maps [0,1] -> PDF page coordinates (display space).
  // We need to compose: first image_matrix (to page coords), then translate
  // to origin (subtract min_x/min_y), then scale from display to pixel coords,
  // then flip Y for bitmap top-down convention.
  //
  // The original working matrix for display-size output was:
  //   flip_y * translate(-min_x, min_y)   where flip_y = (1, 0, 0, -1, 0, display_height)
  //
  // For pixel-size output, we scale the Y-flip and translation by the pixel/display ratio.

  float min_x = image_matrix.e + std::min(image_matrix.a, image_matrix.c);
  float min_y = image_matrix.f + std::min(image_matrix.b, image_matrix.d);

  float scale_x = static_cast<float>(pixel_width) / display_width;
  float scale_y = static_cast<float>(pixel_height) / display_height;

  // Flip vertically (scaled to pixel coords) then translate to origin (scaled)
  CFX_Matrix image_render_matrix(
      scale_x, 0, 0, -scale_y,
      -min_x * scale_x,
      min_y * scale_y + pixel_height);

  // NOTE: We intentionally DO NOT apply clip path here
  // because it may fill the background and destroy transparency
  // The image should render with its natural bounds (SMask will be applied automatically)

  // Perform the actual rendering
  // The renderer will automatically apply the SMask (soft mask) if present
  bool should_continue =
      renderer.Start(image, image_render_matrix, /*bStdCS=*/false);
  while (should_continue) {
    should_continue = renderer.Continue(/*pPause=*/nullptr);
  }

  if (!renderer.GetResult()) {
    return nullptr;
  }

  // Return the bitmap with transparency preserved
  // Caller takes ownership
  return FPDFBitmapFromCFXDIBitmap(result_bitmap.Leak());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetInterpolate(FPDF_PAGEOBJECT page_object, FPDF_BOOL interpolate) {
  CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pPageObj || !pPageObj->IsImage()) {
    return false;
  }
  CPDF_ImageObject* pImageObj = pPageObj->AsImage();
  if (!pImageObj) {
    return false;
  }
  RetainPtr<CPDF_Image> pImage = pImageObj->GetImage();
  if (!pImage || pImage->IsInline()) {
    return false;
  }
  RetainPtr<const CPDF_Stream> pStream = pImage->GetStream();
  if (!pStream) {
    return false;
  }
  // CPDF_Stream keeps its dictionary private; GetDict() is CPDF_Object's public
  // wrapper that routes to the stream's dict override. The freshly-created image
  // dict is mutable underneath the const view, so setting a boolean is safe.
  CPDF_Dictionary* dict =
      const_cast<CPDF_Dictionary*>(pStream->GetDict().Get());
  if (!dict) {
    return false;
  }
  if (interpolate) {
    dict->SetNewFor<CPDF_Boolean>("Interpolate", true);
  } else {
    dict->RemoveFor("Interpolate");
  }
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_ReorderObject(FPDF_PAGE page, FPDF_PAGEOBJECT page_object, size_t new_index) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  CPDF_PageObject* pObj = CPDFPageObjectFromFPDFPageObject(page_object);
  if (!pObj) {
    return false;
  }

  // Locate the object in the page's object list.
  auto it = std::find_if(pPage->begin(), pPage->end(),
                         [pObj](const std::unique_ptr<CPDF_PageObject>& obj) {
                           return obj.get() == pObj;
                         });
  if (it == pPage->end()) {
    return false;  // Object not on this page.
  }

  const size_t count = pPage->GetPageObjectCount();
  if (count == 0) {
    return false;
  }

  const size_t cur = std::distance(pPage->begin(), it);

  // Clamp |new_index| to the valid range. count-1 is the last (topmost) list
  // position since list order maps to z order.
  if (new_index >= count) {
    new_index = count - 1;
  }
  if (new_index == cur) {
    return true;  // Already there: no-op.
  }

  // A page can contain multiple content streams. Their order takes precedence
  // over the in-memory page-object list when the PDF is rendered, so merely
  // moving the object in that list cannot move it across stream boundaries.
  // Put the moved object in the target object's stream as well. Regenerating
  // the old and target streams then makes list order and paint order agree.
  const int32_t target_content_stream =
      (*std::next(pPage->begin(), new_index))->GetContentStream();

  // Remove ownership, then re-insert at |new_index|. Both are public holder
  // APIs run in the same synchronous call, so the object never leaves the page
  // (it is only briefly held by the local unique_ptr). RemovePageObject dirties
  // the old content stream. Marking the object dirty after assigning the target
  // stream makes GenerateContent rewrite that stream too.
  std::unique_ptr<CPDF_PageObject> holder = pPage->RemovePageObject(pObj);
  if (!holder) {
    return false;
  }
  holder->SetContentStream(target_content_stream);
  holder->SetDirty(true);
  // |new_index| <= count - 1 == pre-removal size == post-removal list size,
  // so InsertPageObjectAtIndex accepts it (the top case appends at size).
  return pPage->InsertPageObjectAtIndex(new_index, std::move(holder));
}
