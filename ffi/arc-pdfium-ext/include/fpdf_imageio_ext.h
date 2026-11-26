// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Image <-> PDF conversion extension.
//
// PDFium exposes page editing (FPDFPage_*), rendering (FPDF_RenderPageBitmap)
// and bitmap primitives (FPDFBitmap_*), so Image <-> PDF needs no internal
// classes; these functions package the piecewise API into the two directional
// operations a consumer actually wants:
//
//   Image -> PDF:  FPDFExt_NewDocumentFromImages() builds a new document with
//                  one full-page image per input frame (page sized to the
//                  frame's physical size at 72 dpi unless overridden).
//   PDF -> Image:  FPDFExt_RenderPagesToBitmaps() renders a page range to raw
//                  BGRA FPDF_BITMAP handles at a target DPI.
//
// Both operate on raw BGR/BGRA pixels as the interchange, keeping the image
// *codec* (PNG/JPEG/WebP encode/decode) at the host boundary instead of
// pulling a decoder/encoder into the kernel. The kernel owns the page/render
// semantics; the host encodes/decodes pixels.

#ifndef FPDF_IMAGEIO_EXT_H_
#define FPDF_IMAGEIO_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// One decoded image frame to place on a PDF page.
typedef struct FPDFExt_ImageFrame {
  const unsigned char* pixels;  // BGR (3 bpp) or BGRA (4 bpp), top-down
  int width;                    // pixel columns, > 0
  int height;                   // pixel rows, > 0
  int stride;                   // bytes per row (>= width * bytes_per_pixel);
                                // 0 = tightly packed
  int has_alpha;                // 1 = BGRA, 0 = BGR
  double width_pt;              // destination page width in points; 0 = width px at 72 dpi
  double height_pt;             // destination page height in points; 0 = auto by aspect of width_pt
} FPDFExt_ImageFrame;

// Build a new document containing one page per |frames| entry, each page a
// full-page image. Page box defaults to the frame's size at 72 dpi (1 px = 1 pt),
// overridable per frame by |width_pt|/|height_pt| (a zero keeps that dimension
// auto from the other). The image is scaled (not cropped) to fill the page box.
//
//   frames       - array of |count| image frames (copied into the document)
//   count        - number of frames/pages, >= 1
//   out_document - receives the new document; caller closes with FPDF_CloseDocument()
//
// Returns TRUE on success. On failure the partially built document is closed
// and *out_document is left NULL.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_NewDocumentFromImages(
    const FPDFExt_ImageFrame* frames,
    int count,
    FPDF_DOCUMENT* out_document);

// Render the page range [first_page, last_page] (0-based; last_page = -1 means
// the last page, first_page < 0 means the first page) to one BGRA FPDF_BITMAP
// per page at |dpi| (72 = one pixel per page point). Each output bitmap's pixel
// buffer is BGRA, its rows in PDFium's native order.
//
//   out_bitmaps  - caller-owned array of capacity >= page count; each entry set
//                  to a bitmap the caller owns and releases with FPDFBitmap_Destroy()
//   out_count    - in: capacity of |out_bitmaps|; out: number of pages rendered
//
// Returns TRUE when all requested pages rendered. On partial failure the pages
// already produced are left in |out_bitmaps| (caller still owns them) and FALSE
// is returned.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_RenderPagesToBitmaps(
    FPDF_DOCUMENT document,
    int first_page,
    int last_page,
    int dpi,
    FPDF_BITMAP* out_bitmaps,
    int* out_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FPDF_IMAGEIO_EXT_H_