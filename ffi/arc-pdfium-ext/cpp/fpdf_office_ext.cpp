// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fpdf_office_ext.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_image.h"
#include "core/fpdfapi/page/cpdf_imageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "fpdf_edit.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// pdfium public headers in this build don't expose plain TRUE/FALSE macros.
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

uint32_t Crc32(const unsigned char* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void AppendBe32(std::vector<unsigned char>* out, uint32_t v) {
  out->push_back(static_cast<unsigned char>(v >> 24));
  out->push_back(static_cast<unsigned char>(v >> 16));
  out->push_back(static_cast<unsigned char>(v >> 8));
  out->push_back(static_cast<unsigned char>(v));
}

void AppendChunk(std::vector<unsigned char>* out, const char type[4],
                 const std::vector<unsigned char>& data) {
  AppendBe32(out, static_cast<uint32_t>(data.size()));
  const size_t type_off = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), data.begin(), data.end());
  AppendBe32(out, Crc32(out->data() + type_off, 4 + data.size()));
}

std::vector<unsigned char> PngEncode(const unsigned char* pixels, int w_, int h_,
                                     int stride, bool has_alpha) {
  const size_t w = static_cast<size_t>(w_);
  const size_t h = static_cast<size_t>(h_);
  std::vector<unsigned char> png;
  static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  png.insert(png.end(), kSig, kSig + 8);
  const int bpp = has_alpha ? 4 : 3;
  std::vector<unsigned char> ihdr;
  AppendBe32(&ihdr, static_cast<uint32_t>(w));
  AppendBe32(&ihdr, static_cast<uint32_t>(h));
  ihdr.push_back(8);
  ihdr.push_back(has_alpha ? 6 : 2);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  AppendChunk(&png, "IHDR", ihdr);
  std::vector<unsigned char> raw;
  raw.reserve(h * (w * bpp + 1));
  for (size_t y = 0; y < h; ++y) {
    raw.push_back(0);
    const unsigned char* row = pixels + y * stride;
    for (size_t x = 0; x < w; ++x) {
      const unsigned char* p = row + x * 4;
      if (has_alpha) {
        raw.push_back(p[2]);  // BGRA -> RGBA
        raw.push_back(p[1]);
        raw.push_back(p[0]);
        raw.push_back(p[3]);
      } else {
        raw.push_back(p[2]);
        raw.push_back(p[1]);
        raw.push_back(p[0]);
      }
    }
  }
  DataVector<uint8_t> compressed =
      FlateModule::Encode(pdfium::span<const uint8_t>(raw.data(), raw.size()));
  std::vector<unsigned char> idat(compressed.begin(), compressed.end());
  AppendChunk(&png, "IDAT", idat);
  AppendChunk(&png, "IEND", std::vector<unsigned char>());
  return png;
}

void RotateBgra(const unsigned char* src, int sw, int sh, int stride, int turns,
                bool flip_h, bool flip_v, int* out_w, int* out_h,
                std::vector<unsigned char>* dst) {
  const bool swap = turns % 2 == 1;
  const int dw = swap ? sh : sw;
  const int dh = swap ? sw : sh;
  dst->assign(static_cast<size_t>(dw) * dh * 4, 0);
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) {
      int ox = x, oy = y;
      if (turns == 1) {
        int t = ox;
        ox = sh - 1 - oy;
        oy = t;
      } else if (turns == 2) {
        ox = sw - 1 - ox;
        oy = sh - 1 - oy;
      } else if (turns == 3) {
        int t = ox;
        ox = oy;
        oy = sw - 1 - t;
      }
      if (flip_h)
        ox = dw - 1 - ox;
      if (flip_v)
        oy = dh - 1 - oy;
      if (ox < 0 || ox >= dw || oy < 0 || oy >= dh)
        continue;
      unsigned char* d = dst->data() + (static_cast<size_t>(oy) * dw + ox) * 4;
      const unsigned char* s = src + (static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4);
      std::memcpy(d, s, 4);
    }
  }
  *out_w = dw;
  *out_h = dh;
}

