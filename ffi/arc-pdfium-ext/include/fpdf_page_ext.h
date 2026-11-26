#ifndef FPDF_PAGE_EXT_H_
#define FPDF_PAGE_EXT_H_

#include "fpdfview.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Page Content Stream Status API
// ============================================================================

// Check if a page has dirty content streams that need regeneration
// Parameters:
//  - page: page handle
// Returns:
//  - TRUE if the page has dirty content streams, FALSE otherwise
// Notes:
//  - A content stream is considered dirty if:
//    1. Any page object in that stream has been modified (IsDirty() returns true)
//    2. The stream has been explicitly marked as dirty
//  - This method uses the same logic as CPDF_PageContentGenerator::GenerateModifiedStreams()
//  - If this returns TRUE, calling FPDFPage_GenerateContent() will regenerate content
//  - If this returns FALSE, calling FPDFPage_GenerateContent() will be a no-op
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_IsContentStreamsDirty(FPDF_PAGE page);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_PAGE_EXT_H_
