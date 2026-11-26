#include "fpdfview_ext.h"

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_edit.h"

#include <vector>
#include <set>

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_ReorderPages(FPDF_DOCUMENT document, const int* page_indices, int count) {
    CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
    if (!pDoc || !page_indices) {
        return false;
    }

    int page_count = pDoc->GetPageCount();
    
    // Validate count matches page count
    if (count != page_count) {
        return false;
    }

    // Validate that all indices are valid and unique
    std::set<int> index_set;
    for (int i = 0; i < count; i++) {
        int idx = page_indices[i];
        if (idx < 0 || idx >= page_count) {
            return false;
        }
        if (index_set.count(idx) > 0) {
            // Duplicate index found
            return false;
        }
        index_set.insert(idx);
    }

    // Use FPDF_MovePages to reorder pages
    // Strategy: Move pages one by one to their target positions
    // We need to track the current positions as we move pages
    std::vector<int> current_order;
    current_order.reserve(page_count);
    for (int i = 0; i < page_count; i++) {
        current_order.push_back(i);
    }

    // Build the target order from page_indices
    std::vector<int> target_order(page_indices, page_indices + count);

    // Perform moves to achieve the target order
    for (int target_pos = 0; target_pos < page_count; target_pos++) {
        int target_page = target_order[target_pos];
        
        // Find current position of the target page
        int current_pos = -1;
        for (int i = 0; i < page_count; i++) {
            if (current_order[i] == target_page) {
                current_pos = i;
                break;
            }
        }
        
        if (current_pos == -1) {
            return false; // Should never happen if validation passed
        }
        
        // If the page is already in the correct position, skip
        if (current_pos == target_pos) {
            continue;
        }
        
        // Move the page from current_pos to target_pos
        int page_to_move = current_pos;
        if (!FPDF_MovePages(document, &page_to_move, 1, target_pos)) {
            return false;
        }
        
        // Update current_order to reflect the move
        int moved_page = current_order[current_pos];
        current_order.erase(current_order.begin() + current_pos);
        current_order.insert(current_order.begin() + target_pos, moved_page);
    }

    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentHasPageLabel(FPDF_DOCUMENT document) {
    CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
    if (!pDoc) {
        return false;
    }

    // Get the root dictionary
    RetainPtr<CPDF_Dictionary> pRoot = pDoc->GetMutableRoot();
    if (!pRoot) {
        return false;
    }

    // Check if the PageLabels entry exists in the document catalog
    // PageLabels is a number tree that defines custom page numbering
    RetainPtr<CPDF_Object> pPageLabels = pRoot->GetMutableObjectFor("PageLabels");
    
    // If PageLabels exists and is a dictionary, the document has page labels
    return pPageLabels && pPageLabels->IsDictionary();
}


