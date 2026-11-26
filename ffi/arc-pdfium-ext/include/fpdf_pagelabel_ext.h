// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FPDF_PAGELABEL_EXT_H_
#define FPDF_PAGELABEL_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// PDF logical page label (PageLabels) read/write extensions.
//
// PDFium's public API only exposes the read accessor FPDF_GetPageLabel and has
// no write support. These functions read/write the document catalog's
// /PageLabels number tree so label edits persist via FPDF_SaveAsCopy.

// Style of the numeric portion of a page label. Mirrors PDF's /S name:
//   noNumber   -> prefix only (no /S in the dictionary)  e.g. "Cover"
//   decimal    -> /S D      1, 2, 3...
//   upperRoman -> /S R      I, II, III...
//   lowerRoman -> /S r      i, ii, iii...
//   upperAlpha -> /S A      A, B, C...
//   lowerAlpha -> /S a      a, b, c...
typedef enum {
  FPDFPAGELABEL_NoNumber = 0,
  FPDFPAGELABEL_Decimal = 1,
  FPDFPAGELABEL_UpperRoman = 2,
  FPDFPAGELABEL_LowerRoman = 3,
  FPDFPAGELABEL_UpperAlpha = 4,
  FPDFPAGELABEL_LowerAlpha = 5,
} FPDFPAGELABEL_STYLE;

// A single contiguous page-label range. Pages [first_index, next_first_index)
// share one numbering rule; |start_number| numbers the first page of the range.
typedef struct {
  int first_index;          // 0-based first page of the range
  const char* prefix;       // /P, UTF-8, may be empty/NULL
  FPDFPAGELABEL_STYLE style;// /S
  int start_number;         // /St (default 1)
} FPDFPAGE_LabelRange;

// Replace the document's /PageLabels number tree with |ranges|.
//   count == 0 removes /PageLabels entirely (falls back to physical numbering).
//   Each label dictionary is built from a range's /P (prefix), /S (style),
//   and /St (start). NoNumber style omits /S so the page shows its prefix only.
// Returns TRUE on success. Mutates the in-memory catalog; persisted by
// FPDF_SaveAsCopy on save.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentSetPageLabels(FPDF_DOCUMENT document,
                           const FPDFPAGE_LabelRange* ranges,
                           int count);

// Read the document's /PageLabels number tree into |ranges| (sorted by
// first_index). Returns TRUE when the document has page labels. When FALSE or
// no PageLabels present, *count is 0 and *out_ranges is NULL.
// Caller must release the result with FPDF_DocumentFreePageLabelRanges.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentGetPageLabelRanges(FPDF_DOCUMENT document,
                                FPDFPAGE_LabelRange** out_ranges,
                                int* out_count);

// Free |ranges| (and its prefix strings) allocated by FPDF_DocumentGetPageLabelRanges.
FPDF_EXPORT void FPDF_CALLCONV
FPDF_DocumentFreePageLabelRanges(FPDFPAGE_LabelRange* ranges, int count);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_PAGELABEL_EXT_H_