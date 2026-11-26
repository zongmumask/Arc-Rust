// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fpdf_ocg_ext.h"

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

// Type conversion: CPDF_Dictionary* <-> FPDF_OCG
inline FPDF_OCG FPDFOCGFromCPDFDictionary(const CPDF_Dictionary* dict) {
    return reinterpret_cast<FPDF_OCG>(const_cast<CPDF_Dictionary*>(dict));
}

inline CPDF_Dictionary* CPDFDictionaryFromFPDFOCG(FPDF_OCG ocg) {
    return reinterpret_cast<CPDF_Dictionary*>(ocg);
}

// Returns true if |ocg_dict| is an indirect object owned by |doc| (rejects
// cross-document / stale-but-alive handles). Inline objects (ObjNum 0) are
// accepted. A freed handle cannot be safely validated; this guards the
// cross-document case where the dict is alive in another document.
bool OCGBelongsToDocument(CPDF_Dictionary* ocg_dict, CPDF_Document* doc) {
    if (!ocg_dict || !doc) return false;
    const uint32_t obj_num = ocg_dict->GetObjNum();
    if (obj_num == 0) return true;  // inline object embedded in a doc-owned object
    // 不能按指针比较归属：`ParseIndirectObject` 可能重建出同一逻辑对象的另一实例，
    // 导致 objnum 相同而指针不同，把本属当前 doc 的 OCG 误判为跨文档（使 SetVisible
    // 提前 return、ON/OFF 数组永不落地 →“点眼睛无效”）。用对象号判断：真正跨文档时
    // 该对象号在当前 doc 解析不到（返回空）；属于本 doc 则解析出 objnum 一致的实例。
    RetainPtr<CPDF_Object> parsed = doc->ParseIndirectObject(obj_num);
    return parsed && parsed->GetObjNum() == obj_num;
}

// Get or create the OCProperties dictionary in the document root.
RetainPtr<CPDF_Dictionary> GetOrCreateOCProperties(CPDF_Document* doc) {
    if (!doc) return nullptr;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return nullptr;

    RetainPtr<CPDF_Dictionary> oc_props = root->GetMutableDictFor("OCProperties");
    if (!oc_props) {
        oc_props = doc->NewIndirect<CPDF_Dictionary>();
        if (!oc_props) return nullptr;  // Failed to create
        root->SetNewFor<CPDF_Reference>("OCProperties", doc, oc_props->GetObjNum());
    }
    return oc_props;
}

// Get or create the default configuration dictionary (D) in OCProperties.
RetainPtr<CPDF_Dictionary> GetOrCreateConfig(CPDF_Document* doc,
                                             CPDF_Dictionary* oc_props) {
    if (!doc || !oc_props) return nullptr;
    
    RetainPtr<CPDF_Dictionary> config = oc_props->GetMutableDictFor("D");
    if (!config) {
        config = doc->NewIndirect<CPDF_Dictionary>();
        if (!config) return nullptr;  // Failed to create
        config->SetNewFor<CPDF_Name>("BaseState", "ON");
        oc_props->SetNewFor<CPDF_Reference>("D", doc, config->GetObjNum());
    }
    return config;
}

// Get or create the OCGs array in OCProperties.
RetainPtr<CPDF_Array> GetOrCreateOCGsArray(CPDF_Document* doc,
                                            CPDF_Dictionary* oc_props) {
    if (!doc || !oc_props) return nullptr;
    
    RetainPtr<CPDF_Array> ocgs = oc_props->GetMutableArrayFor("OCGs");
    if (!ocgs) {
        ocgs = doc->NewIndirect<CPDF_Array>();
        if (!ocgs) return nullptr;  // Failed to create
        oc_props->SetNewFor<CPDF_Reference>("OCGs", doc, ocgs->GetObjNum());
    }
    return ocgs;
}

// Get or create the Order array in the config dictionary.
RetainPtr<CPDF_Array> GetOrCreateOrderArray(CPDF_Document* doc,
                                             CPDF_Dictionary* config) {
    if (!doc || !config) return nullptr;
    
    RetainPtr<CPDF_Array> order = config->GetMutableArrayFor("Order");
    if (!order) {
        order = doc->NewIndirect<CPDF_Array>();
        if (!order) return nullptr;  // Failed to create
        config->SetNewFor<CPDF_Reference>("Order", doc, order->GetObjNum());
    }
    return order;
}

