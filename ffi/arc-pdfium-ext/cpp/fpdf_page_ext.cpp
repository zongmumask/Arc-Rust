#include "fpdf_page_ext.h"

#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pageobjectholder.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_IsContentStreamsDirty(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }

  // Check if any page object is dirty
  // This mirrors the logic in CPDF_PageContentGenerator::GenerateModifiedStreams()
  for (const auto& pObj : *pPage) {
    // Must include dirty page objects even if they are marked as inactive.
    // Otherwise an inactive object will not be detected that its stream needs
    // to be removed as part of regeneration.
    if (pObj->IsDirty()) {
      return true;
    }
  }

  // Check if any streams are explicitly marked as dirty
  // Note: We use HasDirtyStreams() instead of TakeDirtyStreams() to avoid
  // modifying the page state (TakeDirtyStreams() clears the dirty streams set)
  if (pPage->HasDirtyStreams()) {
    return true;
  }

  return false;
}
