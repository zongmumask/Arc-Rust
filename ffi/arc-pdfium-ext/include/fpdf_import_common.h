// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Shared helpers for the Import (Office -> PDF) extensions. Reads an OOXML
// container (ZIP) and pulls text out of its XML parts; inflation uses PDFium's
// internal zlib (same FlateModule the watermark extension already links), so
// the kernel stays dependency-free elsewhere. Header-inline like
// fpdf_ooxml_common.h.

#ifndef FPDF_IMPORT_COMMON_H_
#define FPDF_IMPORT_COMMON_H_

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "core/fxcodec/data_and_bytes_consumed.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/span.h"

#ifndef FPDF_EXT_AA_TRUE_FALSE_
#define FPDF_EXT_AA_TRUE_FALSE_
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif

namespace pdfin {

inline uint16_t LE16(const std::vector<uint8_t>& b, size_t o) {
  return static_cast<uint16_t>(b[o]) |
         (static_cast<uint16_t>(b[o + 1]) << 8);
}

inline uint32_t LE32(const std::vector<uint8_t>& b, size_t o) {
  return static_cast<uint32_t>(b[o]) |
         (static_cast<uint32_t>(b[o + 1]) << 8) |
         (static_cast<uint32_t>(b[o + 2]) << 16) |
         (static_cast<uint32_t>(b[o + 3]) << 24);
}

// Extract |wanted| (exact archive member name, e.g. "word/document.xml") from
// a ZIP byte buffer, inflating deflate members. Empty on failure / absence.
inline std::vector<uint8_t> ExtractZipPart(const std::vector<uint8_t>& zip,
                                           const std::string& wanted) {
  if (zip.size() < 22)
    return {};
  // Locate the end-of-central-directory record (scan the tail for its sig).
  size_t eocd = static_cast<size_t>(-1);
  const size_t from = zip.size() > 65557 ? zip.size() - 65557 : 0;
  for (size_t i = zip.size() - 22;; --i) {
    if (zip[i] == 0x50 && zip[i + 1] == 0x4B && zip[i + 2] == 0x05 &&
        zip[i + 3] == 0x06) {
      eocd = i;
      break;
    }
    if (i == from)
      break;
  }
  if (eocd == static_cast<size_t>(-1))
    return {};

  const uint32_t nentry = LE16(zip, eocd + 10);
  const uint32_t cd_off = LE32(zip, eocd + 16);
  size_t p = cd_off;
  for (uint32_t i = 0; i < nentry && p + 46 <= zip.size(); ++i) {
    if (LE32(zip, p) != 0x02014B50)
      break;
    const uint16_t method = LE16(zip, p + 10);
    const uint32_t comp_size = LE32(zip, p + 20);
    const uint32_t uncomp_size = LE32(zip, p + 24);
    const uint16_t name_len = LE16(zip, p + 28);
    const size_t local_off = LE32(zip, p + 42);
    std::string name(reinterpret_cast<const char*>(zip.data() + p + 46),
                     name_len);
    if (name == wanted && local_off + 30 <= zip.size()) {
      const uint16_t ln = LE16(zip, local_off + 26);
      const uint16_t extra = LE16(zip, local_off + 28);
      const size_t dstart = local_off + 30 + ln + extra;
      if (dstart + comp_size > zip.size())
        return {};
      std::vector<uint8_t> raw(zip.begin() + static_cast<long>(dstart),
                               zip.begin() + static_cast<long>(dstart + comp_size));
      if (method == 0)
        return raw;  // stored
      if (method == 8) {  // deflated
        DataAndBytesConsumed r = FlateModule::FlateOrLZWDecode(
            /*bLZW=*/false,
            pdfium::span<const uint8_t>(raw.data(), raw.size()),
            /*bEarlyChange=*/true, /*predictor=*/0, /*Colors=*/0,
            /*BitsPerComponent=*/0, /*Columns=*/0, uncomp_size);
        const uint8_t* dp = r.data.data();
        const size_t dn = r.data.size();
        return dp ? std::vector<uint8_t>(dp, dp + dn) : std::vector<uint8_t>();
      }
      return {};
    }
    p += 46 + name_len + LE16(zip, p + 30) + LE16(zip, p + 32);
  }
  return {};
}

// Decode the common XML entities into characters.
inline std::string XmlUnescape(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      size_t sc = s.find(';', i);
      if (sc != std::string::npos) {
        std::string e = s.substr(i + 1, sc - i - 1);
        if (e == "amp") { r += '&'; i = sc + 1; continue; }
        if (e == "lt") { r += '<'; i = sc + 1; continue; }
        if (e == "gt") { r += '>'; i = sc + 1; continue; }
        if (e == "quot") { r += '"'; i = sc + 1; continue; }
        if (e == "apos") { r += '\''; i = sc + 1; continue; }
        if (!e.empty() && e[0] == '#') {
          unsigned int cp = 0;
          const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
          const size_t ds = hex ? 2 : 1;
          for (size_t k = ds; k < e.size() && e[k] != ';'; ++k) {
            cp = cp * (hex ? 16 : 10) + (hex ? "0123456789abcdef"[e[k] & 0x0F]
                                                   : e[k] - '0');
          }
          if (cp <= 0xFF)
            r += static_cast<char>(cp);
          i = sc + 1;
          continue;
        }
      }
    }
    r += s[i];
    ++i;
  }
  return r;
}

