// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fpdf_watermark_ext.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// A grid cell origin in page space.
struct Cell {
  float x;
  float y;
};

// Build the text operand `(escaped)` for a WinAnsi/base-font string.
// Latin-1 chars are written as literal/octal escapes; code points above U+00FF
// are skipped (they need a subset-CID font, deferred). This matches the docs'
// "一期用基础字体直写文本" scoping.
ByteString BuildTextOperand(const WideString& text) {
  ByteString out = "(";
  const size_t n = text.GetLength();
  for (size_t i = 0; i < n; ++i) {
    const wchar_t c = text[i];
    if (c == 0x28) {          // (
      out += "\\(";
    } else if (c == 0x29) {   // )
      out += "\\)";
    } else if (c == 0x5c) {   // backslash
      out += "\\\\";
    } else if (c >= 0x20 && c <= 0x7E) {
      out += static_cast<char>(c);
    } else if (c >= 0x80 && c <= 0xFF) {
      out += ByteString::Format("\\%03o", static_cast<unsigned>(c & 0xFF));
    }
    // else: skip.
  }
  out += ")";
  return out;
}

// 用于测量字宽的原始字节（WinAnsi/base-font，无转义/括号），字符筛选与
// BuildTextOperand 保持一致——只统计实际会被绘制出来的字符。
ByteString BuildTextBytes(const WideString& text) {
  ByteString out;
  const size_t n = text.GetLength();
  for (size_t i = 0; i < n; ++i) {
    const wchar_t c = text[i];
    if ((c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF)) {
      out += static_cast<char>(c & 0xFF);
    }
  }
  return out;
}

// CID (Type0/Identity-H) text operand: hex-encoded 2-byte charcodes (glyph
// indexes after subsetting). Chars the font can't map are skipped, mirroring
// the Latin-1 skip in BuildTextOperand.
ByteString BuildCIDOperand(CPDF_Font* font, const WideString& text) {
  ByteString out = "<";
  const size_t n = text.GetLength();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t code = font->CharCodeFromUnicode(text[i]);
    if (code == CPDF_Font::kInvalidCharCode) {
      continue;
    }
    out += ByteString::Format("%04X", code);
  }
  out += ">";
  return out;
}

// Parse unitsPerEm ('head') and the typographic ascent/descent ('hhea') from a
// TrueType/OpenType blob, normalised to per-1000-em. These are the metrics the
// glyphs actually render with — identical whether read by FreeType, a PDF font
// or CoreText — so preview and write can centre on the same frame.
bool GetHheaMetrics(pdfium::span<const uint8_t> ttf, int* ascent, int* descent,
                    int* units_per_em) {
  if (ttf.size() < 12) {
    return false;
  }
  auto R16 = [&](size_t off) -> uint16_t {
    return static_cast<uint16_t>((ttf[off] << 8) | ttf[off + 1]);
  };
  const uint16_t num_tables = R16(4);
  if (ttf.size() < 12u + 16u * num_tables) {
    return false;
  }
  size_t head_off = 0, hhea_off = 0;
  for (uint16_t i = 0; i < num_tables; ++i) {
    const size_t rec = 12u + 16u * i;
    if (rec + 16 > ttf.size()) break;
    const bool is_head = ttf[rec] == 'h' && ttf[rec + 1] == 'e' &&
                         ttf[rec + 2] == 'a' && ttf[rec + 3] == 'd';
    const bool is_hhea = ttf[rec] == 'h' && ttf[rec + 1] == 'h' &&
                         ttf[rec + 2] == 'e' && ttf[rec + 3] == 'a';
    const uint32_t off = static_cast<uint32_t>(ttf[rec + 8]) << 24 |
                         static_cast<uint32_t>(ttf[rec + 9]) << 16 |
                         static_cast<uint32_t>(ttf[rec + 10]) << 8 |
                         ttf[rec + 11];
    if (is_head) head_off = off;
    else if (is_hhea) hhea_off = off;
  }
  if (head_off == 0 || hhea_off == 0) return false;
  if (head_off + 20 > ttf.size() || hhea_off + 8 > ttf.size()) return false;
  const uint16_t upem = R16(head_off + 18);
  if (upem == 0) return false;
  const int16_t asc = static_cast<int16_t>(R16(hhea_off + 4));
  const int16_t desc = static_cast<int16_t>(R16(hhea_off + 6));
  *units_per_em = upem;
  *ascent = static_cast<int>(std::round(static_cast<float>(asc) * 1000.0f / upem));
  *descent = static_cast<int>(std::round(static_cast<float>(desc) * 1000.0f / upem));
  return true;
}

// Per-cell centring for an embedded (subset) Type0 font: same convention as the
// stock-font branch — horizontal half-advance left, vertical centre of the
// [descent, ascent] frame. |asc_em|/|desc_em| are the caller-provided metrics
// (per-1000-em, from the SOURCE font so the preview and the written PDF agree);
// if both are 0 we fall back to the PDFium/embedded-font metrics.
void CenterCID(CPDF_Font* font,
               const WideString& text,
               float font_size,
               int asc_em,
               int desc_em,
               float* center_dx,
               float* center_dy) {
  int width_1000 = 0;
  const size_t n = text.GetLength();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t code = font->CharCodeFromUnicode(text[i]);
    if (code == CPDF_Font::kInvalidCharCode) {
      continue;
    }
    width_1000 += font->GetCharWidth(code);
  }
  if (asc_em == 0) {
    asc_em = font->GetTypeAscent();
  }
  if (desc_em == 0) {
    desc_em = font->GetTypeDescent();
  }
  const float asc_pt = static_cast<float>(asc_em) * font_size / 1000.0f;
  const float desc_pt = static_cast<float>(desc_em) * font_size / 1000.0f;
  *center_dx = -static_cast<float>(width_1000) * font_size / 1000.0f / 2.0f;
  *center_dy = -(asc_pt + desc_pt) / 2.0f;
}

// Resolve a watermark text font: either a caller-provided embedded Type0 font
// (|font|, subset via FPDFFont_CreateType0FromSubset) or a stock Standard-14
// Type1 dict (|font_name|). Produces the indirect font-object number, the Tj
// operand, and per-cell centring for the given text.
struct WmFont {
  bool ok = false;
  uint32_t objnum = 0;
  ByteString text_op = "()";
  float center_dx = 0.0f;
  float center_dy = 0.0f;
};
WmFont BuildWmFont(CPDF_Document* doc,
                   const char* font_name,
                   FPDF_FONT font,
                   const WideString& text,
                   float font_size,
                   int asc_em,
                   int desc_em) {
  WmFont wf;
  if (font) {
    CPDF_Font* wfont = CPDFFontFromFPDFFont(font);
    if (!wfont) {
      return wf;
    }
    const RetainPtr<const CPDF_Dictionary> dict = wfont->GetFontDict();
    if (!dict || dict->IsInline()) {
      return wf;
    }
    wf.objnum = dict->GetObjNum();
    wf.text_op = BuildCIDOperand(wfont, text);
    CenterCID(wfont, text, font_size, asc_em, desc_em, &wf.center_dx, &wf.center_dy);
    wf.ok = true;
    return wf;
  }
  const ByteString base_font =
      (font_name && font_name[0] != '\0') ? ByteString(font_name)
                                          : ByteString("Helvetica");
  auto f1 = doc->NewIndirect<CPDF_Dictionary>();
  f1->SetNewFor<CPDF_Name>("Type", "Font");
  f1->SetNewFor<CPDF_Name>("Subtype", "Type1");
  f1->SetNewFor<CPDF_Name>("BaseFont", base_font);
  f1->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
  wf.objnum = f1->GetObjNum();
  wf.text_op = BuildTextOperand(text);
  RetainPtr<CPDF_Font> measure =
      CPDF_Font::GetStockFont(doc, base_font.AsStringView());
  if (!measure) {
    measure = CPDF_Font::GetStockFont(doc, ByteString("Helvetica").AsStringView());
  }
  if (measure) {
    const int width_1000 =
        measure->GetStringWidth(BuildTextBytes(text).AsStringView());
    // 优先用调用方传入的源字体 hhea（与预览一致）；否则回退 PDFium 内置 Type1 度量。
    int asc = asc_em;
    int desc = desc_em;
    if (asc == 0) {
      asc = measure->GetTypeAscent();
    }
    if (desc == 0) {
      desc = measure->GetTypeDescent();
    }
    const float ascent = static_cast<float>(asc) * font_size / 1000.0f;
    const float descent = static_cast<float>(desc) * font_size / 1000.0f;
    wf.center_dx = -static_cast<float>(width_1000) * font_size / 1000.0f / 2.0f;
    wf.center_dy = -(ascent + descent) / 2.0f;
  }
  wf.ok = true;
  return wf;
}

