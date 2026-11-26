#ifndef FPDF_FONT_EXT_H_
#define FPDF_FONT_EXT_H_

#include "fpdfview.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Font Type Enumeration
// ============================================================================
// Note: FPDF_FONT is already defined in fpdfview.h

// Font type enumeration
typedef enum {
    FPDF_FONT_TYPE_UNKNOWN = 0,   // Unknown font type
    FPDF_FONT_TYPE_TYPE1 = 1,     // Type1 font
    FPDF_FONT_TYPE_TRUETYPE = 2,  // TrueType font
    FPDF_FONT_TYPE_TYPE3 = 3,     // Type3 font
    FPDF_FONT_TYPE_TYPE0 = 4      // Type0 (CID) font - supports Unicode
} FPDF_FONT_TYPE;

// ============================================================================
// Page Font Enumeration API
// ============================================================================

// Get the number of fonts in a page
// Parameters:
//  - page: page handle
// Returns:
//  - Number of fonts in the page, or -1 on failure
// Notes:
//  - Counts fonts in the page's /Resources/Font dictionary
FPDF_EXPORT int FPDF_CALLCONV
FPDFPage_GetFontCount(FPDF_PAGE page);

// Get a font handle from a page by index
// Parameters:
//  - page: page handle
//  - index: font index (0 to count-1)
// Returns:
//  - Font handle, or NULL on failure
// Notes:
//  - The returned handle is managed by the page, do not manually release
//  - Handle remains valid until the page is closed
FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFPage_GetFont(FPDF_PAGE page, int index);

// Get font type
// Parameters:
//  - font: font handle
// Returns:
//  - Font type (FPDF_FONT_TYPE_*), or FPDF_FONT_TYPE_UNKNOWN on failure
FPDF_EXPORT int FPDF_CALLCONV
FPDFFont_GetType(FPDF_FONT font);

// Check if font is a subset
// Parameters:
//  - font: font handle
// Returns:
//  - TRUE if the font is a subset, FALSE otherwise
// Notes:
//  - A font is typically a subset if the BaseFont name starts with a 6-character prefix followed by '+'
//  - Example: "ABCDEF+FontName" indicates a subset
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_IsSubset(FPDF_FONT font);

// Check if font has ToUnicode mapping
// Parameters:
//  - font: font handle
// Returns:
//  - TRUE if the font has ToUnicode mapping, FALSE otherwise
// Notes:
//  - ToUnicode mapping allows proper text extraction and searching
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_HasToUnicode(FPDF_FONT font);

// Get font resource name (e.g., "F1", "F2")
// Parameters:
//  - font: font handle
//  - buffer: buffer to receive resource name (can be NULL to query length)
//  - buflen: pointer to buffer length, returns actual length (including null terminator)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Call with buffer=NULL to get required buffer size first
//  - Resource name is the key in the page's /Resources/Font dictionary
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_GetResourceName(FPDF_FONT font,
                         char* buffer,
                         unsigned long* buflen);

// Check if a font supports a Unicode character
// Parameters:
//  - font: font handle
//  - unicode: Unicode code point
// Returns:
//  - TRUE if the font supports the character, FALSE otherwise
// Notes:
//  - Queries the font's cmap or ToUnicode mapping
//  - Checks if a glyph exists for the given Unicode code point
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_SupportsUnicode(FPDF_FONT font, unsigned int unicode);

// Check if a font name is one of the Standard 14 fonts
// Parameters:
//  - base_font_name: BaseFont name (e.g., "Times-Roman", "Helvetica")
// Returns:
//  - TRUE if the font is a Standard 14 font, FALSE otherwise
// Notes:
//  - Standard 14 fonts are built into all PDF readers
//  - These fonts do not need to be embedded
//  - Automatically handles subset prefixes (e.g., "ABCDEF+Times-Roman")
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_IsStandard14(const char* base_font_name);

// Check if a font handle represents a Standard 14 font
// Parameters:
//  - font: font handle
// Returns:
//  - TRUE if the font is a Standard 14 font, FALSE otherwise
// Notes:
//  - Uses CPDF_Font's IsStandardFont() method for accurate detection
//  - More reliable than string-based checking
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_IsStandard14Font(FPDF_FONT font);

// Check if a Standard 14 font supports a Unicode character
// Parameters:
//  - base_font_name: Standard 14 font name (e.g., "Times-Roman")
//  - unicode: Unicode code point
// Returns:
//  - TRUE if the font supports the character, FALSE otherwise
// Notes:
//  - Standard 14 fonts mainly support ASCII (0x20-0x7E) and Latin-1 (0xA0-0xFF)
//  - Symbol and ZapfDingbats have special character ranges
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_Standard14SupportsChar(const char* base_font_name,
                                unsigned int unicode);

