#ifndef FPDFVIEW_EXT_H_
#define FPDFVIEW_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reorder pages in the document according to the provided array
// Parameters:
//  - document: handle to the document
//  - page_indices: array of page indices in the desired order
//  - count: number of elements in the page_indices array
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - The page_indices array must contain all page indices from 0 to count-1
//  - Each index must appear exactly once in the array
//  - The document will be reordered to match the order specified in the array
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_ReorderPages(FPDF_DOCUMENT document, const int* page_indices, int count);

// Check if the document has page labels defined
// Parameters:
//  - document: handle to the document
// Returns:
//  - TRUE if the document has page labels, FALSE otherwise
// Notes:
//  - Page labels are defined in the document catalog's /PageLabels entry
//  - Page labels allow custom numbering schemes (Roman numerals, letters, etc.)
//  - If this returns TRUE, page reordering may affect the page label structure
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentHasPageLabel(FPDF_DOCUMENT document);

#ifdef __cplusplus
}
#endif

#endif  // FPDFVIEW_EXT_H_