int OddCountFor(float step, float dim);

// Grid cells centred across the page. 1x1 (single) places at the caller-provided
// anchor + offset instead of dead-centring. For a grid, the caller supplies the
// continuous cell pitch (step_x/step_y, from size & spacing); the kernel picks
// the largest odd count that fits each dimension so the centre cell's origin
// lands exactly on the page centre. step<=0 falls back to the legacy
// even-tiling (page-width / tiles) used by older callers.
std::vector<Cell> ComputeCells(float width,
                               float height,
                               int tiles_x,
                               int tiles_y,
                               float step_x,
                               float step_y,
                               float anchor_x,
                               float anchor_y,
                               float offset_x,
                               float offset_y) {
  if (tiles_x < 1) tiles_x = 1;
  if (tiles_y < 1) tiles_y = 1;
  if (tiles_x == 1 && tiles_y == 1) {
    // single：用归一化锚点×页面尺寸 + 页面偏移直接定放置点。
    return {{anchor_x * width + offset_x, anchor_y * height + offset_y}};
  }
  // grid：以连续格距居中排布；最大奇数格数压页心，四周渐变留边。
  const float sx = (step_x > 0.f) ? step_x : width / static_cast<float>(tiles_x);
  const float sy = (step_y > 0.f) ? step_y : height / static_cast<float>(tiles_y);
  const int cols = OddCountFor(sx, width);
  const int rows = OddCountFor(sy, height);
  const float start_x = (width - cols * sx) * 0.5f;
  const float start_y = (height - rows * sy) * 0.5f;
  std::vector<Cell> cells;
  cells.reserve(static_cast<size_t>(cols) * rows);
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      cells.push_back({start_x + (c + 0.5f) * sx, start_y + (r + 0.5f) * sy});
    }
  }
  return cells;
}

// Small odd n with n*step >= dim, so the grid always overlaps every edge (the
// overhang is clipped to the page box → edge tiles always show, no bare margin).
// Centre cell still lands on the page centre; step stays continuous.
int OddCountFor(float step, float dim) {
  if (step <= 0.f || dim <= 0.f) return 1;
  int n = static_cast<int>(std::ceil(dim / step));
  if (n < 1) n = 1;
  if (n % 2 == 0) ++n;
  return n;
}

// Returns true once a watermark Form name token is found in |bytes|. Fills |name|.
bool ExtractFormName(const std::string& bytes, ByteString* name) {
  const std::string token = " Do ";
  std::string::size_type do_pos = bytes.find(token);
  bool found = false;
  while (do_pos != std::string::npos) {
    std::string::size_type slash = bytes.rfind('/', do_pos);
    std::string::size_type end = do_pos;
    while (end > 0 && (bytes[end - 1] == ' ' || bytes[end - 1] == '\n')) {
      --end;
    }
    if (slash != std::string::npos && slash + 1 < end) {
      const size_t len = end - slash - 1;
      *name = ByteString(bytes.c_str() + slash + 1, len);
      found = true;
      break;
    }
    do_pos = bytes.find(token, do_pos + token.size());
  }
  return found;
}

// 返回指定 Form 资源名对应的水印 /PieceInfo DocSettings 里嵌的 guid 是否等于 |target|。
bool WmFormGuidEquals(CPDF_Dictionary* page_dict,
                      const ByteString& form_name,
                      const WideString& target) {
  if (target.IsEmpty()) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> res = page_dict->GetDictFor("Resources");
  if (!res) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> xobj = res->GetDictFor("XObject");
  if (!xobj) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> form = xobj->GetDictFor(form_name.AsStringView());
  if (!form) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> piece = form->GetDictFor("PieceInfo");
  if (!piece) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> adbe = piece->GetDictFor("ADBE_CompoundType");
  if (!adbe) {
    return false;
  }
  const WideString ds = adbe->GetUnicodeTextFor("DocSettings");
  if (ds.IsEmpty()) {
    return false;
  }
  const ByteString sb = ds.ToUTF8();
  const std::string utf8(sb.c_str(), sb.GetLength());
  const std::string kGuidKey = "guid=\"";
  std::string::size_type g = utf8.find(kGuidKey);
  if (g == std::string::npos) {
    return false;
  }
  g += kGuidKey.size();
  std::string::size_type e = utf8.find('"', g);
  if (e == std::string::npos) {
    return false;
  }
  const WideString shown = WideString::FromUTF8(ByteStringView(utf8.c_str() + g, e - g));
  return shown == target;
}

