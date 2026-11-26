// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Watermark extension. PDFium's public API cannot build the standard watermark
// envelope (a full-page Form XObject wrapped in an /Artifact tag with an
// embedded editable metadata XML). These extension functions construct that
// envelope per page and remove it again by regenerating the content stream.

#ifndef FPDF_WATERMARK_EXT_H_
#define FPDF_WATERMARK_EXT_H_

#include "fpdfview.h"
#include "fpdf_ocg_ext.h"

#ifdef __cplusplus
extern "C" {
#endif

// Add a text watermark to a page.
//
// The watermark is written as:
//   - a full-page Form XObject (BBox == page box, /Group /S /Transparency)
//     containing the text drawn once per grid cell (rotated about each cell
//     center, fill RGBA from opacity) using a standard Type1 font;
//   - a page content stream entry that invokes the Form inside an /Artifact
//     <</Subtype /Watermark /Type /Pagination>> BDC/EMC block so PDF consumers
//     can recognise and suppress it;
//   - a /PieceInfo /ADBE_CompoundType /DocSettings <xml> holding |guid| so the
//     watermark stays editable/re-identifiable across the session;
//   - an Optional Content Group named "Watermark" (see FPDFOCG_Create) returned
//     through |out_ocg| so callers can toggle its visibility.
//
//   page       - page handle
//   document   - owning document handle
//   guid       - UTF-16LE identifier for the envelope (embedded in /DocSettings)
//   text       - UTF-16LE watermark text (Latin-1 range is rendered; CJK glyphs
//                need a subsetCID font and are skipped for now)
//   font_name  - standard-14 base font, e.g. "Helvetica" (may be NULL)
//   font_size  - font size in page points
//   rotation_deg - rotation applied per cell, degrees
//   opacity    - per-text opacity in 0..1
//   red, green, blue - fill color in 0..1
//   tiles_x, tiles_y - grid dimensions (>=1). 1x1 = single placement at anchor+offset
//   anchor_x, anchor_y - single 放置的归一化锚点 (0..1)（grid 忽略）
//   offset_x, offset_y - single 放置的页面偏移 (pt)（grid 忽略）
//   on_top     - TRUE 水印盖在内容之上(默认)；FALSE 插到 Contents 开头垫在内容之下
//
// Returns the Form object number (>0) on success, 0 on failure.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_AddWatermark(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING guid,
    FPDF_WIDESTRING text,
    const char* font_name,
    float font_size,
    float rotation_deg,
    float opacity,
    float red,
    float green,
    float blue,
    int tiles_x,
    int tiles_y,
    float step_x,
    float step_y,
    float anchor_x,
    float anchor_y,
    float offset_x,
    float offset_y,
    FPDF_BOOL on_top,
    int asc_em,
    int desc_em);

// Enveloped text watermark using a caller-created font handle.
//
// The font is usually an embedded subset built with FPDFFont_CreateType0FromSubset;
// this variant lets a custom/Unicode font (non Standard-14, e.g. a CJK or
// calligraphic face) render faithfully in any viewer instead of falling back to
// a stock typeface. The envelope (Form XObject + /Artifact + OCG + /PieceInfo
// guid) matches FPDFPage_AddWatermark exactly; the Form's /F1 references the
// passed font, and the text is written as Identity-H hex CIDs.
//
//   page, document, guid, text - as FPDFPage_AddWatermark
//   font  - non-null FPDF_FONT created on |document| (subset or full). Caller
//           retains ownership and must close it with FPDFFont_Close after all
//           pages are written.
//   remaining params - as FPDFPage_AddWatermark (font_name replaced).
//
// Returns the Form object number (>0) on success, 0 on failure.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_AddWatermarkWithFont(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING guid,
    FPDF_WIDESTRING text,
    FPDF_FONT font,
    float font_size,
    float rotation_deg,
    float opacity,
    float red,
    float green,
    float blue,
    int tiles_x,
    int tiles_y,
    float step_x,
    float step_y,
    float anchor_x,
    float anchor_y,
    float offset_x,
    float offset_y,
    FPDF_BOOL on_top,
    int asc_em,
    int desc_em);

// Remove all watermark envelopes previously added by FPDFPage_AddWatermark from
// the page: drops the /Artifact-invoking content stream(s), removes the related
// /XObject Form resource(s) and the /PieceInfo metadata.
//
//   page - page handle
//
// Returns TRUE on success (or if none present).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_RemoveWatermark(FPDF_PAGE page);

// A watermark definition reconstructed from a page's envelope. Heap strings
// (guid/text/font_name) are UTF-8, must be released with FPDFPage_FreeWatermark.
typedef struct FPDFExt_Watermark {
  char* guid;         // /PieceInfo /DocSettings guid (UUID)
  char* text;         // watermark text (text watermarks only)
  char* font_name;    // standard-14 base font (text watermarks only)
  int watermark_type; // 0 = text, 1 = image
  float font_size;    // text: page points
  float rotation_deg; // degrees, reverse-clockwise
  float opacity;      // 0..1
  float red;          // 0..1
  float green;        // 0..1
  float blue;         // 0..1
  int tiles_x;        // grid columns (1 = single)
  int tiles_y;        // grid rows
  float width_pt;     // image: displayed width (points)
  float height_pt;    // image: displayed height (points)
} FPDFExt_Watermark;

// Number of distinct watermark envelopes on |page| (each prior
// FPDFPage_AddWatermark adds one). Returns 0 if none or on error.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetWatermarkCount(FPDF_PAGE page);

