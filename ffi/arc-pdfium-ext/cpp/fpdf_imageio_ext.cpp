// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fpdf_imageio_ext.h"

#include <cmath>
#include <cstring>

#include "fpdf_edit.h"

// pdfium public headers don't define plain TRUE/FALSE macros in this build.
#ifndef FPDF_EXT_AA_TRUE_FALSE_
#define FPDF_EXT_AA_TRUE_FALSE_
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif

namespace {

// Bytes per pixel implied by |has_alpha|.
int BytesPerPixel(int has_alpha) {
  return has_alpha ? 4 : 3;
}

// True when an image is sized to the page at a 1:1 px:pt mapping (72 dpi).
bool AddPageFromFrame(FPDF_DOCUMENT doc,
                      int page_index,
                      const FPDFExt_ImageFrame& frame) {
  const int bpp = BytesPerPixel(frame.has_alpha);
  const int stride = frame.stride > 0 ? frame.stride : frame.width * bpp;
  if (frame.width <= 0 || frame.height <= 0 || stride < frame.width * bpp) {
    return false;
  }

  // Page box: frame's physical size at 72 dpi, with an override keeping the
  // other dimension proportional (scale by the ratio actually used on width).
  double scale = 1.0;
  if (frame.width_pt > 0) {
    scale = frame.width_pt / frame.width;
  }
  const double page_w = frame.width_pt > 0 ? frame.width_pt : frame.width;
  const double page_h =
      frame.height_pt > 0 ? frame.height_pt : frame.height * scale;

  FPDF_PAGE page = FPDFPage_New(doc, page_index, page_w, page_h);
  if (!page) {
    return false;
  }

  FPDF_BITMAP bitmap = FPDFBitmap_Create(frame.width, frame.height, frame.has_alpha);
  if (!bitmap) {
    FPDF_ClosePage(page);
    return false;
  }
  // Copy source rows (top-down, honoring |stride|) into the bitmap buffer.
  const unsigned char* src = frame.pixels;
  unsigned char* dst = static_cast<unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
  const int dst_stride = FPDFBitmap_GetStride(bitmap);
  const int row_bytes = frame.width * bpp;
  for (int y = 0; y < frame.height; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                src + static_cast<size_t>(y) * stride, static_cast<size_t>(row_bytes));
  }

  FPDF_PAGEOBJECT image_obj = FPDFPageObj_NewImageObj(doc);
  if (!image_obj) {
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    return false;
  }

  // Attach the bitmap to the image object, map it to fill the page box, insert
  // and serialize. The source bitmap must outlive FPDFPage_GenerateContent,
  // which pulls the pixel data into the content stream.
  FPDF_PAGE pages[1] = {page};
  bool ok = FPDFImageObj_SetBitmap(pages, 1, image_obj, bitmap);
  if (ok) {
    const double sx = page_w / frame.width;
    const double sy = page_h / frame.height;
    ok = FPDFImageObj_SetMatrix(image_obj, sx, 0.0, 0.0, sy, 0.0, 0.0);
  }
  if (ok) {
    FPDFPage_InsertObject(page, image_obj);
    ok = FPDFPage_GenerateContent(page);
  }

  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);
  return ok;
}

}  // namespace

extern "C" {

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_NewDocumentFromImages(const FPDFExt_ImageFrame* frames,
                              int count,
                              FPDF_DOCUMENT* out_document) {
  if (!frames || count <= 0 || !out_document) {
    return FALSE;
  }
  *out_document = nullptr;

  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  if (!doc) {
    return FALSE;
  }
  for (int i = 0; i < count; ++i) {
    if (!AddPageFromFrame(doc, i, frames[i])) {
      FPDF_CloseDocument(doc);
      return FALSE;
    }
  }
  *out_document = doc;
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_RenderPagesToBitmaps(FPDF_DOCUMENT document,
                             int first_page,
                             int last_page,
                             int dpi,
                             FPDF_BITMAP* out_bitmaps,
                             int* out_count) {
  if (!document || !out_bitmaps || !out_count) {
    return FALSE;
  }
  const int total = FPDF_GetPageCount(document);
  if (total <= 0 || dpi <= 0) {
    return FALSE;
  }
  const int lo = first_page < 0 ? 0 : first_page;
  const int hi = last_page < 0 ? total - 1 : last_page;
  if (lo < 0 || hi >= total || lo > hi) {
    return FALSE;
  }
  const int n = hi - lo + 1;
  if (*out_count < n) {
    return FALSE;
  }

  FPDF_BOOL all_ok = TRUE;
  for (int i = 0; i < n; ++i) {
    FPDF_PAGE page = FPDF_LoadPage(document, lo + i);
    FPDF_BITMAP bitmap = nullptr;
    if (page) {
      const double wpt = FPDF_GetPageWidth(page);
      const double hpt = FPDF_GetPageHeight(page);
      const int pw = static_cast<int>(wpt * dpi / 72.0 + 0.5);
      const int ph = static_cast<int>(hpt * dpi / 72.0 + 0.5);
      if (pw > 0 && ph > 0) {
        bitmap = FPDFBitmap_Create(pw, ph, /*alpha=*/1);
        if (bitmap) {
          FPDFBitmap_FillRect(bitmap, 0, 0, pw, ph, /*white=*/0xFFFFFF);
          FPDF_RenderPageBitmap(bitmap, page, 0, 0, pw, ph, /*rotate=*/0, /*flags=*/0);
        }
      }
      FPDF_ClosePage(page);
    }
    out_bitmaps[i] = bitmap;
    if (!bitmap) {
      all_ok = FALSE;
    }
  }
  *out_count = n;
  return all_ok;
}

}  // extern "C"