bool FindAndDropWatermarkStreams(CPDF_Document* doc,
                                 CPDF_Dictionary* page_dict,
                                 std::vector<ByteString>* dropped_forms,
                                 const WideString* only_guid = nullptr) {
  if (!doc || !page_dict) {
    return false;
  }
  RetainPtr<const CPDF_Object> contents_obj =
      page_dict->GetDirectObjectFor("Contents");
  if (!contents_obj) {
    return true;  // nothing to do
  }

  const bool as_array = contents_obj->IsArray();
  std::vector<RetainPtr<const CPDF_Stream>> kept_streams;

  auto is_watermark = [](const RetainPtr<const CPDF_Stream>& st,
                         ByteString* name) {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(st);
    acc->LoadAllDataFiltered();
    pdfium::span<const uint8_t> span = acc->GetSpan();
    std::string bytes;
    if (!span.empty()) {
      bytes.assign(reinterpret_cast<const char*>(span.data()), span.size());
    }
    const bool wm = bytes.find("/Artifact") != std::string::npos &&
                    bytes.find("/Subtype /Watermark") != std::string::npos;
    if (wm) {
      ExtractFormName(bytes, name);
    }
    return wm;
  };

  // 当 only_guid 给定时仅丢弃 guid 匹配的那个水印流；其余（他组水印）保留。
  auto should_drop = [&](const ByteString& name) {
    return only_guid == nullptr || WmFormGuidEquals(page_dict, name, *only_guid);
  };

  if (as_array) {
    RetainPtr<const CPDF_Array> arr = pdfium::WrapRetain(contents_obj->AsArray());
    for (size_t i = 0; i < arr->size(); ++i) {
      RetainPtr<const CPDF_Stream> st = arr->GetStreamAt(i);
      if (!st) {
        continue;
      }
      ByteString name;
      if (!is_watermark(st, &name)) {
        kept_streams.push_back(std::move(st));
      } else if (should_drop(name)) {
        if (!name.IsEmpty()) {
          dropped_forms->push_back(name);
        }
      } else {
        kept_streams.push_back(std::move(st));
      }
    }
    RetainPtr<CPDF_Array> mutable_arr =
        page_dict->GetMutableArrayFor("Contents");
    if (mutable_arr) {
      mutable_arr->Clear();
      for (const auto& st : kept_streams) {
        mutable_arr->AppendNew<CPDF_Reference>(doc, st->GetObjNum());
      }
    }
    if (kept_streams.empty()) {
      page_dict->RemoveFor("Contents");
    }
  } else if (contents_obj->IsStream()) {
    RetainPtr<const CPDF_Stream> st = pdfium::WrapRetain(contents_obj->AsStream());
    ByteString name;
    if (is_watermark(st, &name)) {
      if (should_drop(name)) {
        if (!name.IsEmpty()) {
          dropped_forms->push_back(name);
        }
        page_dict->RemoveFor("Contents");
      }
      // 仅对部分流删除（单流 Contents 但 guid 不匹配）也保留原流。
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Add watermark
// ---------------------------------------------------------------------------

// Core enveloped watermark writer, shared by the string-name and font-handle
// entry points. |font_name| is used only when |font| is nullptr (stock typeface);
// |font| takes precedence (embedded subset Type0).
static int AddWatermarkEnvelope(
    CPDF_Page* pPage,
    CPDF_Document* doc,
    FPDF_WIDESTRING guid,
    FPDF_WIDESTRING text,
    const char* font_name,
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
    int desc_em) {
  const CFX_SizeF& size = pPage->GetPageSize();
  const float W = size.width;
  const float H = size.height;
  if (W <= 0 || H <= 0) {
    return 0;
  }

  WideString ws_text = text ? WideStringFromFPDFWideString(text) : WideString();
  if (ws_text.IsEmpty()) {
    return 0;
  }

  WmFont wf = BuildWmFont(doc, font_name, font, ws_text, font_size, asc_em, desc_em);
  if (!wf.ok) {
    return 0;
  }
  const uint32_t fnum = wf.objnum;
  const ByteString& text_op = wf.text_op;
  const float center_dx = wf.center_dx;
  const float center_dy = wf.center_dy;

  const float alpha = std::min(1.0f, std::max(0.0f, opacity));
  const float rot = rotation_deg * 3.14159265358979f / 180.0f;
  const float cosr = std::cos(rot);
  const float sinr = std::sin(rot);

  // Composed Form stream body: one draw chunk per grid cell. Each cell's text
  // is centred in its own (post-cm rotation) coordinate frame; centring metrics
  // already computed in BuildWmFont against the resolved font.
  std::vector<Cell> cells =
      ComputeCells(W, H, tiles_x, tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x, offset_y);

  ByteString form_body;
  for (const Cell& cell : cells) {
    form_body += ByteString::Format(
        "q %.4f %.4f %.4f %.4f %.4f %.4f cm /GS1 gs "
        "%.3f %.3f %.3f rg BT /F1 %.1f Tf %.3f %.3f Td ",
        cosr, sinr, -sinr, cosr, cell.x, cell.y, red, green, blue, font_size,
        center_dx, center_dy);
    form_body += text_op;
    form_body += " Tj ET Q\n";
  }

  // XML metadata (guid) embedded for re-identification / editing.
  WideString ws_guid = guid ? WideStringFromFPDFWideString(guid) : WideString();
  WideString xml =
      L"<WatermarkSettings version=\"1.0\" guid=\"" + ws_guid + L"\">" +
      ws_text + L"</WatermarkSettings>";

  // Form 流字典必须是内联(direct)对象：CPDF_Stream 构造函数 CHECK(dict_->IsInline())。
  // 流对象本身经 NewIndirect 注册为间接对象，其字典内联即标准 PDF 结构。
  auto form_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  form_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  form_dict->SetNewFor<CPDF_Name>("Subtype", "Form");

  // Explicit identity /Matrix + conforming /Group, mirroring the UPdf envelope.
  auto matrix = pdfium::MakeRetain<CPDF_Array>();
  matrix->AppendNew<CPDF_Number>(1.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  matrix->AppendNew<CPDF_Number>(1.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  form_dict->SetFor("Matrix", matrix);

  auto bbox = pdfium::MakeRetain<CPDF_Array>();
  bbox->AppendNew<CPDF_Number>(0.0f);
  bbox->AppendNew<CPDF_Number>(0.0f);
  bbox->AppendNew<CPDF_Number>(W);
  bbox->AppendNew<CPDF_Number>(H);
  form_dict->SetFor("BBox", bbox);

  auto group = pdfium::MakeRetain<CPDF_Dictionary>();
  group->SetNewFor<CPDF_Name>("S", "Transparency");
  group->SetNewFor<CPDF_Name>("Type", "Group");
  group->SetNewFor<CPDF_Boolean>("K", false);
  form_dict->SetFor("Group", group);

  auto resources = pdfium::MakeRetain<CPDF_Dictionary>();
  auto font_res = pdfium::MakeRetain<CPDF_Dictionary>();
  font_res->SetNewFor<CPDF_Reference>("F1", doc, static_cast<unsigned>(fnum));
  resources->SetFor("Font", font_res);
  auto gs_res = pdfium::MakeRetain<CPDF_Dictionary>();
  auto gs = pdfium::MakeRetain<CPDF_Dictionary>();
  gs->SetNewFor<CPDF_Name>("Type", "ExtGState");
  gs->SetNewFor<CPDF_Number>("ca", alpha);
  gs->SetNewFor<CPDF_Number>("CA", alpha);
  gs_res->SetFor("GS1", gs);
  resources->SetFor("ExtGState", gs_res);
  form_dict->SetFor("Resources", resources);

  // UPdf-conformant /OC: an OCMD whose OCG is inline with every device intent
  // (/View /Print /Export /PageElement) set to /ON, so the watermark stays
  // visible without relying on catalog /OCProperties state.
  auto oc = pdfium::MakeRetain<CPDF_Dictionary>();
  auto ocgs = pdfium::MakeRetain<CPDF_Dictionary>();
  ocgs->SetNewFor<CPDF_String>("Name", "Watermark");
  ocgs->SetNewFor<CPDF_Name>("Type", "OCG");
  auto usage = pdfium::MakeRetain<CPDF_Dictionary>();
  auto view = pdfium::MakeRetain<CPDF_Dictionary>();
  view->SetNewFor<CPDF_Name>("ViewState", "ON");
  usage->SetFor("View", view);
  auto print = pdfium::MakeRetain<CPDF_Dictionary>();
  print->SetNewFor<CPDF_Name>("PrintState", "ON");
  usage->SetFor("Print", print);
  auto export_ = pdfium::MakeRetain<CPDF_Dictionary>();
  export_->SetNewFor<CPDF_Name>("ExportState", "ON");
  usage->SetFor("Export", export_);
  auto page_el = pdfium::MakeRetain<CPDF_Dictionary>();
  page_el->SetNewFor<CPDF_Name>("Subtype", "FG");
  usage->SetFor("PageElement", page_el);
  ocgs->SetFor("Usage", usage);
  oc->SetFor("OCGs", ocgs);
  oc->SetNewFor<CPDF_Name>("Type", "OCMD");
  form_dict->SetFor("OC", oc);

  auto piece = pdfium::MakeRetain<CPDF_Dictionary>();
  auto adbe = pdfium::MakeRetain<CPDF_Dictionary>();
  adbe->SetNewFor<CPDF_String>("DocSettings", WideStringView(xml));
  adbe->SetNewFor<CPDF_Name>("Private", "Watermark");
  piece->SetFor("ADBE_CompoundType", adbe);
  form_dict->SetFor("PieceInfo", piece);

  auto form = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(form_body.c_str(),
                          form_body.c_str() + form_body.GetLength()),
      form_dict);

  // Register in page /Resources/XObject.
  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  if (!page_res) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> xobj = page_res->GetMutableDictFor("XObject");
  if (!xobj) {
    xobj = doc->NewIndirect<CPDF_Dictionary>();
    page_res->SetNewFor<CPDF_Reference>("XObject", doc, xobj->GetObjNum());
  }
  const ByteString form_name = ByteString::Format("Wm%d", form->GetObjNum());
  xobj->SetNewFor<CPDF_Reference>(form_name, doc, form->GetObjNum());

  // Append the /Artifact-invoking content stream.
  ByteString content = "/Artifact <</Subtype /Watermark /Type /Pagination >> BDC\n";
  content += "q 1 0 0 1 0 0 cm /" + form_name + " Do Q\nEMC\n";

  RetainPtr<CPDF_Dictionary> page_dict = pPage->GetMutableDict();
  RetainPtr<const CPDF_Object> contents_obj =
      page_dict->GetDirectObjectFor("Contents");
  RetainPtr<CPDF_Array> contents_array;
  if (contents_obj && contents_obj->IsArray()) {
    contents_array = page_dict->GetMutableArrayFor("Contents");
    if (!contents_array) {
      return 0;
    }
  } else {
    contents_array = doc->NewIndirect<CPDF_Array>();
    if (contents_obj) {
      contents_array->AppendNew<CPDF_Reference>(doc, contents_obj->GetObjNum());
    }
    page_dict->SetNewFor<CPDF_Reference>("Contents", doc, contents_array->GetObjNum());
  }

  // Watermark stream in the page /Contents array: layer above(Bottom 不适用于此)
  // 或下方。on_top=true → 追加在末尾（盖在内容之上）；on_top=false → 插到开头（垫在内容之下）。
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto wm_stream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(content.c_str(),
                          content.c_str() + content.GetLength()),
      stream_dict);
  if (on_top) {
    contents_array->AppendNew<CPDF_Reference>(doc, wm_stream->GetObjNum());
  } else {
    contents_array->InsertNewAt<CPDF_Reference>(0, doc, wm_stream->GetObjNum());
  }

  return static_cast<int>(form->GetObjNum());
}

// Public enveloped entry points (stock typeface or embedded subset font).
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
    int desc_em) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!pPage || !doc || pPage->GetDocument() != doc) {
    return 0;
  }
  return AddWatermarkEnvelope(
      pPage, doc, guid, text, font_name, nullptr, font_size, rotation_deg,
      opacity, red, green, blue, tiles_x, tiles_y, step_x, step_y, anchor_x, anchor_y,
      offset_x, offset_y, on_top, asc_em, desc_em);
}

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
    int desc_em) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!pPage || !doc || pPage->GetDocument() != doc) {
    return 0;
  }
  return AddWatermarkEnvelope(
      pPage, doc, guid, text, nullptr, font, font_size, rotation_deg, opacity,
      red, green, blue, tiles_x, tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x,
      offset_y, on_top, asc_em, desc_em);
}

