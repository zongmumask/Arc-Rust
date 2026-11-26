#ifndef FPDF_OCR_EXT_H_
#define FPDF_OCR_EXT_H_

#include "fpdfview.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle types
typedef void* FPDF_FONT_STREAM;
typedef void* FPDF_OCR_FONT;

// OCR text box structure
typedef struct FPDF_OCR_TEXTBOX_ {
    const unsigned short* text;  // UTF-16LE encoded text
    int text_length;             // Text length in characters (not bytes)
    float x;                     // Bottom-left X coordinate (PDF coordinate system)
    float y;                     // Bottom-left Y coordinate (PDF coordinate system)
    float width;                 // Width
    float height;                // Height
    float font_size;             // Font size
} FPDF_OCR_TEXTBOX;

// ============================================================================
// Font Stream Management
// ============================================================================

// Create a font stream object from font file data
// Parameters:
//  - document: document handle
//  - font_data: font file data (TTF/OTF format)
//  - font_data_size: font file size in bytes
// Returns:
//  - Font stream handle, or NULL on failure
// Notes:
//  - Font data will be copied into the PDF document
//  - Must call FPDF_CloseFontStream when done
FPDF_EXPORT FPDF_FONT_STREAM FPDF_CALLCONV
FPDF_LoadFontStreamFromData(FPDF_DOCUMENT document,
                            const void* font_data,
                            unsigned long font_data_size);

// Release a font stream object
// Parameters:
//  - font_stream: font stream handle
// Notes:
//  - Handle becomes invalid after release
//  - Font data is already embedded in PDF, does not affect created fonts
FPDF_EXPORT void FPDF_CALLCONV
FPDF_CloseFontStream(FPDF_FONT_STREAM font_stream);

// ============================================================================
// OCR Font Creation
// ============================================================================

// Create an OCR font for a page (Type0 composite font with Unicode support)
// Parameters:
//  - page: page handle
//  - font_stream: font stream handle (from FPDF_LoadFontStreamFromData)
//  - unicode_chars: array of Unicode characters to support (UTF-16LE)
//  - char_count: number of characters
//  - font_name: font name in page resources (e.g., "F1")
// Returns:
//  - OCR font handle, or NULL on failure
// Notes:
//  - Automatically creates ToUnicode CMap and CIDToGIDMap
//  - All characters map to the same glyphless glyph (GID=1)
//  - Font is automatically registered in page resource dictionary
//  - Ownership: the caller owns the returned handle. CPDF_Font::Create does NOT
//    register the font with the document's font cache, so the document does not
//    hold a reference. The caller MUST call FPDF_CloseOCRFont to release it,
//    otherwise the CPDF_Font object leaks.
//  - Text objects added via FPDF_AddInvisibleTextObject* retain their own
//    reference to the font, so it is safe to call FPDF_CloseOCRFont after adding
//    text objects (the font stays alive until the text objects are released).
FPDF_EXPORT FPDF_OCR_FONT FPDF_CALLCONV
FPDF_CreateOCRFontForPage(FPDF_PAGE page,
                         FPDF_FONT_STREAM font_stream,
                         const unsigned short* unicode_chars,
                         int char_count,
                         const char* font_name);

// Release an OCR font object
// Parameters:
//  - font: OCR font handle
// Notes:
//  - Releases the CPDF_Font underlying the handle (adopts the reference that
//    FPDF_CreateOCRFontForPage leaked via CPDF_Font::Leak()). Must be called to
//    avoid leaking the font.
//  - Safe to call after the font has been used to add text objects: text objects
//    retain their own reference, so closing only drops the caller's reference.
//  - Calling with a NULL handle is a no-op.
FPDF_EXPORT void FPDF_CALLCONV
FPDF_CloseOCRFont(FPDF_OCR_FONT font);

// ============================================================================
// Text Object Addition
// ============================================================================

// Add a single invisible text object
// Parameters:
//  - page: page handle
//  - font: OCR font handle
//  - text_utf16: UTF-16LE encoded text
//  - text_length: text length in characters
//  - x, y: text bottom-left coordinates (PDF coordinate system)
//  - width, height: text box dimensions
//  - font_size: font size
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Text rendering mode is invisible (MODE_INVISIBLE)
//  - Text matrix is automatically calculated to fit specified dimensions
//  - Must call FPDFPage_GenerateContent after adding all text objects
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_AddInvisibleTextObject(FPDF_PAGE page,
                            FPDF_OCR_FONT font,
                            const unsigned short* text_utf16,
                            int text_length,
                            float x,
                            float y,
                            float width,
                            float height,
                            float font_size);

// Add multiple invisible text objects with automatic spacing
// Parameters:
//  - page: page handle
//  - font: OCR font handle
//  - text_boxes: array of text boxes
//  - box_count: number of text boxes
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Automatically detects if adjacent text boxes are on the same line
//  - Automatically inserts space objects between text boxes on the same line with gaps
//  - Space width is calculated based on text box spacing
//  - Must call FPDFPage_GenerateContent after adding all text objects
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_AddInvisibleTextObjectsWithSpacing(FPDF_PAGE page,
                                        FPDF_OCR_FONT font,
                                        const FPDF_OCR_TEXTBOX* text_boxes,
                                        int box_count);

// ============================================================================
// High-Level Convenience API
// ============================================================================

// Add a complete invisible OCR text layer to a page
// Parameters:
//  - page: page handle
//  - font_data: glyphless font file data (TTF format)
//  - font_data_size: font file size
//  - text_boxes: array of text boxes
//  - box_count: number of text boxes
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - This is a convenience API that automatically:
//    1. Loads font stream
//    2. Collects used Unicode characters
//    3. Creates OCR font
//    4. Adds text objects in batch (with space insertion)
//    5. Generates page content stream
//    6. Cleans up resources
//  - Suitable for most scenarios, no need to manually manage font objects
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_AddInvisibleTextLayer(FPDF_PAGE page,
                           const void* font_data,
                           unsigned long font_data_size,
                           const FPDF_OCR_TEXTBOX* text_boxes,
                           int box_count);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_OCR_EXT_H_
