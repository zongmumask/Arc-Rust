#ifndef FPDF_TEXT_EXT_H_
#define FPDF_TEXT_EXT_H_

#include "fpdfview.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Text Object Character Information API
// ============================================================================

// Get the number of characters in a text object
// Parameters:
//  - text_object: text object handle
// Returns:
//  - Number of characters in the text object, or 0 on failure
// Notes:
//  - Counts actual characters, excluding invalid char codes
//  - This is equivalent to CPDF_TextObject::CountChars()
FPDF_EXPORT int FPDF_CALLCONV
FPDFTextObj_CountChars(FPDF_PAGEOBJECT text_object);

// Get character information from a text object
// Parameters:
//  - text_object: text object handle
//  - char_index: character index (0 to CountChars-1)
//  - char_code: pointer to receive character code (can be NULL)
//  - origin_x: pointer to receive character origin X (can be NULL)
//  - origin_y: pointer to receive character origin Y (can be NULL)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - char_index is the character index in the text object, not the item index
//  - origin is relative to the text object's position
//  - origin is in PDF user space units
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetCharInfo(FPDF_PAGEOBJECT text_object,
                        int char_index,
                        uint32_t* char_code,
                        double* origin_x,
                        double* origin_y);

// Get the bounding box of a character in a text object
// Parameters:
//  - text_object: text object handle
//  - char_index: character index (0 to CountChars-1)
//  - left: pointer to receive left boundary
//  - right: pointer to receive right boundary
//  - bottom: pointer to receive bottom boundary
//  - top: pointer to receive top boundary
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Uses the same calculation logic as FPDFText_GetCharBox
//  - Boundaries are in PDF user space (page coordinates)
//  - This method does NOT require TextPage, works directly with TextObject
//  - More efficient than FPDFText_GetCharBox when you have the TextObject
//
// Calculation steps:
//  1. Get character bounding box from font (in font units)
//  2. Scale by fontSize / 1000
//  3. Add character origin offset
//  4. Apply text object's transformation matrix
//  5. Convert to page coordinates
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetCharBox(FPDF_PAGEOBJECT text_object,
                       int char_index,
                       double* left,
                       double* right,
                       double* bottom,
                       double* top);

// Get the character spacing (Tc) of a text object.
// Returns 0.0 if the text object has no explicit char spacing.
FPDF_EXPORT float FPDF_CALLCONV
FPDFTextObj_GetCharSpacing(FPDF_PAGEOBJECT text_object);

// Get the word spacing (Tw) of a text object.
// Returns 0.0 if the text object has no explicit word spacing.
FPDF_EXPORT float FPDF_CALLCONV
FPDFTextObj_GetWordSpacing(FPDF_PAGEOBJECT text_object);

// Set the character spacing (Tc) on a text object.
// Call FPDFPage_GenerateContent() after modifying the page.
FPDF_EXPORT void FPDF_CALLCONV
FPDFTextObj_SetCharSpacing(FPDF_PAGEOBJECT text_object, float char_spacing);

// Set the word spacing (Tw) on a text object.
// Call FPDFPage_GenerateContent() after modifying the page.
FPDF_EXPORT void FPDF_CALLCONV
FPDFTextObj_SetWordSpacing(FPDF_PAGEOBJECT text_object, float word_spacing);

// Get the total number of items (chars + kerning markers) in a text object.
// This includes kInvalidCharCode entries that represent TJ kerning adjustments.
// Use FPDFTextObj_GetItemInfo to inspect each item.
FPDF_EXPORT int FPDF_CALLCONV
FPDFTextObj_GetItemCount(FPDF_PAGEOBJECT text_object);

// Get information about a single item in a text object.
// Parameters:
//  - text_object: text object handle
//  - index: item index (0 to GetItemCount-1)
//  - char_code: receives the char code; 0xFFFFFFFF means this is a kerning marker
//  - kerning: receives the kerning value (in 1/1000 em units, only valid when
//             char_code == 0xFFFFFFFF)
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetItemInfo(FPDF_PAGEOBJECT text_object,
                        int index,
                        uint32_t* char_code,
                        float* kerning);

// Set the text content of a text object using multiple segments with kerning.
// This is the TJ-equivalent of FPDFText_SetText.
//
// Parameters:
//  - text_object: text object handle (must have a font already set)
//  - utf16_segments: array of UTF-16LE null-terminated strings, one per segment
//  - kernings: array of kerning values between segments (length = segment_count - 1)
//              Values are in 1/1000 em units (same as TJ array numbers).
//              Positive = move next segment left (tighter spacing).
//  - segment_count: number of segments (must be >= 1)
// Returns TRUE on success.
//
// Note: After calling this function, call FPDFPage_GenerateContent() to
// persist the changes.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetTextWithKerning(FPDF_PAGEOBJECT text_object,
                               const FPDF_WCHAR** utf16_segments,
                               const float* kernings,
                               int segment_count);

