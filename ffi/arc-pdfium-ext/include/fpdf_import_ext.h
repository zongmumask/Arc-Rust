// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Office -> PDF import extension (the "reading" direction of the Image / Word /
// Excel / PPT <-> PDF matrix). Reads an OOXML container, lays its text out and
// writes a PDF via PDFium. First-cut tier 1 ships FPDFImport_FromWord (docx ->
// PDF, flowing text into pages with the standard Helvetica typeface);
// FPDFImport_FromExcel / FPDFImport_FromPptx are declared for API stability and
// currently report unimplemented.

#ifndef FPDF_IMPORT_EXT_H_
#define FPDF_IMPORT_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Options for the Import conversions. All fields optional (zero = default).
typedef struct FPDFExt_ImportOptions {
  double page_width_pt;   // output page width in points; 0 = A4
  double page_height_pt;  // output page height in points; 0 = A4
  double font_size;       // body font size in points; 0 = 11
  int flags;              // reserved, pass 0
} FPDFExt_ImportOptions;

// Convert a Word .docx byte stream to a new PDF document: extract the text
// paragraphs and flow them onto pages (styled runs/floated objects are later
// tiers; the range options do not apply to this flow layout).
//
//   data         - .docx container bytes
//   n            - length of |data|
//   options      - page size / font size; may be NULL (A4, 11 pt)
//   out_document - receives the new document; caller closes with FPDF_CloseDocument()
//
// Returns TRUE on success (a one-page document is produced even for empty
// input). On failure *out_document is left NULL.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFImport_FromWord(
    const unsigned char* data,
    size_t n,
    const FPDFExt_ImportOptions* options,
    FPDF_DOCUMENT* out_document);

// Excel .xlsx -> PDF (not yet implemented; returns FALSE at this tier).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFImport_FromExcel(
    const unsigned char* data,
    size_t n,
    const FPDFExt_ImportOptions* options,
    FPDF_DOCUMENT* out_document);

// PowerPoint .pptx -> PDF (not yet implemented; returns FALSE at this tier).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFImport_FromPptx(
    const unsigned char* data,
    size_t n,
    const FPDFExt_ImportOptions* options,
    FPDF_DOCUMENT* out_document);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FPDF_IMPORT_EXT_H_