// Pull |word/document.xml|-style paragraphs from raw XML: each </w:p> flushes
// the accumulated text (runs concatenated); <w:br/> and <w:tab/> become
// newline and tab. Styling is ignored at this tier.
inline std::vector<std::string> CollectDocxParagraphs(const std::string& xml) {
  std::vector<std::string> paras;
  std::string cur;
  size_t i = 0;
  const size_t n = xml.size();
  while (i < n) {
    if (xml[i] != '<') {
      ++i;
      continue;
    }
    size_t close = xml.find('>', i);
    if (close == std::string::npos)
      break;
    std::string tag = xml.substr(i + 1, close - i - 1);
    // Trim leading/trailing whitespace, drop a trailing self-closing '/'.
    size_t b = tag.find_first_not_of(" \t\r\n");
    size_t e = tag.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) { i = close + 1; continue; }
    std::string t = tag.substr(b, e - b + 1);
    if (!t.empty() && t.back() == '/')
      t.pop_back();
    if (t.empty()) { i = close + 1; continue; }
    const bool closing = t.front() == '/';
    size_t sp = t.find_first_of(" \t\r\n");
    std::string base = closing ? t.substr(1) : (sp == std::string::npos ? t : t.substr(0, sp));
    if (base == "p" || base == "w:p") {
      if (closing) {
        if (!cur.empty())
          paras.push_back(cur);
        cur.clear();
      }
    } else if (!closing && (base == "t" || base == "w:t")) {
      size_t et = xml.find("</w:t>", close);
      if (et != std::string::npos) {
        cur += XmlUnescape(xml.substr(close + 1, et - close - 1));
        i = et + 6;
        continue;
      }
    } else if (!closing && (base == "br" || base == "w:br")) {
      cur += '\n';
    } else if (!closing && (base == "tab" || base == "w:tab")) {
      cur += '\t';
    }
    i = close + 1;
  }
  if (!cur.empty())
    paras.push_back(cur);
  return paras;
}

// ---------------------------------------------------------------------------
// XML tag helpers.
// ---------------------------------------------------------------------------
// Split a raw element tag into its (namespace-stripped) base name and whether
// it is a closing tag. Handles self-closing and attributes.
inline void SplitTag(const std::string& tag, std::string* base, bool* closing) {
  size_t b = tag.find_first_not_of(" \t\r\n");
  size_t e = tag.find_last_not_of(" \t\r\n");
  *closing = false;
  *base = "";
  if (b == std::string::npos)
    return;
  std::string t = tag.substr(b, e - b + 1);
  if (!t.empty() && t.back() == '/')
    t.pop_back();
  if (t.empty())
    return;
  if (t.front() == '/') {
    *closing = true;
    t = t.substr(1);
  }
  size_t sp = t.find_first_of(" \t\r\n");
  std::string nm = sp == std::string::npos ? t : t.substr(0, sp);
  size_t colon = nm.rfind(':');
  *base = colon == std::string::npos ? nm : nm.substr(colon + 1);
}

// Value of an attribute like name="value" in a raw tag string ("" if absent).
inline std::string GetAttr(const std::string& tag, const std::string& name) {
  const std::string key = name + "=\"";
  size_t p = tag.find(key);
  if (p == std::string::npos)
    return "";
  size_t q = tag.find('"', p + key.size());
  return q == std::string::npos ? "" : tag.substr(p + key.size(), q - (p + key.size()));
}

// OOXML boolean attribute: present with a truthy value => bold/italic on.
inline bool BoolAttr(const std::string& tag) {
  std::string v = GetAttr(tag, "w:val");
  if (v.empty())
    v = GetAttr(tag, "val");
  return !(v == "0" || v == "false" || v == "off");
}

// Transcode UTF-8 to Latin-1 (C2/C3 -> U+0080..U+00FF; others dropped).
inline std::string Latin1FromUtf8(const std::string& utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
    } else if (c == 0xC2 && i + 1 < utf8.size()) {
      out += static_cast<char>(0x80 | (utf8[i + 1] & 0x3F));
      i += 2;
    } else if (c == 0xC3 && i + 1 < utf8.size()) {
      out += static_cast<char>(0xC0 | (utf8[i + 1] & 0x3F));
      i += 2;
    } else {
      ++i;
    }
  }
  return out;
}