// Set text content using raw font charcodes with optional kerning between segments.
//
// Unlike FPDFTextObj_SetTextWithKerning which takes Unicode strings and encodes
// them via font->CharCodeFromUnicode, this function takes raw charcodes directly.
// This bypasses the Unicode→charcode reverse lookup, which is essential for:
//  - Ligature glyphs (e.g. Th ligature charcode 0x1D that maps to U+0054 U+0068)
//  - Fonts whose CharCodeFromUnicode is unreliable or missing
//
// Parameters:
//  - text_object:      text object handle (must have a font already set)
//  - charcodes:        flat array of raw uint32 charcodes
//  - segment_lengths:  number of charcodes in each segment (length = segment_count)
//  - kernings:         kerning values between segments (length = segment_count - 1,
//                      ignored when segment_count == 1)
//  - segment_count:    number of segments (must be >= 1)
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetCharcodesWithKerning(FPDF_PAGEOBJECT text_object,
                                     const uint32_t* charcodes,
                                     const int* segment_lengths,
                                     const float* kernings,
                                     int segment_count);

// Safe variant of FPDFTextObj_SetTextWithKerning that takes explicit
// segment_lengths instead of relying on null-terminated UTF-16 segments.
//
// Parameters:
//  - text_object:     text object handle (must have a font already set)
//  - utf16_segments:  array of pointers to UTF-16LE code unit arrays, one per
//                     segment (each of segment_lengths[i] code units; need not
//                     be null-terminated)
//  - segment_lengths: number of UTF-16 code units in each segment
//                     (length = segment_count)
//  - kernings:        kerning values between segments (length = segment_count - 1,
//                     ignored when segment_count == 1)
//  - segment_count:   number of segments (must be >= 1)
//
// Returns TRUE on success, FALSE on failure (null segment pointer, negative
// segment length, or segment_count < 1).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetTextWithKerning2(FPDF_PAGEOBJECT text_object,
                                const FPDF_WCHAR** utf16_segments,
                                const int* segment_lengths,
                                const float* kernings,
                                int segment_count);

// Safe variant of FPDFTextObj_SetCharcodesWithKerning that takes an explicit
// charcodes_count to bound the charcodes array, preventing out-of-bounds reads
// when segment_lengths sums beyond the array.
//
// Parameters:
//  - text_object:     text object handle (must have a font already set)
//  - charcodes:       flat array of raw uint32 charcodes
//  - charcodes_count: number of uint32 entries in charcodes
//  - segment_lengths: number of charcodes in each segment (length = segment_count)
//  - kernings:        kerning values between segments (length = segment_count - 1,
//                     ignored when segment_count == 1)
//  - segment_count:   number of segments (must be >= 1)
//
// Returns TRUE on success, FALSE on failure (negative segment length,
// cumulative segment length exceeding charcodes_count, or segment_count < 1).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetCharcodesWithKerning2(FPDF_PAGEOBJECT text_object,
                                     const uint32_t* charcodes,
                                     int charcodes_count,
                                     const int* segment_lengths,
                                     const float* kernings,
                                     int segment_count);

// Get the Unicode codepoint for a character in a text object by its char index.
// This uses the font's UnicodeFromCharCode mapping, which is more reliable than
// FPDFTextObj_GetText for fonts with non-standard encodings.
//
// NOTE: This returns only the first Unicode codepoint. Multi-character mappings
// (e.g. "Th" ligature → U+0054 U+0068, "ffi" → U+0066 U+0066 U+0069) are
// silently truncated. Prefer FPDFTextObj_GetCharUnicodeString for full results.
//
// Parameters:
//  - text_object: text object handle
//  - char_index:  0-based character index (0 to FPDFTextObj_CountChars-1)
//
// Returns the first Unicode codepoint, or 0 if the character has no Unicode
// mapping or on error.
FPDF_EXPORT unsigned int FPDF_CALLCONV
FPDFTextObj_GetCharUnicode(FPDF_PAGEOBJECT text_object, int char_index);

// Get the full Unicode string mapping for a character in a text object.
//
// Uses the font's UnicodeFromCharCode mapping and returns the complete result,
// including multi-character expansions for ligatures (e.g. "Th", "ffi", "ffl").
//
// Usage (two-call pattern):
//   unsigned long len = FPDFTextObj_GetCharUnicodeString(obj, idx, NULL, 0);
//   std::vector<unsigned short> buf(len);
//   FPDFTextObj_GetCharUnicodeString(obj, idx, buf.data(), len);
//   // buf now contains len UTF-16 code units (no null terminator).
//
// Parameters:
//  - text_object: text object handle
//  - char_index:  0-based character index (0 to FPDFTextObj_CountChars-1)
//  - buffer:      output buffer for UTF-16 code units, or NULL to query size
//  - buflen:      buffer capacity in unsigned shorts
//
// Returns the number of UTF-16 code units written (or that would be written
// if buffer is NULL). Returns 0 on error or if no Unicode mapping exists.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFTextObj_GetCharUnicodeString(FPDF_PAGEOBJECT text_object,
                                  int char_index,
                                  unsigned short* buffer,
                                  unsigned long buflen);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_TEXT_EXT_H_
