#ifndef FPDF_BOOKMARK_EXT_H_
#define FPDF_BOOKMARK_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bookmark creation and manipulation functions.
// PDFium's public API only supports reading bookmarks. These extension functions
// allow creating and modifying PDF bookmarks (outlines).

// Create a new bookmark in the document.
// Parameters:
//   - document: handle to the document
//   - title: UTF-16LE encoded bookmark title (must be null-terminated)
//   - page_index: 0-based page index for the bookmark destination (-1 for no destination)
//   - top_y: Y coordinate (PDF page space, bottom-left origin) of the destination
//            within the page. Values >= 0 produce an XYZ destination that jumps
//            precisely to that position; negative values fall back to a whole-page
//            Fit destination.
//   - parent: handle to parent bookmark (NULL for top-level bookmark)
//   - insert_after: handle to sibling bookmark to insert after (NULL to insert as first child)
// Returns:
//   - Handle to the newly created bookmark, or NULL on failure
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
FPDFExt_CreateBookmark(FPDF_DOCUMENT document,
                       FPDF_WIDESTRING title,
                       int page_index,
                       double top_y,
                       FPDF_BOOKMARK parent,
                       FPDF_BOOKMARK insert_after);

// Set the title of an existing bookmark.
// Parameters:
//   - bookmark: handle to the bookmark
//   - title: UTF-16LE encoded new title (must be null-terminated)
// Returns:
//   - TRUE on success, FALSE on failure
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_SetBookmarkTitle(FPDF_BOOKMARK bookmark, FPDF_WIDESTRING title);

// Set the destination of an existing bookmark.
// Parameters:
//   - bookmark: handle to the bookmark
//   - page_index: 0-based page index (-1 to remove destination)
//   - fit_type: view fit type (PDFDEST_VIEW_XYZ = 1, PDFDEST_VIEW_FIT = 2, etc.)
//   - x, y, zoom: position and zoom parameters (meaning depends on fit_type)
// Returns:
//   - TRUE on success, FALSE on failure
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_SetBookmarkDest(FPDF_BOOKMARK bookmark,
                        int page_index,
                        int fit_type,
                        double x,
                        double y,
                        double zoom);

// Delete a bookmark from the document.
// Parameters:
//   - document: handle to the document
//   - bookmark: handle to the bookmark to delete
// Returns:
//   - TRUE on success, FALSE on failure
// Note:
//   - Child bookmarks are moved up to the parent level
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_DeleteBookmark(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark);

// Clear all bookmarks from the document.
// Parameters:
//   - document: handle to the document
// Returns:
//   - TRUE on success, FALSE on failure
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_ClearAllBookmarks(FPDF_DOCUMENT document);

// Get the number of top-level bookmarks.
// Parameters:
//   - document: handle to the document
// Returns:
//   - Number of top-level bookmarks, or 0 if none or on error
FPDF_EXPORT int FPDF_CALLCONV
FPDFExt_GetBookmarkCount(FPDF_DOCUMENT document);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_BOOKMARK_EXT_H_