// Set a reference from owner[key] to target.
void SetReferenceFor(CPDF_Dictionary* owner,
                     const char* key,
                     CPDF_Document* doc,
                     CPDF_Dictionary* target) {
    if (!owner || !doc || !target) return;
    const uint32_t obj_num = target->GetObjNum();
    if (obj_num != 0) {
        owner->SetNewFor<CPDF_Reference>(key, doc, obj_num);
    } else {
        owner->SetFor(key, pdfium::WrapRetain(target));
    }
}

// Append a reference to the target dictionary in the given array.
void AppendReferenceTo(CPDF_Array* array,
                       CPDF_Document* doc,
                       CPDF_Dictionary* target) {
    if (!array || !doc || !target) return;
    const uint32_t obj_num = target->GetObjNum();
    if (obj_num != 0) {
        array->AppendNew<CPDF_Reference>(doc, obj_num);
    } else {
        // For inline dictionaries, add a direct reference
        array->Append(pdfium::WrapRetain(target));
    }
}

// Check if an array contains the given dictionary (by reference or direct object).
bool ArrayContainsDict(const CPDF_Array* array, const CPDF_Dictionary* dict) {
    if (!array || !dict) return false;
    for (size_t i = 0; i < array->size(); i++) {
        RetainPtr<const CPDF_Dictionary> item = array->GetDictAt(i);
        if (!item) continue;  // Skip null entries
        if (item.Get() == dict) return true;
    }
    return false;
}

// Remove a dictionary reference from an array.
// Returns true if the item was found and removed.
bool RemoveDictFromArray(CPDF_Array* array, const CPDF_Dictionary* dict) {
    if (!array || !dict) return false;
    for (size_t i = 0; i < array->size(); i++) {
        RetainPtr<const CPDF_Dictionary> item = array->GetDictAt(i);
        if (!item) continue;  // Skip null entries
        if (item.Get() == dict) {
            array->RemoveAt(i);
            return true;
        }
    }
    return false;
}

}  // namespace

// ============================================================================
// OCG CRUD Implementation
// ============================================================================

FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_Create(FPDF_DOCUMENT document, FPDF_WIDESTRING name) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc || !name) return nullptr;

    WideString ws_name = WideStringFromFPDFWideString(name);
    if (ws_name.IsEmpty()) return nullptr;  // Reject empty names

    // Create OCG dictionary: /Type /OCG /Name (name)
    auto ocg_dict = doc->NewIndirect<CPDF_Dictionary>();
    if (!ocg_dict) return nullptr;  // Failed to create
    
    ocg_dict->SetNewFor<CPDF_Name>("Type", "OCG");
    // Use WideStringView(ws_name), NOT ws_name.AsStringView(): AsStringView() returns a
    // UTF-32 view on macOS and silently drops non-ASCII characters from the layer name.
    // WideStringView preserves the full Unicode string. (FPDFOCG_SetName uses the same.)
    ocg_dict->SetNewFor<CPDF_String>("Name", WideStringView(ws_name));

    // Get or create OCProperties
    RetainPtr<CPDF_Dictionary> oc_props = GetOrCreateOCProperties(doc);
    if (!oc_props) return nullptr;

    // Add to OCGs array
    RetainPtr<CPDF_Array> ocgs = GetOrCreateOCGsArray(doc, oc_props.Get());
    if (!ocgs) return nullptr;
    AppendReferenceTo(ocgs.Get(), doc, ocg_dict.Get());

    // Get or create config (D)
    RetainPtr<CPDF_Dictionary> config = GetOrCreateConfig(doc, oc_props.Get());
    if (!config) return nullptr;

    // Add to Order array
    RetainPtr<CPDF_Array> order = GetOrCreateOrderArray(doc, config.Get());
    if (!order) return nullptr;
    AppendReferenceTo(order.Get(), doc, ocg_dict.Get());

    return FPDFOCGFromCPDFDictionary(ocg_dict.Get());
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFOCG_GetCount(FPDF_DOCUMENT document) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) return 0;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return 0;

    RetainPtr<CPDF_Dictionary> oc_props = root->GetMutableDictFor("OCProperties");
    if (!oc_props) return 0;

    RetainPtr<CPDF_Array> ocgs = oc_props->GetMutableArrayFor("OCGs");
    if (!ocgs) return 0;

    return static_cast<int>(ocgs->size());
}

FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_Get(FPDF_DOCUMENT document, int index) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc || index < 0) return nullptr;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return nullptr;

    RetainPtr<CPDF_Dictionary> oc_props = root->GetMutableDictFor("OCProperties");
    if (!oc_props) return nullptr;

    RetainPtr<CPDF_Array> ocgs = oc_props->GetMutableArrayFor("OCGs");
    if (!ocgs || index >= static_cast<int>(ocgs->size())) return nullptr;

    // First try Order array for ordered access
    RetainPtr<CPDF_Dictionary> config = oc_props->GetMutableDictFor("D");
    if (config) {
        RetainPtr<CPDF_Array> order = config->GetMutableArrayFor("Order");
        if (order && index < static_cast<int>(order->size())) {
            RetainPtr<const CPDF_Dictionary> ocg = order->GetDictAt(index);
            if (ocg) {
                // Validate that this OCG exists in the OCGs array
                bool found_in_ocgs = false;
                for (size_t i = 0; i < ocgs->size(); i++) {
                    RetainPtr<const CPDF_Dictionary> ocg_item = ocgs->GetDictAt(i);
                    if (ocg_item && ocg_item.Get() == ocg.Get()) {
                        found_in_ocgs = true;
                        break;
                    }
                }
                if (found_in_ocgs) {
                    return FPDFOCGFromCPDFDictionary(ocg.Get());
                }
            }
        }
    }

    // Fallback: use OCGs array directly
    RetainPtr<const CPDF_Dictionary> ocg = ocgs->GetDictAt(index);
    return ocg ? FPDFOCGFromCPDFDictionary(ocg.Get()) : nullptr;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_Delete(FPDF_DOCUMENT document, FPDF_OCG ocg) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    CPDF_Dictionary* ocg_dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!doc || !ocg_dict) return false;

    RetainPtr<CPDF_Dictionary> oc_props = GetOrCreateOCProperties(doc);
    if (!oc_props) return false;

    // Remove from OCGs array
    RetainPtr<CPDF_Array> ocgs = oc_props->GetMutableArrayFor("OCGs");
    if (ocgs) {
        RemoveDictFromArray(ocgs.Get(), ocg_dict);
    }

    // Remove from Order array
    RetainPtr<CPDF_Dictionary> config = oc_props->GetMutableDictFor("D");
    if (config) {
        RetainPtr<CPDF_Array> order = config->GetMutableArrayFor("Order");
        if (order) {
            RemoveDictFromArray(order.Get(), ocg_dict);
        }

        // Remove from ON array
        RetainPtr<CPDF_Array> on = config->GetMutableArrayFor("ON");
        if (on) RemoveDictFromArray(on.Get(), ocg_dict);

        // Remove from OFF array
        RetainPtr<CPDF_Array> off = config->GetMutableArrayFor("OFF");
        if (off) RemoveDictFromArray(off.Get(), ocg_dict);
    }

    return true;
}

// ============================================================================
// OCG Properties Implementation
// ============================================================================

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFOCG_GetName(FPDF_OCG ocg, FPDF_WCHAR* buffer, unsigned long buflen) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!dict) return 0;

    WideString ws_name = dict->GetUnicodeTextFor("Name");
    if (ws_name.IsEmpty()) return 0;

    // Encode as UTF-16LE (matching PDFium's official API pattern)
    ByteString encoded = ws_name.ToUTF16LE();
    unsigned long encoded_len = static_cast<unsigned long>(encoded.GetLength());
    unsigned long required = encoded_len + sizeof(FPDF_WCHAR);

    // Safety check for overflow
    if (required < encoded_len) return 0;  // Overflow detected

    if (!buffer || buflen < required) {
        return required;
    }

    // Additional safety: verify encoded string is valid
    const char* encoded_str = encoded.c_str();
    if (!encoded_str) return 0;

    memcpy(buffer, encoded_str, encoded_len);
    // Null terminator - ensure we don't write past buffer
    if (buflen >= required) {
        memset(reinterpret_cast<char*>(buffer) + encoded_len, 0, sizeof(FPDF_WCHAR));
    }
    return required;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetName(FPDF_OCG ocg, FPDF_WIDESTRING name) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!dict || !name) return false;

    WideString ws_name = WideStringFromFPDFWideString(name);
    // See FPDFOCG_Create: WideStringView preserves Unicode; AsStringView() drops non-ASCII.
    dict->SetNewFor<CPDF_String>("Name", WideStringView(ws_name));
    return true;
}

