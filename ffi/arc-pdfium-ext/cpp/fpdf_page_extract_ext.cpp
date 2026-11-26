// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// PDF page data extraction (P1: 对标 Stirling extract/PageReader.read +
// GlyphCollector). Reads CPDF_TextPage::CharInfo so each glyph carries what the
// public FPDFText_* API drops: render mode (invisible/hidden), effective font
// size from the text matrix, font descent/space metrics and rotation.

#include "fpdf_page_extract_ext.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdftext/cpdf_textpage.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdf_text.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;
}  // namespace

// pdfium public headers in this build don't expose plain TRUE/FALSE macros.
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

extern "C" {

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphRaw(
    FPDF_TEXTPAGE text_page, int index, FPDFExt_RawGlyph* out) {
  if (!text_page || index < 0 || !out)
    return FALSE;
  CPDF_TextPage* tp = CPDFTextPageFromFPDFTextPage(text_page);
  if (!tp || index >= tp->CountChars())
    return FALSE;
  const CPDF_TextPage::CharInfo& ci =
      tp->GetCharInfo(static_cast<size_t>(index));
  out->unicode = ci.unicode();
  out->char_code = ci.char_code();
  out->char_type = static_cast<int>(ci.char_type());
  out->seq = index;
  out->origin_x = ci.origin().x;
  out->origin_y = ci.origin().y;
  out->left = ci.char_box().left;
  out->right = ci.char_box().right;
  out->top = ci.char_box().top;
  out->bottom = ci.char_box().bottom;
  const CFX_Matrix& m = ci.matrix();
  const double szx = std::hypot(static_cast<double>(m.a), static_cast<double>(m.b));
  const double szy = std::hypot(static_cast<double>(m.c), static_cast<double>(m.d));
  out->angle = std::atan2(static_cast<double>(m.b), static_cast<double>(m.a)) * kRadToDeg;
  int dir = static_cast<int>(std::lround(out->angle)) % 360;
  if (dir < 0)
    dir += 360;
  // Effective size = text-matrix scale on the reading axis (Stirling
  // GlyphCollector.fontSize); sanity window [0.5, 400]pt, then the nominal
  // /FontSize, then the box height.
  double size = (dir % 180 != 0) ? szx : szy;
  if (size <= 0.5 || size > 400.0) {
    size = FPDFText_GetFontSize(text_page, index);
    if (size <= 0.5 || size > 400.0)
      size = std::max((out->top - out->bottom) * 1.4, 4.0);
  }
  out->size = size;
  // Stirling horizontalScale: along/across reading axis, clamped to [0.2, 5].
  const double along = (dir % 180 != 0) ? szy : szx;
  const double across = (dir % 180 != 0) ? szx : szy;
  double hscale = across > 0.5 ? along / across : 1.0;
  out->hscale = (hscale > 0.2 && hscale < 5.0) ? hscale : 1.0;

  // Stirling metrics(): defaults 0.8/0.2 em, overridden by descriptor values.
  out->ascent = 0.8 * size;
  out->descent = 0.2 * size;
  out->space_width = 0.27 * size;
  out->render_mode = -1;
  out->invisible = 0;
  const CPDF_TextObject* to = ci.text_object();
  if (!to)
    return TRUE;
  const TextRenderingMode mode = to->GetTextRenderMode();
  out->render_mode = static_cast<int>(mode);
  out->invisible = (mode == TextRenderingMode::MODE_INVISIBLE ||
                    mode == TextRenderingMode::MODE_CLIP)
                       ? 1
                       : 0;
  RetainPtr<CPDF_Font> font = to->GetFont();
  if (font) {
    // Font descriptor ascent/descent in 1000-unit space (Stirling metrics():
    // clamped to [0.5,1.2] / [0.05,0.45] em, else the 0.8/0.2 default).
    const double ac = font->GetTypeAscent() / 1000.0;
    const double dc = -font->GetTypeDescent() / 1000.0;
    if (ac >= 0.5 && ac <= 1.2)
      out->ascent = ac * size;
    if (dc >= 0.05 && dc <= 0.45)
      out->descent = dc * size;
    // Space advance = font /Widths space (1000-unit) x hscale (Stirling
    // spaceWidth), clamped to [0.15, 0.4] em.
    const uint32_t space_code = font->CharCodeFromUnicode(L' ');
    const int space_adv = space_code ? font->GetCharWidth(space_code) : 0;
    if (space_adv > 0) {
      double em = space_adv / 1000.0;
      em = std::max(0.15, std::min(0.4, em));
      out->space_width = em * size * out->hscale;
    }
  }
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphWidthEm(
    FPDF_TEXTPAGE text_page, int index, double* out_width_em) {
  if (!text_page || !out_width_em)
    return FALSE;
  CPDF_TextPage* tp = CPDFTextPageFromFPDFTextPage(text_page);
  if (!tp || index < 0 || index >= tp->CountChars())
    return FALSE;
  const CPDF_TextObject* to =
      tp->GetCharInfo(static_cast<size_t>(index)).text_object();
  if (!to)
    return FALSE;
  RetainPtr<CPDF_Font> font = to->GetFont();
  if (!font)
    return FALSE;
  // PDFType1 font /Widths are floats (e.g. 782.4) but CPDF_Font::GetCharWidth
  // returns int and truncates. PDFBox reads the raw float. Mirror PDFium's
  // LoadCharWidths indexing against the font dict, but keep the float.
  if (!font->IsCIDFont()) {
    RetainPtr<const CPDF_Array> widths = font->GetFontDict()->GetArrayFor("Widths");
    if (widths) {
      const uint32_t code = tp->GetCharInfo(static_cast<size_t>(index)).char_code();
      const size_t first = font->GetFontDict()->GetIntegerFor("FirstChar", 0);
      if (code >= first && code - first < widths->size()) {
        RetainPtr<const CPDF_Number> num = widths->GetNumberAt(code - first);
        if (num) {
          *out_width_em = num->GetNumber();
          return TRUE;
        }
      }
    }
  }
  *out_width_em = font->GetCharWidth(
      tp->GetCharInfo(static_cast<size_t>(index)).char_code());
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphOriginX(
    FPDF_TEXTPAGE text_page, int index, double* out_x) {
  if (!text_page || !out_x)
    return FALSE;
  CPDF_TextPage* tp = CPDFTextPageFromFPDFTextPage(text_page);
  if (!tp || index < 0 || index >= tp->CountChars())
    return FALSE;
  const auto& ci = tp->GetCharInfo(static_cast<size_t>(index));
  const CPDF_TextObject* to = ci.text_object();
  if (!to)
    return FALSE;
  // PDFium's CharInfo origin uses PDF font widths truncated to int. Replay the
  // same text object with the original float /Widths (PDFBox semantics),
  // preserving kerning / word & char spacing from the text object.
  RetainPtr<CPDF_Font> font = to->GetFont();
  if (!font || font->IsCIDFont())
    return FALSE;
  RetainPtr<const CPDF_Array> widths = font->GetFontDict()->GetArrayFor("Widths");
  if (!widths)
    return FALSE;
  const size_t first = font->GetFontDict()->GetIntegerFor("FirstChar", 0);
  const float fontSize = to->GetFontSize();
  const size_t n = to->CountItems();
  // Match the CharInfo origin to its text-object item: CharInfo origins are
  // page space, item origins are text space, so invert the text matrix first.
  size_t start = 0;
  double best = std::numeric_limits<double>::max();
  const CFX_Matrix& im = ci.matrix();
  const CFX_Matrix inv = im.GetInverse();
  CFX_PointF local = inv.Transform(ci.origin());
  for (size_t j = 0; j < n; ++j) {
    const auto item = to->GetItemInfo(j);
    const double d = std::fabs(item.origin_.x - local.x);
    if (d < best) {
      best = d;
      start = j;
    }
  }
  if (best > 0.5)
    return FALSE;
  double pos = 0;
  for (size_t i = 0; i <= start; ++i) {
    const uint32_t code = to->GetItemInfo(i).char_code_;
    if (code < first || code - first >= widths->size())
      return FALSE;
    const double w = widths->GetNumberAt(code - first)->GetNumber();
    if (i < start) {
      // PDFium char_positions_ already includes kerning; add only the
      // float-vs-int width delta to the existing origin.
      const int truncated = font->GetCharWidth(code);
      pos += (w - truncated) / 1000.0 * fontSize;
    }
  }
  const CFX_Matrix& m = ci.matrix();
  *out_x = ci.origin().x + pos * std::hypot(m.a, m.b);
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphFontAscent(
    FPDF_TEXTPAGE text_page, int index, double* out_ascent,
    double* out_descent) {
  if (!text_page || !out_ascent || !out_descent)
    return FALSE;
  CPDF_TextPage* tp = CPDFTextPageFromFPDFTextPage(text_page);
  if (!tp || index < 0 || index >= tp->CountChars())
    return FALSE;
  const CPDF_TextObject* to =
      tp->GetCharInfo(static_cast<size_t>(index)).text_object();
  if (!to)
    return FALSE;
  RetainPtr<CPDF_Font> font = to->GetFont();
  if (!font)
    return FALSE;
  *out_ascent = font->GetTypeAscent();
  *out_descent = -font->GetTypeDescent();
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Annot_GetFieldType(
    FPDF_ANNOTATION annot, int* field_type) {
  if (!annot || !field_type)
    return FALSE;
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx)
    return FALSE;
  const CPDF_Dictionary* dict = ctx->GetAnnotDict();
  // /FT may live on the field dict; walk up /Parent (Stirling fieldType()).
  for (int depth = 0; dict && depth < 32; ++depth) {
    ByteString ft = dict->GetNameFor("FT");
    if (ft == "Tx") {
      *field_type = FPDFExt_FIELD_TEXT;
      return TRUE;
    }
    if (ft == "Btn") {
      *field_type = FPDFExt_FIELD_BUTTON;
      return TRUE;
    }
    if (ft == "Ch") {
      *field_type = FPDFExt_FIELD_CHOICE;
      return TRUE;
    }
    if (ft == "Sig") {
      *field_type = FPDFExt_FIELD_SIGNATURE;
      return TRUE;
    }
    if (ft == "Fd") {
      *field_type = FPDFExt_FIELD_FD;
      return TRUE;
    }
    if (ft.IsEmpty())
      break;
    return FALSE;
  }
  *field_type = FPDFExt_FIELD_NONE;
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Annot_HasNormalAppearance(
    FPDF_ANNOTATION annot) {
  if (!annot)
    return FALSE;
  CPDF_AnnotContext* ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!ctx)
    return FALSE;
  const CPDF_Dictionary* dict = ctx->GetAnnotDict();
  if (!dict || !dict->KeyExist("AP"))
    return FALSE;
  const CPDF_Dictionary* ap = dict->GetDictFor("AP").Get();
  // Stirling drawn(): a normal (/N) appearance stream is present.
  return ap && ap->KeyExist("N");
}

}  // extern "C"