// ---------------------------------------------------------------------------
// Remove watermark(s)
// ---------------------------------------------------------------------------

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_RemoveWatermark(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> page_dict = pPage->GetMutableDict();
  CPDF_Document* doc = pPage->GetDocument();
  if (!doc || !page_dict) {
    return false;
  }

  std::vector<ByteString> dropped_forms;
  if (!FindAndDropWatermarkStreams(doc, page_dict.Get(), &dropped_forms)) {
    return false;
  }

  // Remove the referenced Form resources.
  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  if (page_res) {
    RetainPtr<CPDF_Dictionary> xobj = page_res->GetMutableDictFor("XObject");
    if (xobj) {
      for (const ByteString& name : dropped_forms) {
        xobj->RemoveFor(name.AsStringView());
      }
    }
  }
  return true;
}

// Remove only the watermark whose envelope guid (in its Form /PieceInfo) equals
// |guid|. Other watermarks on the same page are left intact — unlike
// FPDFPage_RemoveWatermark which clears every watermark on the page.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFPage_RemoveWatermarkByGuid(
    FPDF_PAGE page,
    FPDF_DOCUMENT document,
    FPDF_WIDESTRING guid) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> page_dict = pPage->GetMutableDict();
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || !page_dict) {
    return false;
  }
  const WideString target = guid ? WideStringFromFPDFWideString(guid) : WideString();
  if (target.IsEmpty()) {
    return true;  // 无 guid 即无可精确移除的目标，避免误清整页
  }
  std::vector<ByteString> dropped_forms;
  if (!FindAndDropWatermarkStreams(doc, page_dict.Get(), &dropped_forms, &target)) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  if (page_res) {
    RetainPtr<CPDF_Dictionary> xobj = page_res->GetMutableDictFor("XObject");
    if (xobj) {
      for (const ByteString& name : dropped_forms) {
        xobj->RemoveFor(name.AsStringView());
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Read back watermarks (PDF re-identification)
// ---------------------------------------------------------------------------

namespace {

// 颜色解析：把填充色从任意颜色空间解析成 sRGB 0..1（annot_ext 的同名 helper
// 在它自己的翻译单元里，这里复制一份避免跨文件依赖）。
bool WmResolveColorRGB(const CPDF_Color* color, float rgb[3]) {
  if (!color || color->IsNull()) {
    return false;
  }
  auto value = color->GetRGB();
  if (!value.has_value()) {
    return false;
  }
  rgb[0] = value->red;
  rgb[1] = value->green;
  rgb[2] = value->blue;
  return true;
}

// Whether the watermark Form dict is an image watermark (<Type>Image</Type> in
// /PieceInfo DocSettings). Text watermarks embed their text as the XML body;
// image watermarks carry an image XObject and can't be reconstructed as text.
bool WmFormIsImageDict(const CPDF_Dictionary* fdict) {
  if (!fdict) {
    return false;
  }
  auto piece = fdict->GetDictFor("PieceInfo");
  auto adbe = piece ? piece->GetDictFor("ADBE_CompoundType") : nullptr;
  if (!adbe) {
    return false;
  }
  WideString ds = adbe->GetUnicodeTextFor("DocSettings");
  const ByteString utf8 = ds.ToUTF8();
  const std::string body(utf8.c_str(), utf8.GetLength());
  return body.find("<Type>Image") != std::string::npos;
}

// A watermark Form whose /DocSettings marks it as an image watermark
// (<Type>Image</Type>). Image watermarks aren't (yet) reconstructible as text,
// so they're excluded from the textual watermark ledger; they remain removable
// via the /Artifact+Subtype/Watermark detection in FindAndDropWatermarkStreams
// and their OCG visibility toggle is unchanged.
bool WmFormIsImage(CPDF_Dictionary* page_dict, const ByteString& name) {
  RetainPtr<const CPDF_Dictionary> resources =
      page_dict ? page_dict->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> xobj =
      resources ? resources->GetDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Object> entry =
      xobj ? xobj->GetDirectObjectFor(name.AsStringView()) : nullptr;
  RetainPtr<const CPDF_Object> resolved = entry ? entry->GetDirect() : nullptr;
  if (!resolved || !resolved->IsStream()) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> fdict = resolved->AsStream()->GetDict();
  auto piece = fdict ? fdict->GetDictFor("PieceInfo") : nullptr;
  auto adbe = piece ? piece->GetDictFor("ADBE_CompoundType") : nullptr;
  if (!adbe) {
    return false;
  }
  WideString ds = adbe->GetUnicodeTextFor("DocSettings");
  const ByteString utf8 = ds.ToUTF8();
  const std::string body(utf8.c_str(), utf8.GetLength());
  return body.find("<Type>Image") != std::string::npos;
}

// Collect the distinct watermark Form names referenced by /Artifact waters on
// the page (mirrors the add/remove detection used by -RemoveWatermark).
// |include_images| controls whether image watermarks are surfaced; the remove
// path always scans Artifact streams directly and never uses this ledger.
bool WmCollectForms(CPDF_Document* doc,
                    CPDF_Dictionary* page_dict,
                    bool include_images,
                    std::vector<ByteString>* forms) {
  if (!doc || !page_dict) {
    return false;
  }
  RetainPtr<const CPDF_Object> contents_obj =
      page_dict->GetDirectObjectFor("Contents");
  if (!contents_obj) {
    return true;
  }

  auto collect_from = [forms, page_dict, include_images](const RetainPtr<const CPDF_Stream>& st) {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(st);
    acc->LoadAllDataFiltered();
    pdfium::span<const uint8_t> span = acc->GetSpan();
    std::string bytes;
    if (!span.empty()) {
      bytes.assign(reinterpret_cast<const char*>(span.data()), span.size());
    }
    if (bytes.find("/Artifact") == std::string::npos ||
        bytes.find("/Subtype /Watermark") == std::string::npos) {
      return;
    }
    ByteString name;
    if (!ExtractFormName(bytes, &name) || name.IsEmpty()) {
      return;
    }
    if (WmFormIsImage(page_dict, name)) {
      if (!include_images) {
        return;  // keep image watermarks out of the text-only ledger
      }
    }
    for (const ByteString& existing : *forms) {
      if (existing == name) {
        return;  // dedupe
      }
    }
    forms->push_back(name);
  };

  if (contents_obj->IsArray()) {
    RetainPtr<const CPDF_Array> arr = pdfium::WrapRetain(contents_obj->AsArray());
    for (size_t i = 0; i < arr->size(); ++i) {
      RetainPtr<const CPDF_Stream> st = arr->GetStreamAt(i);
      if (st) {
        collect_from(st);
      }
    }
  } else if (contents_obj->IsStream()) {
    collect_from(pdfium::WrapRetain(contents_obj->AsStream()));
  }
  return true;
}

char* WmDupMalloc(const ByteString& src) {
  if (src.IsEmpty()) {
    return nullptr;
  }
  char* out = reinterpret_cast<char*>(std::calloc(src.GetLength() + 1, 1));
  if (out) {
    memcpy(out, src.c_str(), src.GetLength());
  }
  return out;
}

// Extract guid="..." and the text body from the /DocSettings XML the writer
// embeds: <WatermarkSettings version="1.0" guid="...">TEXT</WatermarkSettings>.
void WmParseDocSettings(const WideString& ds,
                        WideString* guid_out,
                        WideString* text_out) {
  const ByteString sb = ds.ToUTF8();
  const std::string utf8(sb.c_str(), sb.GetLength());
  const std::string kGuidKey = "guid=\"";
  std::string::size_type g = utf8.find(kGuidKey);
  if (g != std::string::npos) {
    g += kGuidKey.size();
    std::string::size_type end = utf8.find('"', g);
    if (end != std::string::npos) {
      *guid_out = WideString::FromUTF8(ByteStringView(utf8.c_str() + g, end - g));
    }
  }
  std::string::size_type body_begin = utf8.find('>');
  std::string::size_type body_end = utf8.find("</WatermarkSettings>");
  if (body_begin != std::string::npos && body_end != std::string::npos &&
      body_begin + 1 < body_end) {
    *text_out = WideString::FromUTF8(
        ByteStringView(utf8.c_str() + body_begin + 1, body_end - body_begin - 1));
  }
}

// Extract key="value" from the /DocSettings XML, e.g. guid=".../width="...".
bool WmGetAttr(const std::string& body, const char* key, std::string* val) {
  const std::string k = std::string(key) + "=\"";
  std::string::size_type g = body.find(k);
  if (g == std::string::npos) {
    return false;
  }
  g += k.size();
  std::string::size_type e = body.find('"', g);
  if (e == std::string::npos) {
    return false;
  }
  *val = body.substr(g, e - g);
  return true;
}

}  // namespace

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetWatermarkCount(FPDF_PAGE page) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return 0;
  }
  std::vector<ByteString> forms;
  WmCollectForms(pPage->GetDocument(), pPage->GetMutableDict().Get(), /*include_images*/ true,
                       &forms);
  return static_cast<int>(forms.size());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetWatermarkAt(FPDF_PAGE page,
                                                      int index,
                                                      FPDFExt_Watermark* out) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage || !out) {
    return 0;
  }
  CPDF_Document* doc = pPage->GetDocument();
  if (!doc) {
    return 0;
  }
  std::vector<ByteString> forms;
  WmCollectForms(doc, pPage->GetMutableDict().Get(), /*include_images*/ true, &forms);
  if (index < 0 || index >= static_cast<int>(forms.size())) {
    return 0;
  }

  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  RetainPtr<CPDF_Dictionary> xobj =
      page_res ? page_res->GetMutableDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Object> xobj_entry =
      xobj ? xobj->GetDirectObjectFor(forms[index].AsStringView()) : nullptr;
  RetainPtr<const CPDF_Object> resolved =
      xobj_entry ? xobj_entry->GetDirect() : nullptr;
  if (!resolved || !resolved->IsStream()) {
    return 0;
  }
  RetainPtr<CPDF_Stream> stream(
      const_cast<CPDF_Stream*>(resolved->AsStream()));

  RetainPtr<const CPDF_Dictionary> form_dict = stream->GetDict();
  RetainPtr<const CPDF_Dictionary> res_dict =
      form_dict ? form_dict->GetDictFor("Resources") : nullptr;

  FPDFExt_Watermark wm = {};
  if (WmFormIsImageDict(form_dict.Get())) {
    // Image watermark: geometry was persisted to /DocSettings by the writer;
    // there is no text, font or ExtGState to read. Extract the saved fields.
    wm.watermark_type = 1;
    auto piece = form_dict->GetDictFor("PieceInfo");
    auto adbe = piece ? piece->GetDictFor("ADBE_CompoundType") : nullptr;
    WideString ds = adbe ? adbe->GetUnicodeTextFor("DocSettings") : WideString();
    const ByteString db = ds.ToUTF8();
    const std::string body(db.c_str(), db.GetLength());
    std::string val;
    if (WmGetAttr(body, "guid", &val) && !val.empty()) {
      wm.guid = WmDupMalloc(ByteString(val.c_str(), val.size()));
    }
    if (WmGetAttr(body, "width", &val)) wm.width_pt = strtof(val.c_str(), nullptr);
    if (WmGetAttr(body, "height", &val)) wm.height_pt = strtof(val.c_str(), nullptr);
    if (WmGetAttr(body, "rotation", &val)) wm.rotation_deg = strtof(val.c_str(), nullptr);
    if (WmGetAttr(body, "opacity", &val)) wm.opacity = strtof(val.c_str(), nullptr);
    if (WmGetAttr(body, "tiles_x", &val)) wm.tiles_x = atoi(val.c_str());
    if (WmGetAttr(body, "tiles_y", &val)) wm.tiles_y = atoi(val.c_str());
    *out = wm;
    return 1;
  }
  // guid / text from the /PieceInfo /DocSettings XML embedded by the writer.
  if (form_dict) {
    auto piece = form_dict->GetDictFor("PieceInfo");
    if (piece) {
      auto adbe = piece->GetDictFor("ADBE_CompoundType");
      if (adbe) {
        WideString ds = adbe->GetUnicodeTextFor("DocSettings");
        if (!ds.IsEmpty()) {
          WideString ws_guid, ws_text;
          WmParseDocSettings(ds, &ws_guid, &ws_text);
          wm.guid = WmDupMalloc(ws_guid.ToUTF8());
          wm.text = WmDupMalloc(ws_text.ToUTF8());
        }
      }
    }
  }
  // opacity from the /GS1 ExtGState the writer tagged each draw with.
  if (res_dict) {
    auto ext = res_dict->GetDictFor("ExtGState");
    if (ext) {
      auto gs1 = ext->GetDictFor("GS1");
      if (gs1) {
        auto ca = gs1->GetNumberFor("ca");
        if (ca) {
          wm.opacity = ca->GetNumber();
        }
      }
    }
  }

  // font name from /Resources/Font/F1 /BaseFont (F1 is an indirect reference).
  if (res_dict) {
    RetainPtr<const CPDF_Dictionary> fontdict = res_dict->GetDictFor("Font");
    RetainPtr<const CPDF_Object> f1 =
        fontdict ? fontdict->GetDirectObjectFor("F1") : nullptr;
    RetainPtr<const CPDF_Object> f1res = f1 ? f1->GetDirect() : nullptr;
    if (f1res && f1res->IsDictionary()) {
      RetainPtr<const CPDF_String> sf = f1res->AsDictionary()->GetStringFor("BaseFont");
      if (sf && !sf->GetString().IsEmpty()) {
        wm.font_name = WmDupMalloc(sf->GetString());
      }
    }
  }

  // Reconstruct style from the Form content bytes. The writer emits one fixed
  // cell chunk: "q c1 c2 c3 c4 tx ty cm /GS1 gs r g b rg BT /F1 sz Tf dx dy
  // Td (text) Tj ET Q". CPDF_Form::ParseContent doesn't carry the per-cell cm
  // (that CTM is only applied at render), so pull rotation/cell/color/size
  // straight off the operators we wrote.
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
  acc->LoadAllDataFiltered();
  pdfium::span<const uint8_t> sp = acc->GetSpan();
  std::string body;
  if (!sp.empty()) {
    body.assign(reinterpret_cast<const char*>(sp.data()), sp.size());
  }
  {
  }
  std::vector<float> cell_x;
  std::vector<float> cell_y;
  const char* p = body.c_str();
  const char kCell[] =
      "q %f %f %f %f %f %f cm /GS1 gs %f %f %f rg BT /F1 %f Tf %f %f Td";
  bool took_params = false;
  // 逐格解析：本格用 sscanf 命中 12 个参数（命中与否都无害），然后无条件跳到下一格
  // 固定前缀 "\nq "，指针单调前进、循环必然终止。别再依赖 sscanf 的 %n/used（格式串无 %n）。
  while (p && *p) {
    float v[12];
    if (std::sscanf(p, kCell, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                    &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) == 12) {
      if (!took_params) {
        wm.rotation_deg = atan2f(v[1], v[0]) * 180.0f / 3.14159265358979f;
        wm.red = v[6];
        wm.green = v[7];
        wm.blue = v[8];
        wm.font_size = v[9];
        took_params = true;
      }
      bool new_x = true, new_y = true;
      for (float x : cell_x) {
        if (fabsf(x - v[4]) < 0.5f) {
          new_x = false;
          break;
        }
      }
      for (float y : cell_y) {
        if (fabsf(y - v[5]) < 0.5f) {
          new_y = false;
          break;
        }
      }
      if (new_x) {
        cell_x.push_back(v[4]);
      }
      if (new_y) {
        cell_y.push_back(v[5]);
      }
    }
    const char* next = strstr(p, "\nq ");
    if (!next) {
      break;
    }
    p = next + 1;  // next 指向 '\n'，+1 到下一格 'q' 起点，下轮 sscanf 才能匹配 "q %f"
  }
  wm.tiles_x = static_cast<int>(std::max<size_t>(cell_x.size(), 1));
  wm.tiles_y = static_cast<int>(std::max<size_t>(cell_y.size(), 1));

  *out = wm;
  return 1;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFPage_GetWatermarkImage(FPDF_PAGE page,
                                                         int index,
                                                         FPDF_BITMAP* out) {
  if (!out) {
    return 0;
  }
  *out = nullptr;
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return 0;
  }
  CPDF_Document* doc = pPage->GetDocument();
  if (!doc) {
    return 0;
  }
  std::vector<ByteString> forms;
  WmCollectForms(doc, pPage->GetMutableDict().Get(), /*include_images*/ true,
                 &forms);
  if (index < 0 || index >= static_cast<int>(forms.size())) {
    return 0;
  }

  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  RetainPtr<CPDF_Dictionary> xobj =
      page_res ? page_res->GetMutableDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Object> entry =
      xobj ? xobj->GetDirectObjectFor(forms[index].AsStringView()) : nullptr;
  RetainPtr<const CPDF_Object> resolved = entry ? entry->GetDirect() : nullptr;
  if (!resolved || !resolved->IsStream()) {
    return 0;
  }
  RetainPtr<const CPDF_Dictionary> fdict = resolved->AsStream()->GetDict();

  // Locate the image resource (WmBuildImageFormBody emits /WmIm0).
  RetainPtr<const CPDF_Dictionary> f_res =
      fdict ? fdict->GetDictFor("Resources") : nullptr;
  RetainPtr<const CPDF_Dictionary> f_xobj =
      f_res ? f_res->GetDictFor("XObject") : nullptr;
  RetainPtr<const CPDF_Object> img_entry =
      f_xobj ? f_xobj->GetDirectObjectFor("WmIm0") : nullptr;
  RetainPtr<const CPDF_Object> img_resolved =
      img_entry ? img_entry->GetDirect() : nullptr;
  if (!img_resolved || !img_resolved->IsStream()) {
    return 0;
  }
  RetainPtr<CPDF_Stream> img_stream(
      const_cast<CPDF_Stream*>(img_resolved->AsStream()));
  RetainPtr<const CPDF_Dictionary> img_dict = img_stream->GetDict();
  const int W = img_dict ? img_dict->GetIntegerFor("Width") : 0;
  const int H = img_dict ? img_dict->GetIntegerFor("Height") : 0;
  if (W <= 0 || H <= 0) {
    return 0;
  }

  auto rgb_acc = pdfium::MakeRetain<CPDF_StreamAcc>(img_stream);
  rgb_acc->LoadAllDataFiltered();
  const pdfium::span<const uint8_t> rgb = rgb_acc->GetSpan();
  if (rgb.size() < static_cast<size_t>(W) * H * 3) {
    return 0;
  }

  // /SMask alpha (DeviceGray) if present.
  std::vector<uint8_t> alpha;
  RetainPtr<const CPDF_Object> smask_ref =
      img_dict ? img_dict->GetObjectFor("SMask") : nullptr;
  if (smask_ref) {
    RetainPtr<const CPDF_Object> sm = smask_ref->GetDirect();
    if (sm && sm->IsStream()) {
      RetainPtr<CPDF_Stream> sm_stream(
          const_cast<CPDF_Stream*>(sm->AsStream()));
      auto sm_acc = pdfium::MakeRetain<CPDF_StreamAcc>(sm_stream);
      sm_acc->LoadAllDataFiltered();
      const pdfium::span<const uint8_t> smap = sm_acc->GetSpan();
      if (smap.size() >= static_cast<size_t>(W) * H) {
        alpha.assign(smap.begin(), smap.begin() + static_cast<size_t>(W) * H);
      }
    }
  }
  const bool has_alpha = !alpha.empty();

  // Composite into a BGRA FPDF_BITMAP (FPDFBitmap_Create with alpha expects
  // BGRA, top-down).
  FPDF_BITMAP bmp = FPDFBitmap_Create(W, H, 1);
  if (!bmp) {
    return 0;
  }
  uint8_t* dst = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bmp));
  if (!dst) {
    FPDFBitmap_Destroy(bmp);
    return 0;
  }
  const int stride = FPDFBitmap_GetStride(bmp);
  for (int y = 0; y < H; ++y) {
    const uint8_t* src_row = rgb.data() + static_cast<size_t>(y) * (W * 3);
    uint8_t* dst_row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < W; ++x) {
      const size_t o = static_cast<size_t>(x) * 3;
      const size_t d = static_cast<size_t>(x) * 4;
      dst_row[d + 0] = src_row[o + 2];  // B
      dst_row[d + 1] = src_row[o + 1];  // G
      dst_row[d + 2] = src_row[o + 0];  // R
      dst_row[d + 3] =
          has_alpha ? alpha[static_cast<size_t>(y) * W + x] : 255;
    }
  }
  *out = bmp;
  return 1;
}