// ============================================================================
// OCG Visibility Implementation
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_IsVisible(FPDF_DOCUMENT document, FPDF_OCG ocg) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    CPDF_Dictionary* ocg_dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!doc || !ocg_dict) return false;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return true;  // No OCProperties → default visible

    RetainPtr<CPDF_Dictionary> oc_props = root->GetMutableDictFor("OCProperties");
    if (!oc_props) return true;

    RetainPtr<CPDF_Dictionary> config = oc_props->GetMutableDictFor("D");
    if (!config) return true;

    // Check BaseState (default ON)
    bool visible = config->GetByteStringFor("BaseState", "ON") != "OFF";

    // Check ON array (overrides BaseState)
    RetainPtr<CPDF_Array> on = config->GetMutableArrayFor("ON");
    if (on && ArrayContainsDict(on.Get(), ocg_dict)) {
        visible = true;
    }

    // Check OFF array (overrides BaseState)
    RetainPtr<CPDF_Array> off = config->GetMutableArrayFor("OFF");
    if (off && ArrayContainsDict(off.Get(), ocg_dict)) {
        visible = false;
    }

    return visible ? 1 : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetVisible(FPDF_DOCUMENT document, FPDF_OCG ocg, FPDF_BOOL visible) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    CPDF_Dictionary* ocg_dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!doc || !ocg_dict) return false;
    // Reject cross-document / stale-but-alive OCG handles.
    if (!OCGBelongsToDocument(ocg_dict, doc)) return false;
    // Reject dicts that are explicitly not OCGs.
    ByteString ocg_type = ocg_dict->GetByteStringFor("Type", "");
    if (!ocg_type.IsEmpty() && ocg_type != "OCG") return false;

    RetainPtr<CPDF_Dictionary> oc_props = GetOrCreateOCProperties(doc);
    if (!oc_props) return false;

    RetainPtr<CPDF_Dictionary> config = GetOrCreateConfig(doc, oc_props.Get());
    if (!config) return false;

    // Get or create ON/OFF arrays
    RetainPtr<CPDF_Array> on = config->GetMutableArrayFor("ON");
    if (!on) {
        on = doc->NewIndirect<CPDF_Array>();
        if (!on) return false;  // Failed to create
        config->SetNewFor<CPDF_Reference>("ON", doc, on->GetObjNum());
    }

    RetainPtr<CPDF_Array> off = config->GetMutableArrayFor("OFF");
    if (!off) {
        off = doc->NewIndirect<CPDF_Array>();
        if (!off) return false;  // Failed to create
        config->SetNewFor<CPDF_Reference>("OFF", doc, off->GetObjNum());
    }

    if (visible) {
        // Add to ON, remove from OFF
        if (!ArrayContainsDict(on.Get(), ocg_dict)) {
            AppendReferenceTo(on.Get(), doc, ocg_dict);
        }
        RemoveDictFromArray(off.Get(), ocg_dict);
    } else {
        // Add to OFF, remove from ON
        if (!ArrayContainsDict(off.Get(), ocg_dict)) {
            AppendReferenceTo(off.Get(), doc, ocg_dict);
        }
        RemoveDictFromArray(on.Get(), ocg_dict);
    }

    return true;
}

// ============================================================================
// Annotation–OCG Association Implementation
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetAnnotOC(FPDF_ANNOTATION annot, FPDF_OCG ocg) {
    CPDF_AnnotContext* annot_ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
    if (!annot_ctx) return false;

    CPDF_Dictionary* annot_dict = annot_ctx->GetMutableAnnotDict();
    if (!annot_dict) return false;

    if (!ocg) {
        // Remove /OC entry
        annot_dict->RemoveFor("OC");
        return true;
    }

    CPDF_Dictionary* ocg_dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!ocg_dict) return false;

    // Get the document from the annotation's page
    IPDF_Page* page = annot_ctx->GetPage();
    if (!page) return false;

    CPDF_Document* doc = page->GetDocument();
    if (!doc) return false;

    // Validate that this is actually an OCG dictionary
    ByteString type = ocg_dict->GetByteStringFor("Type", "");
    if (!type.IsEmpty() && type != "OCG") return false;

    // Set /OC as a reference to the OCG dictionary
    const uint32_t obj_num = ocg_dict->GetObjNum();
    if (obj_num != 0) {
        annot_dict->SetNewFor<CPDF_Reference>("OC", doc, obj_num);
    } else {
        // For inline dictionaries, we need to be careful with lifetime
        // Wrap in RetainPtr to ensure proper reference counting
        RetainPtr<CPDF_Dictionary> retained_ocg = pdfium::WrapRetain(ocg_dict);
        if (!retained_ocg) return false;
        annot_dict->SetFor("OC", retained_ocg);
    }

    return true;
}

FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_GetAnnotOC(FPDF_ANNOTATION annot) {
    CPDF_AnnotContext* annot_ctx = CPDFAnnotContextFromFPDFAnnotation(annot);
    if (!annot_ctx) return nullptr;

    const CPDF_Dictionary* annot_dict = annot_ctx->GetAnnotDict();
    if (!annot_dict) return nullptr;

    RetainPtr<const CPDF_Dictionary> oc_dict = annot_dict->GetDictFor("OC");
    if (!oc_dict) return nullptr;

    // Verify it's actually an OCG - require explicit Type field
    ByteString type = oc_dict->GetByteStringFor("Type", "");
    if (type.IsEmpty() || type != "OCG") return nullptr;

    return FPDFOCGFromCPDFDictionary(oc_dict.Get());
}

// ============================================================================
// OCG Custom Properties Implementation (Quill-specific)
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_GetLocked(FPDF_OCG ocg) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!dict) return false;
    return dict->GetBooleanFor("QuillLocked", false) ? 1 : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetLocked(FPDF_OCG ocg, FPDF_BOOL locked) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!dict) return false;
    if (locked) {
        dict->SetNewFor<CPDF_Boolean>("QuillLocked", true);
    } else {
        dict->RemoveFor("QuillLocked");
    }
    return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFOCG_GetColor(FPDF_OCG ocg, char* buffer, unsigned long buflen) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!dict) return 0;

    ByteString color = dict->GetByteStringFor("QuillColor");
    if (color.IsEmpty()) return 0;

    unsigned long color_len = static_cast<unsigned long>(color.GetLength());
    unsigned long required = color_len + 1;

    // Safety check for overflow
    if (required < color_len) return 0;  // Overflow detected

    if (!buffer || buflen < required) {
        return required;
    }

    // Verify color string is valid
    const char* color_str = color.c_str();
    if (!color_str) return 0;

    memcpy(buffer, color_str, required);
    return required;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetColor(FPDF_OCG ocg, const char* color) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!dict) return false;

    if (!color) {
        dict->RemoveFor("QuillColor");
        return true;
    }

    dict->SetNewFor<CPDF_String>("QuillColor", color);
    return true;
}

// ============================================================================
// Active Layer Implementation (Quill-specific)
// ============================================================================
//
// The active layer is persisted as /Root /QuillActiveLayer <ocg ref>. NULL/missing
// means "General" (the virtual default; new annotations get no /OC).

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetActiveLayer(FPDF_DOCUMENT document, FPDF_OCG ocg) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) return false;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return false;

    if (!ocg) {
        // General: clear any persisted active-layer reference.
        root->RemoveFor("QuillActiveLayer");
        return true;
    }

    CPDF_Dictionary* ocg_dict = CPDFDictionaryFromFPDFOCG(ocg);
    if (!ocg_dict) return false;
    // Reject cross-document / stale-but-alive OCG handles.
    if (!OCGBelongsToDocument(ocg_dict, doc)) return false;

    // Validate it is actually an OCG dictionary (reject arbitrary dicts).
    ByteString type = ocg_dict->GetByteStringFor("Type", "");
    if (!type.IsEmpty() && type != "OCG") return false;

    SetReferenceFor(root.Get(), "QuillActiveLayer", doc, ocg_dict);
    return true;
}

FPDF_EXPORT FPDF_OCG FPDF_CALLCONV
FPDFOCG_GetActiveLayer(FPDF_DOCUMENT document) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) return nullptr;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return nullptr;

    RetainPtr<CPDF_Dictionary> ocg = root->GetMutableDictFor("QuillActiveLayer");
    if (!ocg) return nullptr;  // No stored active layer -> General

    // Validate Type=OCG.
    ByteString type = ocg->GetByteStringFor("Type", "");
    if (type.IsEmpty() || type != "OCG") return nullptr;

    // Guard against dangling references to deleted OCGs: require the referenced
    // dictionary to still be present in /OCProperties/OCGs. A deleted OCG's dict
    // object may linger in the file even after FPDFOCG_Delete removed it from the
    // OCGs/Order/ON/OFF arrays; without this check, loadLayers would restore an
    // active layer that no longer exists.
    RetainPtr<CPDF_Dictionary> oc_props = root->GetMutableDictFor("OCProperties");
    if (!oc_props) return nullptr;
    RetainPtr<CPDF_Array> ocgs = oc_props->GetMutableArrayFor("OCGs");
    if (!ocgs || !ArrayContainsDict(ocgs.Get(), ocg.Get())) return nullptr;

    return FPDFOCGFromCPDFDictionary(ocg.Get());
}