bool FetchImageMedia(FPDF_PAGEOBJECT image, int quarter_turns, FPDF_BOOL flip_h,
                     FPDF_BOOL flip_v, std::vector<unsigned char>* media,
                     bool* is_jpeg, int* pw, int* ph) {
  media->clear();
  const int filters = FPDFImageObj_GetImageFilterCount(image);
  char filtername[64] = {0};
  if (filters == 1 && FPDFImageObj_GetImageFilter(image, 0, filtername, sizeof(filtername)) > 0 &&
      std::strcmp(filtername, "DCTDecode") == 0) {
    CPDF_PageObject* po = CPDFPageObjectFromFPDFPageObject(image);
    if (po && po->AsImage()) {
      RetainPtr<CPDF_Image> img = po->AsImage()->GetImage();
      if (img) {
        RetainPtr<const CPDF_Stream> stream = img->GetStream();
        if (stream) {
          DataVector<uint8_t> raw = stream->ReadAllRawData();
          if (!raw.empty()) {
            media->assign(raw.begin(), raw.end());
            *is_jpeg = true;
            unsigned int uw = 0, uh = 0;
            if (FPDFImageObj_GetImagePixelSize(image, &uw, &uh)) {
              *pw = static_cast<int>(uw);
              *ph = static_cast<int>(uh);
            }
            return true;
          }
        }
      }
    }
  }
  FPDF_BITMAP bmp = FPDFImageObj_GetBitmap(image);
  if (!bmp)
    return false;
  const int w = FPDFBitmap_GetWidth(bmp);
  const int h = FPDFBitmap_GetHeight(bmp);
  const int stride = FPDFBitmap_GetStride(bmp);
  const unsigned char* pixels = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bmp));
  if (!pixels || w <= 0 || h <= 0)
    return false;
  const int format = FPDFBitmap_GetFormat(bmp);
  const bool has_alpha = format == FPDFBitmap_BGRA;
  std::vector<unsigned char> oriented;
  int dw = 0, dh = 0;
  if (quarter_turns % 2 != 0 || flip_h)
    RotateBgra(pixels, w, h, stride, quarter_turns, flip_h != 0, flip_v != 0, &dw, &dh, &oriented);
  if (quarter_turns % 2 != 0 || flip_h)
    *media = PngEncode(oriented.data(), dw, dh, dw * 4, has_alpha);
  else
    *media = PngEncode(pixels, w, h, stride, has_alpha);
  *pw = dw ? dw : w;
  *ph = dh ? dh : h;
  return !media->empty();
}

}  // namespace

extern "C" {

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Page_GetCropBox(
    FPDF_PAGE page, float* llx, float* lly, float* urx, float* ury) {
  if (!page || !llx || !lly || !urx || !ury)
    return FALSE;
  CPDF_Page* pp = CPDFPageFromFPDFPage(page);
  const float w = static_cast<float>(FPDF_GetPageWidth(page));
  const float h = static_cast<float>(FPDF_GetPageHeight(page));
  if (pp) {
    RetainPtr<CPDF_Dictionary> d = pp->GetMutableDict();
    CFX_FloatRect crop = d->GetRectFor("CropBox");
    if (crop.Width() > 0 && crop.Height() > 0) {
      *llx = crop.left;
      *lly = crop.bottom;
      *urx = crop.right;
      *ury = crop.top;
      return TRUE;
    }
  }
  *llx = 0;
  *lly = 0;
  *urx = w;
  *ury = h;
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Image_GetMediaBytes(
    FPDF_PAGEOBJECT image, int quarter_turns, FPDF_BOOL flip_h, FPDF_BOOL flip_v,
    unsigned char** out_bytes, size_t* out_len, FPDF_BOOL* out_is_jpeg,
    unsigned int* out_w, unsigned int* out_h) {
  if (out_bytes)
    *out_bytes = nullptr;
  if (out_len)
    *out_len = 0;
  if (!image || !out_bytes || !out_len || !out_is_jpeg || !out_w || !out_h)
    return FALSE;
  std::vector<unsigned char> media;
  bool is_jpeg = false;
  int pw = 0, ph = 0;
  if (!FetchImageMedia(image, quarter_turns, flip_h, flip_v, &media, &is_jpeg, &pw, &ph))
    return FALSE;
  unsigned char* buf = static_cast<unsigned char*>(std::malloc(media.size()));
  if (!buf)
    return FALSE;
  std::memcpy(buf, media.data(), media.size());
  *out_bytes = buf;
  *out_len = media.size();
  *out_is_jpeg = is_jpeg ? TRUE : FALSE;
  *out_w = static_cast<unsigned int>(pw);
  *out_h = static_cast<unsigned int>(ph);
  return TRUE;
}

}  // extern "C"
