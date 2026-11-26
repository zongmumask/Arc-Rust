#ifndef FPDF_PAGEOBJ_EXT_H_
#define FPDF_PAGEOBJ_EXT_H_

#include "fpdfview.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Page Object Content Stream API
// ============================================================================

// Get the content stream index of a page object
// Parameters:
//  - page_object: page object handle
// Returns:
//  - Content stream index (0-based), or -1 if the object is newly created
//    and not yet assigned to a content stream
// Notes:
//  - The content stream index is the index in the page's Contents array
//  - For pages with a single content stream, this returns 0
//  - For newly created objects, this returns -1 (kNoContentStream)
FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetContentStream(FPDF_PAGEOBJECT page_object);

// Set the content stream index of a page object
// Parameters:
//  - page_object: page object handle
//  - content_stream_index: content stream index to set (0-based), or -1 for kNoContentStream
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - This allows preserving content stream structure when replacing objects
//  - Use -1 to mark the object as streamless (will be assigned a stream on save)
//  - This should be called before inserting the object into the page
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetContentStream(FPDF_PAGEOBJECT page_object, int content_stream_index);

// Get the ActualText value from a page object's content marks.
//
// PDF content marks can carry an /ActualText entry that provides the semantic
// Unicode text for a glyph or group of glyphs, independent of the visual
// rendering. This is used for accessibility, copy/search, and cases where the
// visual glyph (e.g. an uppercase letter) differs from the intended semantic
// text (e.g. a lowercase letter).
//
// Parameters:
//  - page_object: page object handle
//  - buffer:      UTF-16LE output buffer (may be NULL to query required size)
//  - buflen:      size of buffer in bytes
// Returns:
//  - Number of bytes required (including null terminator) if buffer is NULL or
//    too small; number of bytes written otherwise.
//  - 0 if the object has no ActualText mark or on error.
// Notes:
//  - If multiple marks carry ActualText, the last one wins (matches PDFium's
//    own text-extraction behaviour in cpdf_textpage.cpp).
//  - The returned string is UTF-16LE encoded.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFPageObj_GetActualText(FPDF_PAGEOBJECT page_object,
                          void* buffer,
                          unsigned long buflen);

// Get the number of content marks on |page_object| that carry a non-empty
// /ActualText value.
//
// Returns:
//  - Number of ActualText marks in content-stream order.
//  - -1 on error.
FPDF_EXPORT int FPDF_CALLCONV
FPDFPageObj_GetActualTextMarkCount(FPDF_PAGEOBJECT page_object);

// Get the ActualText string for the mark at |index| in content-stream order.
//
// Parameters:
//  - page_object: page object handle
//  - index:       0-based ActualText mark index
//  - buffer:      UTF-16LE output buffer (may be NULL to query required size)
//  - buflen:      size of buffer in bytes
//
// Returns:
//  - Number of bytes required (including null terminator) if buffer is NULL or
//    too small; number of bytes written otherwise.
//  - 0 if |index| is out of range or on error.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFPageObj_GetActualTextMark(FPDF_PAGEOBJECT page_object,
                              int index,
                              void* buffer,
                              unsigned long buflen);

// ============================================================================
// Image Object Transparency API
// ============================================================================

// Get a rendered bitmap of an image object with proper transparency support.
// This function is similar to FPDFImageObj_GetRenderedBitmap but ensures that:
// 1. The bitmap is initialized with a transparent background
// 2. The image's SMask (soft mask) is properly applied
// 3. Alpha channel is preserved in the output
//
// Parameters:
//  - document:     document handle
//  - page:         page handle (optional, can be NULL)
//  - image_object: image object handle
//
// Returns:
//  - BGRA bitmap with transparency, or NULL on failure
//  - Caller takes ownership and must call FPDFBitmap_Destroy()
//
// Notes:
//  - The output bitmap is always in BGRA format with alpha channel
//  - The bitmap background is initialized to transparent (0x00000000)
//  - This properly handles PDF images with SMask (transparency mask)
FPDF_EXPORT FPDF_BITMAP FPDF_CALLCONV
FPDFImageObj_GetRenderedBitmapWithTransparency(FPDF_DOCUMENT document,
                                               FPDF_PAGE page,
                                               FPDF_PAGEOBJECT image_object);

// ============================================================================
// Image Object Interpolation API
// ============================================================================

// Set or clear the image XObject's /Interpolate flag, which hints PDF renderers
// to apply high-quality interpolation when the image is scaled (matching the
// behaviour Quartz produces for image-to-PDF, i.e. PDFKit's PDFPage(image:)).
// Absent / false lets viewers resample with a fast/nearest filter, which shows
// as jagged "pixelation" along edges when a photo is enlarged.
// Parameters:
//  - page_object:  image page object handle
//  - interpolate:  TRUE to set /Interpolate true, FALSE to remove the key
// Returns:
//  - TRUE on success, FALSE if |page_object| is not an image object or its
//    image has no backing stream (e.g. inline image).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPageObj_SetInterpolate(FPDF_PAGEOBJECT page_object, FPDF_BOOL interpolate);

// ============================================================================
// Page Object Reorder API
// ============================================================================

// Reorder a page object within its page's object list (list order = z order).
// Parameters:
//  - page:         page handle
//  - page_object:  page object handle that must belong to |page|
//  - new_index:    target list position (0..N-1) in the caller's current view
//                  of the object list (i.e. before this operation removes any
//                  object). Higher index = further forward / on top.
// Returns:
//  - TRUE on success, FALSE if the handles are invalid or |page_object| is not
//    present on |page|.
// Notes:
//  - Out-of-range |new_index| is clamped; if the object is already at the
//    target position this is a no-op returning TRUE. The object is NEVER
//    destroyed or dropped on error (safe against out-of-bounds inserts).
//  - Reorders the in-memory object list, assigns the moved object to the target
//    object's content stream, and dirties both affected streams. This is needed
//    because PDF content-stream order otherwise takes precedence over list
//    order when objects cross stream boundaries.
//  - Does NOT call FPDFPage_GenerateContent. The Swift layer calls
//    Page.markContentStreamModified() + FPDFPage_GenerateContent afterwards,
//    consistent with the rest of the edit paths.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_ReorderObject(FPDF_PAGE page, FPDF_PAGEOBJECT page_object, size_t new_index);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_PAGEOBJ_EXT_H_