// A parenthesised PDF literal string with octal escapes for non-printables.
inline std::string PdfString(const std::string& latin) {
  std::string s = "(";
  for (unsigned char c : latin) {
    if (c == 0x28 || c == 0x29 || c == 0x5C) {
      s += '\\';
      s += static_cast<char>(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\%03o", c);
      s += buf;
    }
  }
  s += ')';
  return s;
}

// ---------------------------------------------------------------------------
// Word: paragraphs of styled runs (bold/italic/half-point size/colour).
// ---------------------------------------------------------------------------
struct DocxRun {
  std::string text;
  bool bold = false;
  bool italic = false;
  int size_half = 22;  // half-points; 22 = 11 pt
  unsigned char r = 0, g = 0, b = 0;
};

inline std::vector<std::vector<DocxRun>> CollectDocxRuns(const std::string& xml) {
  std::vector<std::vector<DocxRun>> paras;
  std::vector<DocxRun> cur;
  DocxRun pending;
  bool pending_has = false;
  bool in_rpr = false;
  size_t i = 0;
  const size_t n = xml.size();
  const auto flush_pending = [&]() {
    if (pending_has) {
      cur.push_back(pending);
      pending = DocxRun();
      pending_has = false;
    }
  };
  while (i < n) {
    if (xml[i] != '<') {
      ++i;
      continue;
    }
    size_t close = xml.find('>', i);
    if (close == std::string::npos)
      break;
    std::string tag = xml.substr(i + 1, close - i - 1);
    std::string base;
    bool closing;
    SplitTag(tag, &base, &closing);
    if (base.empty()) {
      i = close + 1;
      continue;
    }
    if (base == "p") {
      if (closing) {
        flush_pending();
        paras.push_back(cur);
        cur.clear();
      }
    } else if (base == "r") {
      if (!closing) {
        flush_pending();
        pending = DocxRun();  // reset style for the new run
        pending_has = false;
      }
    } else if (base == "rPr") {
      in_rpr = !closing;
    } else if (in_rpr && !closing) {
      if (base == "b") {
        pending.bold = BoolAttr(tag);
      } else if (base == "i") {
        pending.italic = BoolAttr(tag);
      } else if (base == "sz") {
        std::string v = GetAttr(tag, "w:val");
        int s = v.empty() ? 22 : std::atoi(v.c_str());
        pending.size_half = s > 0 ? s : 22;
      } else if (base == "color") {
        std::string v = GetAttr(tag, "w:val");
        if (v.size() >= 6) {
          auto hex = [](char c) -> unsigned char {
            return static_cast<unsigned char>(
                c <= '9' ? c - '0' : (c <= 'F' ? c - 'A' + 10 : c - 'a' + 10));
          };
          pending.r = hex(v[0]) * 16 + hex(v[1]);
          pending.g = hex(v[2]) * 16 + hex(v[3]);
          pending.b = hex(v[4]) * 16 + hex(v[5]);
        }
      }
    }
    if (base == "t") {
      if (!closing) {
        size_t et = xml.find("</w:t>", close);
        if (et != std::string::npos) {
          pending.text += XmlUnescape(xml.substr(close + 1, et - close - 1));
          pending_has = !pending.text.empty();
          i = et + 6;
          continue;
        }
      }
    } else if (base == "br") {
      pending.text += '\n';
      pending_has = true;
    } else if (base == "tab") {
      pending.text += '\t';
      pending_has = true;
    }
    i = close + 1;
  }
  flush_pending();
  if (!cur.empty())
    paras.push_back(cur);
  return paras;
}

// ---------------------------------------------------------------------------
// Excel: worksheet cells (0-based row/col) + shared strings.
// ---------------------------------------------------------------------------
struct XlsxCell {
  int row;
  int col;
  std::string text;
};

// Parse a cell reference "BC42" into 0-based column/row.
inline bool ParseCellRef(const std::string& r, int* col0, int* row0) {
  size_t k = 0;
  int col = 0;
  while (k < r.size() &&
         ((r[k] >= 'A' && r[k] <= 'Z') || (r[k] >= 'a' && r[k] <= 'z'))) {
    char c = r[k] >= 'a' ? static_cast<char>(r[k] - 'a' + 'A') : r[k];
    col = col * 26 + (c - 'A' + 1);
    ++k;
  }
  int row = 0;
  while (k < r.size() && r[k] >= '0' && r[k] <= '9') {
    row = row * 10 + (r[k] - '0');
    ++k;
  }
  if (col == 0 || row == 0)
    return false;
  *col0 = col - 1;
  *row0 = row - 1;
  return true;
}

// xl/sharedStrings.xml -> the string table.
inline std::vector<std::string> CollectSharedStrings(const std::string& xml) {
  std::vector<std::string> out;
  size_t i = 0;
  while ((i = xml.find("<si", i)) != std::string::npos) {
    size_t ce = xml.find('>', i);
    size_t cl = xml.find("</si>", ce);
    if (ce == std::string::npos || cl == std::string::npos)
      break;
    std::string block = xml.substr(ce + 1, cl - ce - 1);
    std::string text;
    size_t j = 0;
    while ((j = block.find("<t", j)) != std::string::npos) {
      size_t ce2 = block.find('>', j);
      size_t ct = block.find("</t>", ce2);
      if (ce2 == std::string::npos || ct == std::string::npos)
        break;
      text += XmlUnescape(block.substr(ce2 + 1, ct - ce2 - 1));
      j = ct + 4;
    }
    out.push_back(text);
    i = cl + 5;
  }
  return out;
}

// A worksheet -> non-empty cells. inlineStr and shared-string ("s") cells are
// resolved; other typed cells take their raw <v> text.
inline std::vector<XlsxCell> CollectSheetCells(
    const std::string& xml, const std::vector<std::string>& shared) {
  std::vector<XlsxCell> out;
  size_t p = 0;
  const size_t n = xml.size();
  while (p < n) {
    size_t s1 = xml.find("<c ", p);
    size_t s2 = xml.find("<c>", p);
    if (s1 == std::string::npos && s2 == std::string::npos)
      break;
    size_t s = (s2 == std::string::npos) ? s1
                : (s1 == std::string::npos) ? s2 : std::min(s1, s2);
    size_t ce = xml.find('>', s);
    if (ce == std::string::npos)
      break;
    size_t cl = xml.find("</c>", ce);
    if (cl == std::string::npos)
      break;
    std::string open = xml.substr(s + 1, ce - s - 1);
    int col = 0, row = 0;
    std::string text;
    if (ParseCellRef(GetAttr(open, "r"), &col, &row)) {
      const std::string block = xml.substr(ce + 1, cl - ce - 1);
      const std::string typ = GetAttr(open, "t");
      size_t v0;
      size_t v1;
      size_t vc;
      if (typ == "inlineStr") {
        size_t t = block.find("<t");
        size_t te = t == std::string::npos ? std::string::npos : block.find('>', t);
        size_t ct = te == std::string::npos ? std::string::npos : block.find("</t>", te);
        if (t != std::string::npos && ct != std::string::npos)
          text = XmlUnescape(block.substr(te + 1, ct - te - 1));
      } else if (typ == "s") {
        v0 = block.find("<v");
        v1 = v0 == std::string::npos ? std::string::npos : block.find('>', v0);
        vc = v1 == std::string::npos ? std::string::npos : block.find("</v>", v1);
        if (v0 != std::string::npos && vc != std::string::npos) {
          int idx = std::atoi(block.substr(v1 + 1, vc - v1 - 1).c_str());
          if (idx >= 0 && idx < static_cast<int>(shared.size()))
            text = shared[static_cast<size_t>(idx)];
        }
      } else {
        v0 = block.find("<v");
        v1 = v0 == std::string::npos ? std::string::npos : block.find('>', v0);
        vc = v1 == std::string::npos ? std::string::npos : block.find("</v>", v1);
        if (v0 != std::string::npos && vc != std::string::npos)
          text = block.substr(v1 + 1, vc - v1 - 1);
      }
      if (!text.empty())
        out.push_back({row, col, text});
    }
    p = cl + 4;
  }
  return out;
}

inline std::string CollectSldText(const std::string& xml) {
  std::string out;
  size_t i = 0;
  const size_t n = xml.size();
  while (i < n) {
    if (xml[i] != '<') {
      ++i;
      continue;
    }
    size_t close = xml.find('>', i);
    if (close == std::string::npos)
      break;
    std::string base;
    bool closing;
    SplitTag(xml.substr(i + 1, close - i - 1), &base, &closing);
    if (base == "t" && !closing) {
      size_t et = xml.find("</a:t>", close);
      if (et == std::string::npos) {
        i = close + 1;
        continue;
      }
      out += XmlUnescape(xml.substr(close + 1, et - close - 1));
      i = et + 6;
      continue;
    }
    if ((base == "p" && closing) || base == "br")
      out += '\n';
    i = close + 1;
  }
  return out;
}

}  // namespace pdfin

#endif  // FPDF_IMPORT_COMMON_H_