// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fpdf_import_ext.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdf_edit.h"
#include "fpdf_import_common.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

constexpr double kA4W = 595.0;
constexpr double kA4H = 842.0;
constexpr double kMargin = 72.0;

class ByteWriter {
 public:
  void Float(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    text_ += buf;
  }
  void Int(long v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%ld", v);
    text_ += buf;
  }
  void Raw(const std::string& s) { text_ += s; }
  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

// A shown-text operator at (x, y) in page points, y-up.
std::string TextOperator(int font, double size, unsigned char r,
                         unsigned char g, unsigned char b, double x, double y,
                         const std::string& latin) {
  ByteWriter w;
  w.Raw("BT\n/F");
  w.Int(font);
  w.Raw(" ");
  w.Float(size);
  w.Raw(" Tf\n");
  w.Int(r);
  w.Raw(" ");
  w.Int(g);
  w.Raw(" ");
  w.Int(b);
  w.Raw(" rg\n1 0 0 1 ");
  w.Float(x);
  w.Raw(" ");
  w.Float(y);
  w.Raw(" Tm\n");
  w.Raw(pdfin::PdfString(latin));
  w.Raw(" Tj\nET\n");
  return w.text();
}

// Write |content| into |page|'s Contents, wiring up four standard-14 font
// resources (F1 normal .. F4 bold-italic), then close the page handle.
void FinalizePage(FPDF_DOCUMENT doc, FPDF_PAGE page,
                  const std::string& content) {
  if (!page)
    return;
  CPDF_Document* cdoc = CPDFDocumentFromFPDFDocument(doc);
  CPDF_Page* pp = CPDFPageFromFPDFPage(page);
  if (!cdoc || !pp) {
    FPDF_ClosePage(page);
    return;
  }
  	RetainPtr<CPDF_Dictionary> page_dict = pp->GetMutableDict();
  	RetainPtr<CPDF_Dictionary> resources =
      page_dict->GetMutableDictFor("Resources");
  	RetainPtr<CPDF_Dictionary> fonts = resources->GetMutableDictFor("Font");
  if (!fonts) {
    fonts = cdoc->New<CPDF_Dictionary>();
    resources->SetFor("Font", fonts);
  }
  static const char* kVariants[4] = {"Helvetica", "Helvetica-Bold",
                                     "Helvetica-Oblique", "Helvetica-BoldOblique"};
  for (int i = 0; i < 4; ++i) {
    	RetainPtr<CPDF_Dictionary> f = cdoc->New<CPDF_Dictionary>();
    f->SetNewFor<CPDF_Name>("Type", "Font");
    f->SetNewFor<CPDF_Name>("Subtype", "Type1");
    f->SetNewFor<CPDF_Name>("BaseFont", kVariants[i]);
    f->SetNewFor<CPDF_Name>("Encoding", "WinAnsiEncoding");
    char key[16];
    std::snprintf(key, sizeof(key), "F%d", i + 1);
    fonts->SetFor(key, f);
  }

  	RetainPtr<CPDF_Dictionary> stream_dict =
      pdfium::MakeRetain<CPDF_Dictionary>();
  	RetainPtr<CPDF_Stream> cs = cdoc->NewIndirect<CPDF_Stream>(
      DataVector<uint8_t>(content.data(), content.data() + content.size()),
      stream_dict);
  page_dict->SetNewFor<CPDF_Reference>("Contents", cdoc, cs->GetObjNum());
  FPDF_ClosePage(page);
}

// Flows styled, line-wrapped text onto pages of a fixed size, rolling to a new
// page when the cursor runs off the bottom.
class FlowPainter {
 public:
  FlowPainter(FPDF_DOCUMENT doc, double w, double h, double margin,
              double base_size)
      : doc_(doc), w_(w), h_(h), margin_(margin), base_size_(base_size) {}

  // Add a run (Latin-1) at the flow cursor, wrapping at the right margin.
  void AddStyled(const std::string& latin, bool bold, bool italic, double size,
                 unsigned char r, unsigned char g, unsigned char b) {
    EnsurePage();
    const int font = 1 + (bold ? 1 : 0) + (italic ? 2 : 0);
    std::string rest = latin;
    while (!rest.empty()) {
      const double x = margin_ + LineWidth();
      const double avail = w_ - margin_ - x;
      size_t per = (size > 0 && avail > 0)
                       ? static_cast<size_t>(avail / (size * 0.5))
                       : 1;
      if (per < 1)
        per = 1;
      if (per >= rest.size()) {
        line_.push_back({font, size, r, g, b, rest});
        rest.clear();
      } else {
        line_.push_back({font, size, r, g, b, rest.substr(0, per)});
        rest = rest.substr(per);
        FlushLine();
      }
    }
  }

