#include "fpdf_bookmark_ext.h"

#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

// Get mutable dictionary from FPDF_BOOKMARK
RetainPtr<CPDF_Dictionary> GetMutableBookmarkDict(FPDF_BOOKMARK bookmark) {
    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFBookmark(bookmark);
    return pdfium::WrapRetain(dict);
}

// Returns true if |dict| is an indirect object owned by |doc| (rejects
// cross-document handles). Inline objects (ObjNum 0) are accepted. A freed
// handle cannot be safely validated; this guards the cross-document case where
// the dict is alive in another document.
bool BelongsToDocument(CPDF_Dictionary* dict, CPDF_Document* doc) {
    if (!dict || !doc) return false;
    const uint32_t obj_num = dict->GetObjNum();
    if (obj_num == 0) return true;  // inline object
    // Query the in-memory indirect object table rather than re-parsing from the
    // file. NewIndirect objects created by FPDFExt_CreateBookmark are tracked in
    // that table; ParseIndirectObject() reads from the file and would return a
    // different instance, wrongly rejecting bookmarks created this session.
    RetainPtr<const CPDF_Object> obj = doc->GetIndirectObject(obj_num);
    return obj.Get() == static_cast<CPDF_Object*>(dict);
}

// Get mutable dictionary from parent/root
RetainPtr<CPDF_Dictionary> GetMutableDictFor(CPDF_Dictionary* parent, ByteStringView key) {
    return parent->GetMutableDictFor(key);
}

void SetReferenceFor(CPDF_Dictionary* owner,
                     const char* key,
                     CPDF_Document* doc,
                     CPDF_Dictionary* target) {
    if (!owner || !doc || !target) {
        return;
    }

    const uint32_t obj_num = target->GetObjNum();
    if (obj_num != 0) {
        owner->SetNewFor<CPDF_Reference>(key, doc, obj_num);
    } else {
        owner->SetFor(key, pdfium::WrapRetain(target));
    }
}

}  // namespace

// ============================================================================
// Bookmark Creation API Implementation
// ============================================================================

FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
FPDFExt_CreateBookmark(FPDF_DOCUMENT document,
                       FPDF_WIDESTRING title,
                       int page_index,
                       double top_y,
                       FPDF_BOOKMARK parent,
                       FPDF_BOOKMARK insert_after) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) {
        return nullptr;
    }

    // Convert title
    WideString ws_title = WideStringFromFPDFWideString(title);

    // Create bookmark dictionary
    auto bookmark_dict = doc->NewIndirect<CPDF_Dictionary>();
    bookmark_dict->SetNewFor<CPDF_String>("Title", WideStringView(ws_title));

    // Set destination
    if (page_index >= 0 && page_index < static_cast<int>(doc->GetPageCount())) {
        auto action_dict = doc->New<CPDF_Dictionary>();
        action_dict->SetNewFor<CPDF_Name>("S", "GoTo");

        auto dest_array = doc->New<CPDF_Array>();
        // Get page object number
        RetainPtr<const CPDF_Dictionary> page_dict = doc->GetPageDictionary(page_index);
        if (page_dict) {
            dest_array->AppendNew<CPDF_Reference>(doc, page_dict->GetObjNum());
        }
        if (top_y >= 0) {
            // Precisely position within the page: [page /XYZ left top zoom].
            // top_y is in PDF page space (bottom-left origin). Zoom 0 keeps the
            // viewer's current zoom level unchanged.
            dest_array->AppendNew<CPDF_Name>("XYZ");
            dest_array->AppendNew<CPDF_Number>(0.0f);                          // left
            dest_array->AppendNew<CPDF_Number>(static_cast<float>(top_y));     // top
            dest_array->AppendNew<CPDF_Number>(0.0f);                          // zoom
        } else {
            dest_array->AppendNew<CPDF_Name>("Fit");
        }

        action_dict->SetFor("D", dest_array);
        bookmark_dict->SetFor("A", action_dict);
    }

    // Get root and create/get Outlines
    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) {
        return nullptr;
    }

    RetainPtr<CPDF_Dictionary> outlines = GetMutableDictFor(root.Get(), "Outlines");
    if (!outlines) {
        outlines = doc->NewIndirect<CPDF_Dictionary>();
        SetReferenceFor(root.Get(), "Outlines", doc, outlines.Get());
    }

    // Get parent
    RetainPtr<CPDF_Dictionary> parent_dict;
    if (parent) {
        parent_dict = GetMutableBookmarkDict(parent);
        // Reject cross-document / stale-but-alive parent handles.
        if (!BelongsToDocument(parent_dict.Get(), doc)) {
            return nullptr;
        }
    } else {
        parent_dict = outlines;
    }

    // Get first/last children
    RetainPtr<CPDF_Dictionary> first = GetMutableDictFor(parent_dict.Get(), "First");
    RetainPtr<CPDF_Dictionary> last = GetMutableDictFor(parent_dict.Get(), "Last");

    if (!first) {
        // First child
        SetReferenceFor(parent_dict.Get(), "First", doc, bookmark_dict.Get());
        SetReferenceFor(parent_dict.Get(), "Last", doc, bookmark_dict.Get());
    } else if (insert_after) {
        // Insert after sibling
        RetainPtr<CPDF_Dictionary> sibling = GetMutableBookmarkDict(insert_after);
        // Reject cross-document / stale-but-alive sibling handles.
        if (!BelongsToDocument(sibling.Get(), doc)) {
            return nullptr;
        }
        RetainPtr<CPDF_Dictionary> next = GetMutableDictFor(sibling.Get(), "Next");

        SetReferenceFor(sibling.Get(), "Next", doc, bookmark_dict.Get());
        SetReferenceFor(bookmark_dict.Get(), "Prev", doc, sibling.Get());

        if (next) {
            SetReferenceFor(bookmark_dict.Get(), "Next", doc, next.Get());
            SetReferenceFor(next.Get(), "Prev", doc, bookmark_dict.Get());
        } else {
            SetReferenceFor(parent_dict.Get(), "Last", doc, bookmark_dict.Get());
        }
    } else {
        // Insert at end
        if (last) {
            SetReferenceFor(last.Get(), "Next", doc, bookmark_dict.Get());
            SetReferenceFor(bookmark_dict.Get(), "Prev", doc, last.Get());
        }
        SetReferenceFor(parent_dict.Get(), "Last", doc, bookmark_dict.Get());
    }

    SetReferenceFor(bookmark_dict.Get(), "Parent", doc, parent_dict.Get());

    // Update count
    int count = 0;
    RetainPtr<CPDF_Dictionary> child = GetMutableDictFor(parent_dict.Get(), "First");
    while (child) {
        count++;
        child = GetMutableDictFor(child.Get(), "Next");
    }
    parent_dict->SetNewFor<CPDF_Number>("Count", count);

    return FPDFBookmarkFromCPDFDictionary(bookmark_dict.Get());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_SetBookmarkTitle(FPDF_BOOKMARK bookmark, FPDF_WIDESTRING title) {
    if (!bookmark || !title) {
        return false;
    }

    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFBookmark(bookmark);
    WideString ws_title = WideStringFromFPDFWideString(title);
    dict->SetNewFor<CPDF_String>("Title", WideStringView(ws_title));
    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_SetBookmarkDest(FPDF_BOOKMARK bookmark,
                        int page_index,
                        int fit_type,
                        double x,
                        double y,
                        double zoom) {
    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_DeleteBookmark(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc || !bookmark) {
        return false;
    }

    CPDF_Dictionary* dict = CPDFDictionaryFromFPDFBookmark(bookmark);

    RetainPtr<CPDF_Dictionary> parent = GetMutableDictFor(dict, "Parent");
    RetainPtr<CPDF_Dictionary> prev = GetMutableDictFor(dict, "Prev");
    RetainPtr<CPDF_Dictionary> next = GetMutableDictFor(dict, "Next");

    if (parent) {
        // Update sibling chain
        if (prev) {
            if (next) {
                SetReferenceFor(prev.Get(), "Next", doc, next.Get());
            } else {
                prev->RemoveFor("Next");
            }
        }
        if (next) {
            if (prev) {
                SetReferenceFor(next.Get(), "Prev", doc, prev.Get());
            } else {
                next->RemoveFor("Prev");
            }
        }

        // Update First/Last
        RetainPtr<CPDF_Dictionary> first = GetMutableDictFor(parent.Get(), "First");
        RetainPtr<CPDF_Dictionary> last = GetMutableDictFor(parent.Get(), "Last");

        if (first.Get() == dict) {
            if (next) {
                SetReferenceFor(parent.Get(), "First", doc, next.Get());
            } else {
                parent->RemoveFor("First");
            }
        }
        if (last.Get() == dict) {
            if (prev) {
                SetReferenceFor(parent.Get(), "Last", doc, prev.Get());
            } else {
                parent->RemoveFor("Last");
            }
        }

        // Update count
        int count = 0;
        RetainPtr<CPDF_Dictionary> child = GetMutableDictFor(parent.Get(), "First");
        while (child) {
            count++;
            child = GetMutableDictFor(child.Get(), "Next");
        }
        if (count > 0) {
            parent->SetNewFor<CPDF_Number>("Count", count);
        } else {
            parent->RemoveFor("Count");
        }
    }

    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFExt_ClearAllBookmarks(FPDF_DOCUMENT document) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) {
        return false;
    }

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) {
        return false;
    }

    root->RemoveFor("Outlines");
    return true;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFExt_GetBookmarkCount(FPDF_DOCUMENT document) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) {
        return 0;
    }

    RetainPtr<CPDF_Dictionary> root = doc->GetMutableRoot();
    if (!root) {
        return 0;
    }

    RetainPtr<CPDF_Dictionary> outlines = GetMutableDictFor(root.Get(), "Outlines");
    if (!outlines) {
        return 0;
    }

    int count = 0;
    RetainPtr<CPDF_Dictionary> child = GetMutableDictFor(outlines.Get(), "First");
    while (child) {
        count++;
        child = GetMutableDictFor(child.Get(), "Next");
    }

    return count;
}
