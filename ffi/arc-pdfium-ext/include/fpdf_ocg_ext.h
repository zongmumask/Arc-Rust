// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FPDF_OCG_EXT_H_
#define FPDF_OCG_EXT_H_

#include "fpdfview.h"
#include "fpdf_annot.h"

#ifdef __cplusplus
extern "C" {
#endif

// OCG (Optional Content Group) management functions.
//
// PDFium's public API does not expose OCG read/write operations.
// These extension functions allow creating, reading, and modifying
// PDF Optional Content Groups — the standard mechanism for annotation
// layers (as used by Drawboard PDF, Acrobat, etc.).

// Type definition for an OCG handle. Internally a CPDF_Dictionary*.
typedef struct fpdf_ocg__* FPDF_OCG;

// ---------------------------------------------------------------------------
// OCG CRUD
// ---------------------------------------------------------------------------

// Create a new OCG (layer) in the document.
// This creates an OCG dictionary, adds it to the document's
// OCProperties/OCGs array, and appends it to the Order array.
//
//   document - handle to the document
//   name     - UTF-16LE encoded layer name (must be null-terminated)
//
// Returns a handle to the new OCG, or NULL on failure.
FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_Create(FPDF_DOCUMENT document, FPDF_WIDESTRING name);

// Get the number of OCGs in the document.
//
//   document - handle to the document
//
// Returns the count, or 0 on failure.
FPDF_EXPORT int FPDF_CALLCONV
FPDFOCG_GetCount(FPDF_DOCUMENT document);

// Get the OCG at the given index (0-based).
// The order follows OCProperties/D/Order.
//
//   document - handle to the document
//   index    - 0-based index
//
// Returns a handle to the OCG, or NULL on failure.
FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_Get(FPDF_DOCUMENT document, int index);

// Delete an OCG from the document.
// Removes the OCG from OCProperties/OCGs, Order, ON/OFF arrays,
// and clears /OC references from all annotations that reference it.
//
//   document - handle to the document
//   ocg      - handle to the OCG to delete
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_Delete(FPDF_DOCUMENT document, FPDF_OCG ocg);

// ---------------------------------------------------------------------------
// OCG Properties
// ---------------------------------------------------------------------------

// Get the name of an OCG.
// |buffer| is only modified if |buflen| is longer than the name length.
//
//   ocg    - handle to the OCG
//   buffer - buffer for the UTF-16LE encoded name (may be NULL)
//   buflen - length of the buffer in bytes
//
// Returns the length of the name in bytes (including trailing zeros),
// or 0 on failure.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFOCG_GetName(FPDF_OCG ocg, FPDF_WCHAR* buffer, unsigned long buflen);

// Set the name of an OCG.
//
//   ocg  - handle to the OCG
//   name - UTF-16LE encoded new name (must be null-terminated)
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetName(FPDF_OCG ocg, FPDF_WIDESTRING name);

// ---------------------------------------------------------------------------
// OCG Visibility
// ---------------------------------------------------------------------------

// Check if an OCG is currently visible.
// Reads the OCProperties/D configuration (BaseState, ON, OFF arrays).
//
//   document - handle to the document
//   ocg      - handle to the OCG
//
// Returns TRUE if visible, FALSE if hidden or on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_IsVisible(FPDF_DOCUMENT document, FPDF_OCG ocg);

// Set the visibility of an OCG.
// Modifies the ON/OFF arrays in OCProperties/D.
//
//   document - handle to the document
//   ocg      - handle to the OCG
//   visible  - TRUE to show, FALSE to hide
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetVisible(FPDF_DOCUMENT document, FPDF_OCG ocg, FPDF_BOOL visible);

// ---------------------------------------------------------------------------
// Annotation–OCG Association
// ---------------------------------------------------------------------------

// Set the /OC (Optional Content) reference on an annotation.
// This associates the annotation with the given OCG (layer).
//
//   annot - handle to the annotation
//   ocg   - handle to the OCG (NULL to remove the /OC entry)
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetAnnotOC(FPDF_ANNOTATION annot, FPDF_OCG ocg);

// Get the /OC reference from an annotation.
// Returns the OCG dictionary that the annotation is associated with.
//
//   annot - handle to the annotation
//
// Returns a handle to the OCG, or NULL if the annotation has no /OC
// or on failure.
FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_GetAnnotOC(FPDF_ANNOTATION annot);

// ---------------------------------------------------------------------------
// OCG Order
// ---------------------------------------------------------------------------

// Reorder OCGs according to the given array of indices.
// Updates OCProperties/D/Order to reflect the new order.
//
//   document   - handle to the document
//   order      - array of 0-based OCG indices in desired order
//   count      - number of elements in the order array
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetOrder(FPDF_DOCUMENT document, const int* order, int count);

// ---------------------------------------------------------------------------
// OCG Custom Properties (Quill-specific, not part of PDF OCG standard)
// ---------------------------------------------------------------------------

// Get the lock state of an OCG.
// Lock state is stored as /QuillLocked (boolean) on the OCG dictionary.
// This is a Quill-specific extension; the PDF OCG standard has no lock concept.
//
//   ocg - handle to the OCG
//
// Returns TRUE if locked, FALSE if unlocked or on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_GetLocked(FPDF_OCG ocg);

// Set the lock state of an OCG.
// Lock state is stored as /QuillLocked (boolean) on the OCG dictionary.
//
//   ocg    - handle to the OCG
//   locked - TRUE to lock, FALSE to unlock
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetLocked(FPDF_OCG ocg, FPDF_BOOL locked);

// Get the color string of an OCG.
// Color is stored as /QuillColor (string, e.g. "FF0000") on the OCG dictionary.
// This is a Quill-specific extension; the PDF OCG standard has no color concept.
//
//   ocg    - handle to the OCG
//   buffer - buffer for the null-terminated ASCII color string (may be NULL)
//   buflen - length of the buffer in bytes
//
// Returns the length of the color string in bytes (including trailing null),
// or 0 on failure or if no color is set.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFOCG_GetColor(FPDF_OCG ocg, char* buffer, unsigned long buflen);

// Set the color string of an OCG.
// Color is stored as /QuillColor (string, e.g. "FF0000") on the OCG dictionary.
// Pass NULL to remove the color entry.
//
//   ocg   - handle to the OCG
//   color - null-terminated ASCII color string (e.g. "FF0000"), or NULL to remove
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetColor(FPDF_OCG ocg, const char* color);

// ---------------------------------------------------------------------------
// Active Layer (Quill-specific, not part of PDF OCG standard)
// ---------------------------------------------------------------------------

// The "active layer" is the layer that new annotations are assigned to on
// creation. The PDF OCG standard has no notion of an active layer, so this is
// a Quill UI preference persisted on the document catalog as a reference to
// the active OCG dictionary:
//
//     /Root /QuillActiveLayer <ocg ref>
//
// A missing (or NULL) entry means "General" - the virtual default layer whose
// annotations carry no /OC. See `LayerManager.setActiveLayer` / `loadLayers`.

// Set the active layer for the document.
// Stores a /Root /QuillActiveLayer reference to the given OCG.
// Pass NULL to clear the entry (active layer = General).
//
//   document - handle to the document
//   ocg      - handle to the OCG to make active, or NULL for General
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetActiveLayer(FPDF_DOCUMENT document, FPDF_OCG ocg);

// Get the active layer for the document.
// Reads /Root /QuillActiveLayer and resolves it to an OCG handle.
// Returns NULL if no active layer is stored (General), if the entry is not a
// valid OCG, or if the referenced OCG no longer exists in /OCProperties/OCGs
// (guards against dangling references to deleted layers).
//
//   document - handle to the document
//
// Returns a handle to the active OCG, or NULL for General / on failure.
FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_GetActiveLayer(FPDF_DOCUMENT document);

// ---------------------------------------------------------------------------
// OCG Usage States (PDF standard, per ISO 32000-1 §8.11.2)
// ---------------------------------------------------------------------------

// Get the print visibility state of an OCG.
// Reads /Usage/Print/PrintState on the OCG dictionary.
// Defaults to TRUE (visible) when no entry is present.
//
//   ocg - handle to the OCG
//
// Returns TRUE if the layer is shown when printing, FALSE otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_GetPrintState(FPDF_OCG ocg);

// Set the print visibility state of an OCG.
// Writes /Usage/Print/PrintState (/ON or /OFF) on the OCG dictionary.
//
//   ocg    - handle to the OCG
//   visible - TRUE to show when printing, FALSE to hide
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetPrintState(FPDF_OCG ocg, FPDF_BOOL visible);

// Get the export visibility state of an OCG.
// Reads /Usage/Export/ExportState on the OCG dictionary.
// Defaults to TRUE (visible) when no entry is present.
//
//   ocg - handle to the OCG
//
// Returns TRUE if the layer is shown when exporting, FALSE otherwise.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_GetExportState(FPDF_OCG ocg);

// Set the export visibility state of an OCG.
// Writes /Usage/Export/ExportState (/ON or /OFF) on the OCG dictionary.
//
//   ocg    - handle to the OCG
//   visible - TRUE to show when exporting, FALSE to hide
//
// Returns TRUE on success, FALSE on failure.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetExportState(FPDF_OCG ocg, FPDF_BOOL visible);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FPDF_OCG_EXT_H_