  // Force a visual line break (write the pending line, move down one row).
  void NewLine() {
    EnsurePage();
    FlushLine();
  }

  // Flush the current line and close the page so the next content starts a
  // fresh page (used to separate slides).
  void BreakPage() {
    FlushLine();
    if (page_) {
      FinalizePage(doc_, page_, content_);
      page_ = nullptr;
      content_.clear();
    }
  }

  void Finalize() {
    EnsurePage();  // always emit at least one page
    FlushLine();
    BreakPage();
  }

 private:
  struct Seg {
    int font;
    double size;
    unsigned char r, g, b;
    std::string text;
  };
  std::vector<Seg> line_;

  double LineWidth() const {
    double w = 0;
    for (const Seg& s : line_)
      w += s.text.size() * (s.size * 0.5);
    return w;
  }

  void EnsurePage() {
    if (page_)
      return;
    page_ = FPDFPage_New(doc_, page_index_++, w_, h_);
    content_.clear();
    y_ = h_ - margin_;
  }

  void NextPage() {
    if (!page_)
      return;
    FinalizePage(doc_, page_, content_);
    page_ = FPDFPage_New(doc_, page_index_++, w_, h_);
    content_.clear();
    y_ = h_ - margin_;
  }

  void FlushLine() {
    if (line_.empty())
      return;
    if (y_ < margin_)
      NextPage();
    double x = margin_;
    for (const Seg& s : line_) {
      content_ += TextOperator(s.font, s.size, s.r, s.g, s.b, x, y_, s.text);
      x += s.text.size() * (s.size * 0.5);
    }
    line_.clear();
    y_ -= 1.2 * base_size_;
    if (y_ < margin_)
      NextPage();
  }

  FPDF_DOCUMENT doc_;
  double w_, h_, margin_, base_size_;
  FPDF_PAGE page_ = nullptr;
  std::string content_;
  int page_index_ = 0;
  double y_ = 0;
};

// Draw a single cell's text at a spreadsheet grid position.
void RenderSheetGrid(FPDF_DOCUMENT doc, int* page_index,
                     const std::vector<pdfin::XlsxCell>& cells, double page_w,
                     double page_h, double margin) {
  const double colw = std::max(50.0, std::min(160.0, (page_w - 2 * margin) / 4.0));
  const double rowh = 26.0;
  FPDF_PAGE page = FPDFPage_New(doc, (*page_index)++, page_w, page_h);
  std::string content;
  for (const pdfin::XlsxCell& c : cells) {
    const double x = margin + c.col * colw;
    const double y = page_h - margin - (c.row + 1) * rowh;
    if (x > page_w - margin || y < margin * 0.25)
      continue;
    content += TextOperator(1, 9.0, 0, 0, 0, x, y, pdfin::Latin1FromUtf8(c.text));
  }
  FinalizePage(doc, page, content);
}

// Parse a <p:sldSz cx="E" cy="E"/> out of presentation.xml; EMU -> points.
bool SlideSize(const std::vector<uint8_t>& pres, double* w, double* h) {
  std::string xml(pres.begin(), pres.end());
  size_t p = xml.find("sldSz");
  if (p == std::string::npos)
    return false;
  size_t cx = xml.find("cx=\"", p);
  size_t cy = xml.find("cy=\"", p);
  *w = 0;
  *h = 0;
  if (cx == std::string::npos)
    return false;
  size_t ce = xml.find('"', cx + 4);
  if (ce != std::string::npos)
    *w = std::atol(xml.substr(cx + 4, ce - cx - 4).c_str()) / 12700.0;
  size_t ye = cy == std::string::npos ? std::string::npos
                                      : xml.find('"', cy + 4);
  if (ye != std::string::npos)
    *h = std::atol(xml.substr(cy + 4, ye - cy - 4).c_str()) / 12700.0;
  return *w > 0 && *h > 0;
}

}  // namespace

