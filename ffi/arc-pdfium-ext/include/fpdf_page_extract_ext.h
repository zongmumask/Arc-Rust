// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// PDF page data extraction for the Office export extensions (P1: 对标 Stirling
// extract/PageReader.read + GlyphCollector). The public FPDFText_* API offers
// text, boxes, angles and colours, but not the semantics Stirling's PageData
// needs: per-character render mode (invisible/hidden), effective font size
// (text-matrix scale, not the nominal /FontSize), font ascent/descent/space
// metrics, content order and rotation. This extension reads those from the
// text page's internal CharInfo (CPDF_TextPage) so callers can split glyphs
// into visible / hidden / rotated exactly like PageReader.build does.
//
// All geometry is PDF user space (pre-display-transform); the caller folds in
// the CropBox/rotate/fit-scale like Stirling PageReader.displayTransform.

#ifndef FPDF_PAGE_EXTRACT_EXT_H_
#define FPDF_PAGE_EXTRACT_EXT_H_

#include <stddef.h>

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Full-per-character raw info (Stirling RawGlyph + font metrics, PDF user
// space). |char_type| mirrors CPDF_TextPage::CharType (0 normal, 1 generated,
// 2 not-unicode, 3 hyphen, 4 piece, 5 actual-text).
typedef struct FPDFExt_RawGlyph {
  unsigned int unicode;
  unsigned int char_code;
  int char_type;
  int seq;
  double origin_x, origin_y;  // glyph origin (baseline start), user space
  double left, right, top, bottom;  // glyph box, user space
  double angle;   // text-matrix rotation, degrees
  double hscale;  // horizontal/vertical scale ratio (Stirling horizontalScale)
  double size;    // effective font size, user-space points (~ Stirl. fontSize())
  double ascent;      // font ascent at this size, points
  double descent;     // font descent at this size, points
  double space_width; // font space advance at this size, points
  int render_mode;    // PDF TextRenderingMode 0..7, -1 when unknown
  int invisible;      // 1 when render mode paints nothing (render mode 3/7)
} FPDFExt_RawGlyph;

// Fill |out| for character |index| (0 .. FPDFText_CountChars-1).
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphRaw(
    FPDF_TEXTPAGE text_page, int index, FPDFExt_RawGlyph* out);

// Fractional font advance for character |index|, in 1/1000 em (unscaled).
// PDFium's CPDF_Font::GetCharWidth returns an int and truncates fractional
// /Widths entries (e.g. 782.4 -> 782), which accumulates into visible 0.2-0.8pt
// column-edge drift on LaTeX PDFs. PDFBox keeps the float, so Stirling's
// getWidthDirAdj() needs the original value. Reads the font dict's /Widths
// array with GetNumberAt/GetNumber; falls back to GetCharWidth when unavailable.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphWidthEm(
    FPDF_TEXTPAGE text_page, int index, double* out_width_em);

// PDFBox-equivalent glyph origin x (same text object, replayed with original
// float /Widths instead of PDFium's truncated int widths). Kerning, word/char
// spacing and text matrix are preserved from PDFium's own text object.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphOriginX(
    FPDF_TEXTPAGE text_page, int index, double* out_x);

// TrueType-style ascent/descent from the font descriptor in 1/1000 em, as
// PDFium reports it (synthesized when absent). Lets callers distinguish a
// real 689/194 descriptor from the synthesized 716/205 default.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetGlyphFontAscent(
    FPDF_TEXTPAGE text_page, int index, double* out_ascent, double* out_descent);

// Annotation field type (/FT) for a widget annotation, walking the /Parent
// chain up to 32 levels (Stirling PageReader.fieldType). Writes one of the
// FPDFExt_FIELD_* values into |field_type|.
#define FPDFExt_FIELD_NONE 0
#define FPDFExt_FIELD_TEXT 1
#define FPDFExt_FIELD_BUTTON 2
#define FPDFExt_FIELD_CHOICE 3
#define FPDFExt_FIELD_SIGNATURE 4
#define FPDFExt_FIELD_FD 5
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Annot_GetFieldType(
    FPDF_ANNOTATION annot, int* field_type);

// TRUE when the annotation draws an appearance (AP dict has an /N normal
// stream) — Stirling PageReader.drawn().
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Annot_HasNormalAppearance(
    FPDF_ANNOTATION annot);

// DOCX font embedding (P4): copy the decoded embedded font program for a text
// character. |font_type| receives 1 (FontFile2/TrueType sfnt), 2 (FontFile3
// OpenType/CFF) or 0 (none). |out_family| receives the PDF /BaseFont name if
// non-null (malloc buffer, caller frees with free()). On success *out_bytes is
// a malloc buffer the caller must free() and *out_len its byte count.
// |max_bytes| bounds the copied program (0 = no limit).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_TextPage_GetEmbeddedFont(
    FPDF_TEXTPAGE text_page, int index, size_t max_bytes,
    unsigned char** out_bytes, size_t* out_len, int* out_font_type,
    char** out_family);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FPDF_PAGE_EXTRACT_EXT_H_