// ============================================================================
// OCG Usage States Implementation (PDF standard)
// ============================================================================

namespace {

// Helper: Get a usage state from /Usage/<category>/<category>State.
// e.g. GetUsageState(dict, "Print", "PrintState") reads /Usage/Print/PrintState.
// Returns true if the state is /ON, false if /OFF.
// Defaults to default_value when the entry is missing.
bool GetUsageState(const CPDF_Dictionary* dict,
                   const char* category,
                   const char* state_key,
                   bool default_value) {
    if (!dict) return default_value;

    RetainPtr<const CPDF_Dictionary> usage = dict->GetDictFor("Usage");
    if (!usage) return default_value;

    RetainPtr<const CPDF_Dictionary> cat = usage->GetDictFor(category);
    if (!cat) return default_value;

    ByteString state = cat->GetByteStringFor(state_key);
    if (state == "ON") return true;
    if (state == "OFF") return false;
    return default_value;
}

// Helper: Set a usage state on /Usage/<category>/<category>State.
// e.g. SetUsageState(dict, "Print", "PrintState", true)
// writes /Usage/Print/PrintState /ON as inline dictionaries (no document needed).
bool SetUsageState(CPDF_Dictionary* dict,
                   const char* category,
                   const char* state_key,
                   bool visible) {
    if (!dict) return false;

    RetainPtr<CPDF_Dictionary> usage = dict->GetMutableDictFor("Usage");
    if (!usage) {
        usage = pdfium::MakeRetain<CPDF_Dictionary>();
        dict->SetFor("Usage", usage);
    }

    RetainPtr<CPDF_Dictionary> cat = usage->GetMutableDictFor(category);
    if (!cat) {
        cat = pdfium::MakeRetain<CPDF_Dictionary>();
        usage->SetFor(category, cat);
    }

    cat->SetNewFor<CPDF_Name>(state_key, visible ? "ON" : "OFF");
    return true;
}

}  // namespace (local for usage helpers)

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_GetPrintState(FPDF_OCG ocg) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    return GetUsageState(dict, "Print", "PrintState", true) ? 1 : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetPrintState(FPDF_OCG ocg, FPDF_BOOL visible) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    return SetUsageState(dict, "Print", "PrintState", visible != 0);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_GetExportState(FPDF_OCG ocg) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    return GetUsageState(dict, "Export", "ExportState", true) ? 1 : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetExportState(FPDF_OCG ocg, FPDF_BOOL visible) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFOCG(ocg);
    return SetUsageState(dict, "Export", "ExportState", visible != 0);
}

// ============================================================================
// OCG Order Implementation
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFOCG_SetOrder(FPDF_DOCUMENT document, const int* order, int count) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc || !order || count <= 0) return false;

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) return false;

    RetainPtr<CPDF_Dictionary> oc_props = root->GetMutableDictFor("OCProperties");
    if (!oc_props) return false;

    RetainPtr<CPDF_Array> ocgs = oc_props->GetMutableArrayFor("OCGs");
    if (!ocgs) return false;

    // Get mutable config
    RetainPtr<CPDF_Dictionary> config = GetOrCreateConfig(doc, oc_props.Get());
    if (!config) return false;

    // Create new Order array
    auto new_order = doc->NewIndirect<CPDF_Array>();
    if (!new_order) return false;

    int valid_count = 0;
    for (int i = 0; i < count; i++) {
        int idx = order[i];
        if (idx >= 0 && idx < static_cast<int>(ocgs->size())) {
            RetainPtr<const CPDF_Dictionary> ocg = ocgs->GetDictAt(idx);
            if (ocg) {
                AppendReferenceTo(new_order.Get(), doc,
                                  const_cast<CPDF_Dictionary*>(ocg.Get()));
                valid_count++;
            }
        }
    }

    // Only update if we added at least one valid entry
    if (valid_count > 0) {
        config->SetNewFor<CPDF_Reference>("Order", doc, new_order->GetObjNum());
        return true;
    }

    return false;
}