// Get the width of a character in a font from its raw charcode.
//
// Matching PDFium's CPDF_Font::GetCharWidthF(charcode) — no Unicode
// round-trip, no fallback.  The caller must supply the font's own charcode
// (glyph index or CID).  For Standard 14 fonts the charcode is the Unicode
// codepoint since Standard 14 fonts are laid out with Latin-1 encoding.
//
// Parameters:
//  - font: font handle
//  - charcode: raw glyph index / CID (NOT a Unicode codepoint)
//  - font_size: font size in points (Tfs)
//  - width: receives the width in text-space (GetCharWidthF × font_size / 1000)
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_GetCharWidth(FPDF_FONT font,
                      unsigned int charcode,
                      float font_size,
                      float* width);

// Get the width of a character in a Standard 14 font
// Parameters:
//  - base_font_name: Standard 14 font name (e.g., "Times-Roman", "Helvetica")
//  - unicode: Unicode code point
//  - font_size: font size
//  - width: pointer to receive the character width
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Does not require creating a font object
//  - Only works for Standard 14 fonts
//  - Returns width in PDF units
// Get the width of a character from a text object
// Parameters:
//  - text_object: text object handle
//  - charcode: character code (not Unicode)
//  - width: pointer to receive the character width
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Uses the text object's font and font size
//  - Works for all font types (Standard 14, embedded, etc.)
//  - Returns width in PDF units (already scaled by font size)
//  - This is the most accurate method for getting character width
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetCharWidth(FPDF_PAGEOBJECT text_object,
                         uint32_t charcode,
                         float* width);

// Look up the charcode for a Unicode codepoint in a font.
//
// font    - the font handle.
// unicode - Unicode codepoint (BMP only, U+0000–U+FFFF).
//
// Returns the charcode, or FPDF_FONT_INVALID_CHARCODE (0xFFFFFFFF) if the
// font has no mapping for this codepoint.
FPDF_EXPORT uint32_t FPDF_CALLCONV
FPDFFont_GetCharCode(FPDF_FONT font, uint32_t unicode);

// Create a Standard 14 font reference (without embedding font data)
// Parameters:
//  - document: document handle
//  - base_font_name: Standard 14 font name (e.g., "Times-Roman", "Helvetica")
// Returns:
//  - Font handle, or NULL on failure
// Notes:
//  - Creates a simple Type1 font dictionary without embedded data
//  - Only works for Standard 14 fonts
//  - Must call FPDFFont_Close when done
//  - The font must be added to a page with FPDFPage_AddFont before use
FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFFont_CreateStandard14(FPDF_DOCUMENT document,
                          const char* base_font_name);

// ============================================================================
// Font Creation API
// ============================================================================

// Create a Type0 font from font data (supports Unicode)
// Parameters:
//  - document: document handle
//  - font_data: font file data (TTF/OTF format)
//  - font_data_size: font data size in bytes
//  - unicode_chars: array of Unicode characters to support (UTF-16LE)
//  - char_count: number of characters
//  - font_name: PostScript name of the font (optional, can be NULL)
// Returns:
//  - Font handle, or NULL on failure
// Notes:
//  - Automatically creates FontDescriptor, ToUnicode CMap, and CIDToGIDMap
//  - Font data will be embedded in the PDF
//  - If font_name is provided, uses format: <PREFIX>+<font_name>
//  - If font_name is NULL, uses: <PREFIX>+CustomFont
//  - Must call FPDFFont_Close when done to release the font object
//  - The font must be added to a page with FPDFPage_AddFont before use
FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFFont_CreateType0FromData(FPDF_DOCUMENT document,
                             const void* font_data,
                             unsigned long font_data_size,
                             const unsigned short* unicode_chars,
                             int char_count,
                             const char* font_name);

// Add a font to a page's resources
// Parameters:
//  - page: page handle
//  - font: font handle
//  - font_name: buffer to receive assigned font name (e.g., "F1")
//  - font_name_len: pointer to buffer length, returns actual length (including null terminator)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Automatically generates a unique font name
//  - Font is added to the page's /Resources/Font dictionary
//  - Call with font_name=NULL to get required buffer size first
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_AddFont(FPDF_PAGE page,
                 FPDF_FONT font,
                 char* font_name,
                 unsigned long* font_name_len);

// ============================================================================
// Text Object Font Operations API
// ============================================================================
//
// Note: The following functions are available in PDFium's public API (fpdf_edit.h):
// - FPDFFont_Close(FPDF_FONT font) - Closes a loaded PDF font
// - FPDFTextObj_GetFont(FPDF_PAGEOBJECT text) - Gets the font of a text object
//
// These functions do not need custom implementation and can be used directly.
// See task-4.3-5.1-completion-summary.md for details.
// ============================================================================

// Set the font of a text object
// Parameters:
//  - text_object: text object handle
//  - font: font handle
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - The font must already be added to the page's resources
//  - Updates the text object to use the specified font
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetFont(FPDF_PAGEOBJECT text_object, FPDF_FONT font);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_FONT_EXT_H_