FPDF_EXPORT void FPDF_CALLCONV FPDFPage_FreeWatermark(FPDFExt_Watermark* wm) {
  if (!wm) {
    return;
  }
  free(wm->guid);
  free(wm->text);
  free(wm->font_name);
  wm->guid = nullptr;
  wm->text = nullptr;
  wm->font_name = nullptr;
}
// ---------------------------------------------------------------------------
// Flattened watermark: draw into the page content stream with no envelope.
// ---------------------------------------------------------------------------

// Core flattened watermark writer, shared by the string-name and font-handle
// entry points. |font_name| used only when |font| is nullptr; |font| takes
// precedence (embedded subset Type0).
static bool AddWatermarkFlatCore(
    CPDF_Page* pPage,
    CPDF_Document* doc,
    FPDF_WIDESTRING text,
    const char* font_name,
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
    int desc_em) {
  const CFX_SizeF& size = pPage->GetPageSize();
  const float W = size.width;
  const float H = size.height;
  if (W <= 0 || H <= 0) {
    return false;
  }
  WideString ws_text = text ? WideStringFromFPDFWideString(text) : WideString();
  if (ws_text.IsEmpty()) {
    return false;
  }

  WmFont wf = BuildWmFont(doc, font_name, font, ws_text, font_size, asc_em, desc_em);
  if (!wf.ok) {
    return false;
  }
  const uint32_t fnum = wf.objnum;
  const ByteString& text_op = wf.text_op;
  const float center_dx = wf.center_dx;
  const float center_dy = wf.center_dy;

  const float alpha = std::min(1.0f, std::max(0.0f, opacity));
  const float rot = rotation_deg * 3.14159265358979f / 180.0f;
  const float cosr = std::cos(rot);
  const float sinr = std::sin(rot);

  const std::vector<Cell> cells =
      ComputeCells(W, H, tiles_x, tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x, offset_y);

  // Unique per-page resource names, collision-free for a normal page.
  const ByteString fName = ByteString::Format("WmF%d", fnum);
  const ByteString gsName = ByteString::Format("WmGs%d", fnum);

  ByteString body;
  for (const Cell& cell : cells) {
    body += ByteString::Format(
        "q %.4f %.4f %.4f %.4f %.4f %.4f cm /%s gs "
        "%.3f %.3f %.3f rg BT /%s %.1f Tf %.3f %.3f Td ",
        cosr, sinr, -sinr, cosr, cell.x, cell.y, gsName.c_str(),
        red, green, blue, fName.c_str(), font_size, center_dx, center_dy);
    body += text_op;
    body += " Tj ET Q\n";
  }

  // Register page resources (font indirect, extgstate inline).
  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  if (!page_res) {
    return false;
  }
  RetainPtr<CPDF_Dictionary> fontres = page_res->GetMutableDictFor("Font");
  fontres->SetNewFor<CPDF_Reference>(fName, doc, fnum);
  RetainPtr<CPDF_Dictionary> ext = page_res->GetMutableDictFor("ExtGState");
  auto gs = pdfium::MakeRetain<CPDF_Dictionary>();
  gs->SetNewFor<CPDF_Name>("Type", "ExtGState");
  gs->SetNewFor<CPDF_Number>("ca", alpha);
  gs->SetNewFor<CPDF_Number>("CA", alpha);
  ext->SetFor(gsName, gs);

  // Append the drawing as an ordinary page content stream (no /Artifact).
  RetainPtr<CPDF_Dictionary> page_dict = pPage->GetMutableDict();
  RetainPtr<const CPDF_Object> contents_obj = page_dict->GetDirectObjectFor("Contents");
  RetainPtr<CPDF_Array> contents_array;
  if (contents_obj && contents_obj->IsArray()) {
    contents_array = page_dict->GetMutableArrayFor("Contents");
    if (!contents_array) {
      return false;
    }
  } else {
    contents_array = doc->NewIndirect<CPDF_Array>();
    if (contents_obj) {
      contents_array->AppendNew<CPDF_Reference>(doc, contents_obj->GetObjNum());
    }
    page_dict->SetNewFor<CPDF_Reference>("Contents", doc, contents_array->GetObjNum());
  }
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto wm_stream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(body.c_str(), body.c_str() + body.GetLength()),
      stream_dict);
  contents_array->AppendNew<CPDF_Reference>(doc, wm_stream->GetObjNum());

  return true;
}