extern "C" {

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFImport_FromWord(
    const unsigned char* data,
    size_t n,
    const FPDFExt_ImportOptions* options,
    FPDF_DOCUMENT* out_document) {
  if (!data || n == 0 || !out_document)
    return FALSE;
  *out_document = nullptr;
  const std::vector<uint8_t> container(data, data + n);
  std::vector<uint8_t> docxml = pdfin::ExtractZipPart(container, "word/document.xml");
  if (docxml.empty())
    return FALSE;
  const std::string xml(docxml.begin(), docxml.end());
  std::vector<std::vector<pdfin::DocxRun>> paras = pdfin::CollectDocxRuns(xml);

  const double page_w = options && options->page_width_pt > 0 ? options->page_width_pt : kA4W;
  const double page_h = options && options->page_height_pt > 0 ? options->page_height_pt : kA4H;
  const double size = options && options->font_size > 0 ? options->font_size : 11.0;

  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  if (!doc)
    return FALSE;

  FlowPainter p(doc, page_w, page_h, kMargin, size);
  for (const auto& para : paras) {
    for (const pdfin::DocxRun& run : para) {
      const double rsize = run.size_half / 2.0;
      p.AddStyled(pdfin::Latin1FromUtf8(run.text), run.bold, run.italic, rsize,
                  run.r, run.g, run.b);
    }
    p.NewLine();  // separate paragraphs
  }
  p.Finalize();
  *out_document = doc;
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFImport_FromExcel(
    const unsigned char* data,
    size_t n,
    const FPDFExt_ImportOptions* options,
    FPDF_DOCUMENT* out_document) {
  if (!data || n == 0 || !out_document)
    return FALSE;
  *out_document = nullptr;
  const std::vector<uint8_t> container(data, data + n);
  std::vector<uint8_t> ss = pdfin::ExtractZipPart(container, "xl/sharedStrings.xml");
  std::vector<std::string> shared =
      pdfin::CollectSharedStrings(std::string(ss.begin(), ss.end()));

  const double page_w = options && options->page_width_pt > 0 ? options->page_width_pt : kA4W;
  const double page_h = options && options->page_height_pt > 0 ? options->page_height_pt : kA4H;

  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  if (!doc)
    return FALSE;
  int page_index = 0;
  int sheet = 1;
  int rendered = 0;
  while (sheet <= 100) {
    const std::string name =
        "xl/worksheets/sheet" + std::to_string(sheet) + ".xml";
    std::vector<uint8_t> sx = pdfin::ExtractZipPart(container, name);
    if (sx.empty())
      break;
    const std::string sxml(sx.begin(), sx.end());
    std::vector<pdfin::XlsxCell> cells = pdfin::CollectSheetCells(sxml, shared);
    if (!cells.empty()) {
      RenderSheetGrid(doc, &page_index, cells, page_w, page_h, kMargin);
      rendered++;
    }
    ++sheet;
  }
  if (rendered == 0) {
    // No data: still hand back a one-page document so the caller can save.
    FPDF_PAGE blank = FPDFPage_New(doc, page_index++, page_w, page_h);
    FinalizePage(doc, blank, "");
  }
  *out_document = doc;
  return TRUE;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFImport_FromPptx(
    const unsigned char* data,
    size_t n,
    const FPDFExt_ImportOptions* options,
    FPDF_DOCUMENT* out_document) {
  if (!data || n == 0 || !out_document)
    return FALSE;
  *out_document = nullptr;
  const std::vector<uint8_t> container(data, data + n);

  double slide_w = kA4W, slide_h = kA4H;
  std::vector<uint8_t> pres = pdfin::ExtractZipPart(container, "ppt/presentation.xml");
  if (!pres.empty())
    SlideSize(pres, &slide_w, &slide_h);

  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  if (!doc)
    return FALSE;

  FlowPainter p(doc, slide_w, slide_h, kMargin, 11.0);
  int slide_index = 1;
  while (slide_index <= 100) {
    const std::string name = "ppt/slides/slide" + std::to_string(slide_index) + ".xml";
    std::vector<uint8_t> sx = pdfin::ExtractZipPart(container, name);
    if (sx.empty())
      break;
    const std::string sxml(sx.begin(), sx.end());
    const std::string text = pdfin::CollectSldText(sxml);
    std::string::size_type pos = 0;
    while (pos <= text.size()) {
      std::string::size_type nl = text.find('\n', pos);
      const std::string ln =
          nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
      if (!ln.empty())
        p.AddStyled(pdfin::Latin1FromUtf8(ln), false, false, 18.0, 0, 0, 0);
      if (nl == std::string::npos)
        break;
      pos = nl + 1;
      p.NewLine();
    }
    p.BreakPage();  // one slide per page
    ++slide_index;
  }
  p.Finalize();
  *out_document = doc;
  return TRUE;
}

}  // extern "C"