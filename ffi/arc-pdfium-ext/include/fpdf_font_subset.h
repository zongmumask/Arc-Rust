#ifndef FPDF_FONT_SUBSET_H_
#define FPDF_FONT_SUBSET_H_

#include "fpdfview.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Font Subsetting API using HarfBuzz
// ============================================================================

// Create a Type0 font with subsetted font data using HarfBuzz
// Parameters:
//  - document: document handle
//  - font_data: original font file data (TTF/OTF format)
//  - font_data_size: original font data size in bytes
//  - unicode_chars: array of Unicode characters to include in subset (UTF-16LE)
//  - char_count: number of characters
//  - font_name: PostScript name of the font (optional, can be NULL)
// Returns:
//  - Font handle, or NULL on failure
// Notes:
//  - Uses HarfBuzz to create a minimal font subset containing only the specified characters
//  - Significantly reduces embedded font size compared to FPDFFont_CreateType0FromData
//  - Automatically creates FontDescriptor, ToUnicode CMap, and CIDToGIDMap
//  - If font_name is provided, uses format: <PREFIX>+<font_name>
//  - Must call FPDFFont_Close when done to release the font object
//  - The font must be added to a page with FPDFPage_AddFont before use
FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFFont_CreateType0FromSubset(FPDF_DOCUMENT document,
                                const void* font_data,
                                unsigned long font_data_size,
                                const unsigned short* unicode_chars,
                                int char_count,
                                const char* font_name);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_FONT_SUBSET_H_