// ---- Public flattened entry points (stock typeface or embedded subset font). ----
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
    int desc_em) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!pPage || !doc || pPage->GetDocument() != doc) {
    return false;
  }
  return AddWatermarkFlatCore(pPage, doc, text, font_name, nullptr, font_size,
                              rotation_deg, opacity, red, green, blue, tiles_x,
                              tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x, offset_y,
                              asc_em, desc_em);
}

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
    int desc_em) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!pPage || !doc || pPage->GetDocument() != doc) {
    return false;
  }
  return AddWatermarkFlatCore(pPage, doc, text, nullptr, font, font_size,
                              rotation_deg, opacity, red, green, blue, tiles_x,
                              tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x, offset_y,
                              asc_em, desc_em);
}

// ---------------------------------------------------------------------------
// Image watermarks (enveloped + flat). Same envelope as the text flavour, but
// the Form resources carry an image XObject (FlateDecode DeviceRGB + SMask
// alpha) and each grid cell places one tile with a rotation/scaling cm.
// ---------------------------------------------------------------------------

namespace {

// Decoded pixel payload of a caller bitmap: 24-bit RGB + optional alpha.
struct WmBitmapData {
  int width = 0;
  int height = 0;
  bool has_alpha = false;
  DataVector<uint8_t> rgb;    // width*height*3
  DataVector<uint8_t> alpha;  // width*height (when has_alpha)
};

bool WmExtractBitmap(const FPDF_BITMAP& bmp, WmBitmapData* out) {
  if (!bmp || !out) {
    return false;
  }
  const int W = FPDFBitmap_GetWidth(bmp);
  const int H = FPDFBitmap_GetHeight(bmp);
  if (W < 1 || H < 1) {
    return false;
  }
  const int format = FPDFBitmap_GetFormat(bmp);
  const int stride = FPDFBitmap_GetStride(bmp);
  const uint8_t* buf = static_cast<const uint8_t*>(
      FPDFBitmap_GetBuffer(bmp));
  if (!buf) {
    return false;
  }
  const bool has_alpha = (format == FPDFBitmap_BGRA ||
                          format == FPDFBitmap_BGRA_Premul);
  const bool four_bpp = (format != FPDFBitmap_BGR);
  if (format != FPDFBitmap_BGR && format != FPDFBitmap_BGRx &&
      format != FPDFBitmap_BGRA && format != FPDFBitmap_BGRA_Premul) {
    return false;  // Gray/Unknown unsupported here.
  }
  const bool premul = (format == FPDFBitmap_BGRA_Premul);
  const int bpp = four_bpp ? 4 : 3;

  out->width = W;
  out->height = H;
  out->has_alpha = has_alpha;
  out->rgb.resize(static_cast<size_t>(W) * H * 3);
  if (has_alpha) {
    out->alpha.resize(static_cast<size_t>(W) * H);
  }

  for (int y = 0; y < H; ++y) {
    const uint8_t* row = buf + static_cast<size_t>(y) * stride;
    for (int x = 0; x < W; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * bpp;
      const size_t o = static_cast<size_t>(y) * W + x;
      uint8_t r = px[2];
      uint8_t g = px[1];
      uint8_t b = px[0];
      uint8_t a = px[3];
      if (premul && a > 0) {
        // Un-premultiply so the /SMask + plain RGB reproduce the source alpha.
        r = static_cast<uint8_t>((static_cast<unsigned>(r) * 255 + a / 2) / a);
        g = static_cast<uint8_t>((static_cast<unsigned>(g) * 255 + a / 2) / a);
        b = static_cast<uint8_t>((static_cast<unsigned>(b) * 255 + a / 2) / a);
      }
      out->rgb[o * 3 + 0] = r;
      out->rgb[o * 3 + 1] = g;
      out->rgb[o * 3 + 2] = b;
      if (has_alpha) {
        out->alpha[o] = a;
      }
    }
  }
  return true;
}

// One image-watermark draw chunk per grid cell. The image XObject has no
// /Matrix, so per the PDF model it occupies a 1x1 user unit; each chunk
// "q m.. cm /WmIm0 Do Q" scales that unit rect to a width_pt x height_pt
// tile centred at (c.x, c.y) and rotated about that centre.
ByteString WmBuildImageFormBody(const std::vector<Cell>& cells,
                                float rotation_deg,
                                float width_pt,
                                float height_pt) {
  const float rad = rotation_deg * 3.14159265358979f / 180.0f;
  const float cosr = std::cos(rad);
  const float sinr = std::sin(rad);
  const float sx = width_pt;
  const float sy = height_pt;
  const float hw = 0.5f;
  const float hh = 0.5f;
  ByteString body;
  for (const Cell& c : cells) {
    const float m00 = cosr * sx;
    const float m01 = sinr * sx;
    const float m10 = -sinr * sy;
    const float m11 = cosr * sy;
    const float tx = c.x - m00 * hw - m10 * hh;
    const float ty = c.y - m01 * hw - m11 * hh;
    body += ByteString::Format(
        "q %.4f %.4f %.4f %.4f %.4f %.4f cm /WmIm0 Do Q\n",
        m00, m01, m10, m11, tx, ty);
  }
  return body;
}

// Build the image XObject (FlateDecode DeviceRGB) with an SMask alpha when
// present. Moves |bitmap|'s own DataVectors into newly allocated streams.
RetainPtr<CPDF_Stream> WmCreateImageStream(CPDF_Document* doc,
                                           WmBitmapData* bitmap,
                                           float opacity) {
  const DataVector<uint8_t> rgb_enc =
      FlateModule::Encode(pdfium::span<const uint8_t>(bitmap->rgb.data(),
                                                       bitmap->rgb.size()));
  auto image_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  image_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  image_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
  image_dict->SetNewFor<CPDF_Number>("Width", bitmap->width);
  image_dict->SetNewFor<CPDF_Number>("Height", bitmap->height);
  image_dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceRGB");
  image_dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
  image_dict->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
  auto image = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(rgb_enc), image_dict);

