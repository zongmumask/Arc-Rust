// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fpdf_pagelabel_ext.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// Map style enum -> /S name; NoNumber (0) returns empty (no /S, prefix-only).
const char* StyleName(FPDFPAGELABEL_STYLE style) {
  switch (style) {
    case FPDFPAGELABEL_Decimal:
      return "D";
    case FPDFPAGELABEL_UpperRoman:
      return "R";
    case FPDFPAGELABEL_LowerRoman:
      return "r";
    case FPDFPAGELABEL_UpperAlpha:
      return "A";
    case FPDFPAGELABEL_LowerAlpha:
      return "a";
    default:
      return "";
  }
}

}  // namespace

extern "C" {

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentSetPageLabels(FPDF_DOCUMENT document,
                           const FPDFPAGE_LabelRange* ranges,
                           int count) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> root = pDoc->GetMutableRoot();
  if (!root) {
    return false;
  }

  if (count <= 0 || !ranges) {
    // Remove existing PageLabels so the document falls back to physical pages.
    root->RemoveFor("PageLabels");
    return true;
  }

  // Build /PageLabels as a flat /Nums number tree. Flat /Nums is valid per
  // ISO 32000-1 7.9.7 and simpler than /Kids subtrees for document-sized lists.
  RetainPtr<CPDF_Array> nums = pDoc->NewIndirect<CPDF_Array>();
  for (int i = 0; i < count; ++i) {
    const FPDFPAGE_LabelRange& range = ranges[i];

    // /Nums keys and values must be appended as inline objects or references.
    // Appending a fresh indirect object (objnum != 0) trips CPDF_Array's
    // CHECK(pObj->IsInline()) and aborts, so reference the indirect dict
    // instead of inserting it directly.
    nums->AppendNew<CPDF_Number>(range.first_index);

    RetainPtr<CPDF_Dictionary> label_dict = pDoc->New<CPDF_Dictionary>();
    if (range.prefix && range.prefix[0] != '\0') {
      WideString prefix = WideString::FromUTF8(range.prefix);
      label_dict->SetNewFor<CPDF_String>("P", prefix.AsStringView());
    }
    const char* style_name = StyleName(range.style);
    if (style_name[0] != '\0') {
      label_dict->SetNewFor<CPDF_Name>("S", style_name);
    }
    if (range.style != FPDFPAGELABEL_NoNumber) {
      // NoNumber has no numeric portion; /St only matters when /S is present.
      label_dict->SetNewFor<CPDF_Number>("St", range.start_number);
    }

    uint32_t dict_objnum = pDoc->AddIndirectObject(std::move(label_dict));
    nums->AppendNew<CPDF_Reference>(pDoc, dict_objnum);
  }

  root->SetNewFor<CPDF_Reference>("PageLabels", pDoc, nums->GetObjNum());
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentGetPageLabelRanges(FPDF_DOCUMENT document,
                                FPDFPAGE_LabelRange** out_ranges,
                                int* out_count) {
  *out_ranges = nullptr;
  *out_count = 0;

  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  const CPDF_Dictionary* root = pDoc->GetRoot();
  if (!root) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> labels_dict = root->GetDictFor("PageLabels");
  if (!labels_dict) {
    return false;
  }

  // Read the flat /Nums array as key/value pairs. A label's resolved value for
  // page `p` is prefix + style(p - first_index + start). Sorting keys keeps the
  // ranges in document order regardless of insertion order.
  struct KeyValue {
    int key;
    RetainPtr<const CPDF_Dictionary> dict;
  };
  std::vector<KeyValue> entries;
  if (const CPDF_Array* nums = labels_dict->GetArrayFor("Nums")) {
    for (size_t i = 0; i + 1 < nums->size(); i += 2) {
      entries.push_back({
          nums->GetIntegerAt(i),
          nums->GetDictAt(i + 1),
      });
    }
  }
  if (entries.empty()) {
    return false;
  }

  std::sort(entries.begin(), entries.end(),
            [](const KeyValue& a, const KeyValue& b) { return a.key < b.key; });

  size_t n = entries.size();
  FPDFPAGE_LabelRange* out = new FPDFPAGE_LabelRange[n]();
  for (size_t i = 0; i < n; ++i) {
    const CPDF_Dictionary* dict = entries[i].dict.Get();
    out[i].first_index = entries[i].key;
    out[i].start_number = dict ? dict->GetIntegerFor("St", 1) : 1;
    out[i].style = FPDFPAGELABEL_Decimal;

    ByteString style = dict ? dict->GetByteStringFor("S") : ByteString();
    if (!style.IsEmpty()) {
      if (style == "D") {
        out[i].style = FPDFPAGELABEL_Decimal;
      } else if (style == "R") {
        out[i].style = FPDFPAGELABEL_UpperRoman;
      } else if (style == "r") {
        out[i].style = FPDFPAGELABEL_LowerRoman;
      } else if (style == "A") {
        out[i].style = FPDFPAGELABEL_UpperAlpha;
      } else if (style == "a") {
        out[i].style = FPDFPAGELABEL_LowerAlpha;
      }
    } else {
      out[i].style = FPDFPAGELABEL_NoNumber;
    }

    if (dict) {
      WideString prefix = dict->GetUnicodeTextFor("P");
      ByteString utf8 = prefix.ToUTF8();
      size_t len = utf8.GetLength();
      char* buffer = new char[len + 1];
      std::memcpy(buffer, utf8.c_str(), len);
      buffer[len] = '\0';
      out[i].prefix = buffer;
    } else {
      out[i].prefix = nullptr;
    }
  }

  *out_ranges = out;
  *out_count = static_cast<int>(n);
  return true;
}

FPDF_EXPORT void FPDF_CALLCONV
FPDF_DocumentFreePageLabelRanges(FPDFPAGE_LabelRange* ranges, int count) {
  if (!ranges) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    delete[] ranges[i].prefix;
  }
  delete[] ranges;
}

}  // extern "C"