// Like FPDFPage_RemoveWatermark, but removes only the watermark envelope whose
// /PieceInfo guid equals |guid|; other watermarks on the same page stay intact
// (the all-watermark flavour clears every watermark on the page).
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_RemoveWatermarkByGuid(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING guid);

// Read the |index|-th watermark envelope on |page| into |out| (1-based success).
// Fields are reconstructed from the Form XObject content stream and /PieceInfo,
// so this round-trips what FPDFPage_AddWatermark wrote (text and image). Returns
// 1 on success, 0 when out of range or on error.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetWatermarkAt(FPDF_PAGE page,
                                                      int index,
                                                      FPDFExt_Watermark* out);

// Decode the image on the |index|-th (image) watermark envelope into a new BGRA
// FPDF_BITMAP (RGB composited with its /SMask alpha). Caller owns |*out| and
// must release with FPDFBitmap_Destroy. Returns 1 on success, else 0.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetWatermarkImage(FPDF_PAGE page,
                                                         int index,
                                                         FPDF_BITMAP* out);

// Release strings owned by |wm| (no-op on NULL fields).
FPDF_EXPORT void FPDF_CALLCONV FPDFPage_FreeWatermark(FPDFExt_Watermark* wm);

// Add a text watermark drawn directly into the page's main content stream,
// WITHOUT the editable envelope (no /Artifact tag, no Form XObject, no optional
// content group, no /PieceInfo guid). What's drawn is ordinary page vector
// content: it renders normally but carries nothing a watermark-remover tool can
// recognise, so the watermark can't be removed or identified later.
//
// Signature mirrors FPDFPage_AddWatermark minus |guid|/|out_ocg|. This is the
// "flatten / merge" flavour used when the user opts to lock a watermark.
//
//   page, document, text, font_name, font_size, rotation_deg, opacity,
//   red, green, blue, tiles_x, tiles_y
//   anchor_x, anchor_y, offset_x, offset_y - single 放置（同 FPDFPage_AddWatermark）
//
// Returns TRUE on success.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_AddWatermarkFlat(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING text,
    const char* font_name,
    float font_size,
    float rotation_deg,
    float opacity,
    float red,
    float green,
    float blue,
    int tiles_x,
    int tiles_y,
    float step_x,
    float step_y,
    float anchor_x,
    float anchor_y,
    float offset_x,
    float offset_y,
    int asc_em,
    int desc_em);

// Flattened text watermark using a caller-created font handle. The drawing is
// written directly into the page content stream with no editable envelope
// (mirroring FPDFPage_AddWatermarkFlat). |font| is typically an embedded subset
// built with FPDFFont_CreateType0FromSubset; the page /Resources/Font /WmF* (or
// /F1) entry references it. Caller retains the font and closes it afterwards.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_AddWatermarkWithFontFlat(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING text,
    FPDF_FONT font,
    float font_size,
    float rotation_deg,
    float opacity,
    float red,
    float green,
    float blue,
    int tiles_x,
    int tiles_y,
    float step_x,
    float step_y,
    float anchor_x,
    float anchor_y,
    float offset_x,
    float offset_y,
    int asc_em,
    int desc_em);

// Add an image watermark to a page, writing the same editable envelope as
// FPDFPage_AddWatermark (full-page Form + /Artifact BDC/EMC + OCG + /PieceInfo
// guid) but drawing a bitmap image instead of text.
//
// |image| is a caller-owned FPDF_BITMAP (BGR/BGRx/BGRA/BGRA_Premul). Each
// pixel's RGB is written as a /DeviceRGB FlateDecode image; when the bitmap
// carries alpha, a DeviceGray /SMask is added so transparency survives and
// multiplies with |opacity|. Every grid cell draws one tile centred in a rect
// of |width_pt| x |height_pt| page points, rotated by |rotation_deg| about the
// cell centre.
//
//   page, document, guid - as FPDFPage_AddWatermark (guid embedded in /DocSettings)
//   image                 - caller bitmap; copied into the object, not retained
//   width_pt, height_pt   - rendered size of one watermark tile in page points
//   rotation_deg, opacity, tiles_x, tiles_y - as FPDFPage_AddWatermark
//   anchor_x, anchor_y, offset_x, offset_y - single 放置；on_top - 图层
//
// Returns the Form object number (>0) on success, 0 on failure.
FPDF_EXPORT int FPDF_CALLCONV FPDFPage_AddImageWatermark(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING guid,
    FPDF_BITMAP image,
    float width_pt,
    float height_pt,
    float rotation_deg,
    float opacity,
    int tiles_x,
    int tiles_y,
    float step_x,
    float step_y,
    float anchor_x,
    float anchor_y,
    float offset_x,
    float offset_y,
    FPDF_BOOL on_top);

// Flattened (merge) image watermark drawn directly into the page content
// stream with no editable envelope, mirroring FPDFPage_AddWatermarkFlat.
// Signature is FPDFPage_AddImageWatermark minus |guid|/|on_top|. The watermarked
// image cannot later be identified or removed by a watermark tool.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_AddImageWatermarkFlat(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_BITMAP image,
    float width_pt,
    float height_pt,
    float rotation_deg,
    float opacity,
    int tiles_x,
    int tiles_y,
    float step_x,
    float step_y,
    float anchor_x,
    float anchor_y,
    float offset_x,
    float offset_y);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FPDF_WATERMARK_EXT_H_