  if (bitmap->has_alpha && !bitmap->alpha.empty()) {
    // Global opacity is folded into the SMask (mirroring UPdf), so the Form
    // carries no ExtGState and transparency comes from the group + mask alone.
    std::vector<uint8_t> tinted(bitmap->alpha.size());
    float o = opacity;
    if (o < 0.f) o = 0.f;
    if (o > 1.f) o = 1.f;
    for (size_t i = 0; i < bitmap->alpha.size(); ++i) {
      tinted[i] = static_cast<uint8_t>(bitmap->alpha[i] * o);
    }
    const DataVector<uint8_t> alpha_enc =
        FlateModule::Encode(pdfium::span<const uint8_t>(tinted.data(),
                                                         tinted.size()));
    auto alpha_dict = pdfium::MakeRetain<CPDF_Dictionary>();
    alpha_dict->SetNewFor<CPDF_Name>("Type", "XObject");
    alpha_dict->SetNewFor<CPDF_Name>("Subtype", "Image");
    alpha_dict->SetNewFor<CPDF_Number>("Width", bitmap->width);
    alpha_dict->SetNewFor<CPDF_Number>("Height", bitmap->height);
    alpha_dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceGray");
    alpha_dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
    alpha_dict->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
    auto alpha = doc->NewIndirect<CPDF_Stream>(
        DataVector<uint8_t>(alpha_enc), alpha_dict);
    image_dict->SetNewFor<CPDF_Reference>("SMask", doc, alpha->GetObjNum());
  }
  return image;
}

}  // namespace

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
    FPDF_BOOL on_top) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!pPage || !doc || pPage->GetDocument() != doc) {
    return 0;
  }
  const CFX_SizeF& size = pPage->GetPageSize();
  const float W = size.width;
  const float H = size.height;
  if (W <= 0 || H <= 0 || width_pt <= 0 || height_pt <= 0) {
    return 0;
  }
  WmBitmapData bitmap;
  if (!WmExtractBitmap(image, &bitmap)) {
    return 0;
  }
  const float alpha = std::min(1.0f, std::max(0.0f, opacity));

  RetainPtr<CPDF_Stream> image_stream =
      WmCreateImageStream(doc, &bitmap, alpha);

  // Per-cell draw chunks.
  const std::vector<Cell> cells =
      ComputeCells(W, H, tiles_x, tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x, offset_y);
  const ByteString form_body =
      WmBuildImageFormBody(cells, rotation_deg, width_pt, height_pt);

  // /PieceInfo XML (guid + geometry so the app can rebuild a faithful image
  // watermark definition on reload).
  WideString ws_guid = guid ? WideStringFromFPDFWideString(guid) : WideString();
  const ByteString xml_bytes = ByteString::Format(
      "<WatermarkSettings version=\"1.0\" guid=\"%s\" width=\"%.4f\" "
      "height=\"%.4f\" rotation=\"%.4f\" opacity=\"%.4f\" tiles_x=\"%d\" "
      "tiles_y=\"%d\"><Type>Image</Type></WatermarkSettings>",
      ws_guid.ToUTF8().c_str(), width_pt, height_pt, rotation_deg, alpha,
      tiles_x, tiles_y);
  WideString xml = WideString::FromUTF8(xml_bytes.AsStringView());

  // Form stream (dict must be inline: CPDF_Stream CHECK(dict_->IsInline())).
  auto form_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  form_dict->SetNewFor<CPDF_Name>("Type", "XObject");
  form_dict->SetNewFor<CPDF_Name>("Subtype", "Form");
  // Explicit identity /Matrix + conforming /Group, mirroring the UPdf envelope.
  auto matrix = pdfium::MakeRetain<CPDF_Array>();
  matrix->AppendNew<CPDF_Number>(1.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  matrix->AppendNew<CPDF_Number>(1.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  matrix->AppendNew<CPDF_Number>(0.0f);
  form_dict->SetFor("Matrix", matrix);
  auto bbox = pdfium::MakeRetain<CPDF_Array>();
  bbox->AppendNew<CPDF_Number>(0.0f);
  bbox->AppendNew<CPDF_Number>(0.0f);
  bbox->AppendNew<CPDF_Number>(W);
  bbox->AppendNew<CPDF_Number>(H);
  form_dict->SetFor("BBox", bbox);
  auto group = pdfium::MakeRetain<CPDF_Dictionary>();
  group->SetNewFor<CPDF_Name>("S", "Transparency");
  group->SetNewFor<CPDF_Name>("Type", "Group");
  group->SetNewFor<CPDF_Boolean>("K", false);
  form_dict->SetFor("Group", group);

  auto resources = pdfium::MakeRetain<CPDF_Dictionary>();
  auto xobj_res = pdfium::MakeRetain<CPDF_Dictionary>();
  xobj_res->SetNewFor<CPDF_Reference>("WmIm0", doc, image_stream->GetObjNum());
  resources->SetFor("XObject", xobj_res);
  form_dict->SetFor("Resources", resources);

  // UPdf-conformant /OC: an OCMD whose OCG is inline with every device intent
  // (/View /Print /Export /PageElement) set to /ON, so the watermark stays
  // visible without relying on catalog /OCProperties state.
  auto oc = pdfium::MakeRetain<CPDF_Dictionary>();
  auto ocgs = pdfium::MakeRetain<CPDF_Dictionary>();
  ocgs->SetNewFor<CPDF_String>("Name", "Watermark");
  ocgs->SetNewFor<CPDF_Name>("Type", "OCG");
  auto usage = pdfium::MakeRetain<CPDF_Dictionary>();
  auto view = pdfium::MakeRetain<CPDF_Dictionary>();
  view->SetNewFor<CPDF_Name>("ViewState", "ON");
  usage->SetFor("View", view);
  auto print = pdfium::MakeRetain<CPDF_Dictionary>();
  print->SetNewFor<CPDF_Name>("PrintState", "ON");
  usage->SetFor("Print", print);
  auto export_ = pdfium::MakeRetain<CPDF_Dictionary>();
  export_->SetNewFor<CPDF_Name>("ExportState", "ON");
  usage->SetFor("Export", export_);
  auto page_el = pdfium::MakeRetain<CPDF_Dictionary>();
  page_el->SetNewFor<CPDF_Name>("Subtype", "FG");
  usage->SetFor("PageElement", page_el);
  ocgs->SetFor("Usage", usage);
  oc->SetFor("OCGs", ocgs);
  oc->SetNewFor<CPDF_Name>("Type", "OCMD");
  form_dict->SetFor("OC", oc);
  auto piece = pdfium::MakeRetain<CPDF_Dictionary>();
  auto adbe = pdfium::MakeRetain<CPDF_Dictionary>();
  adbe->SetNewFor<CPDF_String>("DocSettings", WideStringView(xml));
  adbe->SetNewFor<CPDF_Name>("Private", "Watermark");
  piece->SetFor("ADBE_CompoundType", adbe);
  form_dict->SetFor("PieceInfo", piece);

  auto form = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(form_body.c_str(),
                          form_body.c_str() + form_body.GetLength()),
      form_dict);

  // Register in page /Resources/XObject.
  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  if (!page_res) {
    return 0;
  }
  RetainPtr<CPDF_Dictionary> xobj = page_res->GetMutableDictFor("XObject");
  if (!xobj) {
    xobj = doc->NewIndirect<CPDF_Dictionary>();
    page_res->SetNewFor<CPDF_Reference>("XObject", doc, xobj->GetObjNum());
  }
  const ByteString form_name = ByteString::Format("Wm%d", form->GetObjNum());
  xobj->SetNewFor<CPDF_Reference>(form_name, doc, form->GetObjNum());

  // Append the /Artifact-invoking content stream (same envelope as text).
  ByteString content = "/Artifact <</Subtype /Watermark /Type /Pagination >> BDC\n";
  content += "q 1 0 0 1 0 0 cm /" + form_name + " Do Q\nEMC\n";

  RetainPtr<CPDF_Dictionary> page_dict = pPage->GetMutableDict();
  RetainPtr<const CPDF_Object> contents_obj =
      page_dict->GetDirectObjectFor("Contents");
  RetainPtr<CPDF_Array> contents_array;
  if (contents_obj && contents_obj->IsArray()) {
    contents_array = page_dict->GetMutableArrayFor("Contents");
    if (!contents_array) {
      return 0;
    }
  } else {
    contents_array = doc->NewIndirect<CPDF_Array>();
    if (contents_obj) {
      contents_array->AppendNew<CPDF_Reference>(doc, contents_obj->GetObjNum());
    }
    page_dict->SetNewFor<CPDF_Reference>("Contents", doc, contents_array->GetObjNum());
  }
  // Watermark stream in the page /Contents array. on_top → 末尾(盖内容之上)；
  // 否则插到开头(垫内容之下)。
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto wm_stream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(content.c_str(),
                          content.c_str() + content.GetLength()),
      stream_dict);
  if (on_top) {
    contents_array->AppendNew<CPDF_Reference>(doc, wm_stream->GetObjNum());
  } else {
    contents_array->InsertNewAt<CPDF_Reference>(0, doc, wm_stream->GetObjNum());
  }

  return static_cast<int>(form->GetObjNum());
}

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
    float offset_y) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!pPage || !doc || pPage->GetDocument() != doc) {
    return false;
  }
  const CFX_SizeF& size = pPage->GetPageSize();
  const float W = size.width;
  const float H = size.height;
  if (W <= 0 || H <= 0 || width_pt <= 0 || height_pt <= 0) {
    return false;
  }
  WmBitmapData bitmap;
  if (!WmExtractBitmap(image, &bitmap)) {
    return false;
  }
  const float alpha = std::min(1.0f, std::max(0.0f, opacity));

  RetainPtr<CPDF_Stream> image_stream =
      WmCreateImageStream(doc, &bitmap, alpha);

  const std::vector<Cell> cells =
      ComputeCells(W, H, tiles_x, tiles_y, step_x, step_y, anchor_x, anchor_y, offset_x, offset_y);
  const ByteString body =
      WmBuildImageFormBody(cells, rotation_deg, width_pt, height_pt);

  // Register in the page resources with collision-free names.
  RetainPtr<CPDF_Dictionary> page_res = pPage->GetMutablePageResources();
  if (!page_res) {
    return false;
  }
  const uint32_t onum = image_stream->GetObjNum();
  const ByteString imName = ByteString::Format("WmIm%d", onum);
  RetainPtr<CPDF_Dictionary> xobj = page_res->GetMutableDictFor("XObject");
  xobj->SetNewFor<CPDF_Reference>(imName, doc, onum);

  // Rewrite the draw chunks to the page-level image name, then append as an
  // ordinary (non-/Artifact) content stream.
  ByteString inlined = body;
  inlined.Replace("/WmIm0", (ByteString("/") + imName).AsStringView());

  RetainPtr<CPDF_Dictionary> page_dict = pPage->GetMutableDict();
  RetainPtr<const CPDF_Object> contents_obj = page_dict->GetDirectObjectFor("Contents");
  RetainPtr<CPDF_Array> contents_array;
  if (contents_obj && contents_obj->IsArray()) {
    contents_array = page_dict->GetMutableArrayFor("Contents");
    if (!contents_array) {
      return false;
    }
  } else {
    contents_array = doc->NewIndirect<CPDF_Array>();
    if (contents_obj) {
      contents_array->AppendNew<CPDF_Reference>(doc, contents_obj->GetObjNum());
    }
    page_dict->SetNewFor<CPDF_Reference>("Contents", doc, contents_array->GetObjNum());
  }
  auto stream_dict = pdfium::MakeRetain<CPDF_Dictionary>();
  auto wm_stream = doc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(inlined.c_str(),
                          inlined.c_str() + inlined.GetLength()),
      stream_dict);
  contents_array->AppendNew<CPDF_Reference>(doc, wm_stream->GetObjNum());

  return true;
}
