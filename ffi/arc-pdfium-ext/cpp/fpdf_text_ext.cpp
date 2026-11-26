#include "fpdf_text_ext.h"

#include <cmath>
#include <vector>

#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/font/cpdf_cidfont.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

// Size epsilon for boundary checks (same as PDFium's kSizeEpsilon)
constexpr float kSizeEpsilon = 0.001f;

// Get CPDF_TextObject from FPDF_PAGEOBJECT
CPDF_TextObject* GetTextObject(FPDF_PAGEOBJECT text_object) {
    CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(text_object);
    if (!pPageObj || !pPageObj->IsText()) {
        return nullptr;
    }
    return pPageObj->AsText();
}

}  // namespace

// ============================================================================
// Text Object Character Information API Implementation
// ============================================================================

FPDF_EXPORT int FPDF_CALLCONV
FPDFTextObj_CountChars(FPDF_PAGEOBJECT text_object) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return 0;
    }
    
    return static_cast<int>(pTextObj->CharCount());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetCharInfo(FPDF_PAGEOBJECT text_object,
                        int char_index,
                        uint32_t* char_code,
                        double* origin_x,
                        double* origin_y) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return false;
    }
    
    // Validate char_index
    if (char_index < 0 || static_cast<size_t>(char_index) >= pTextObj->CharCount()) {
        return false;
    }
    
    // Get character information
    CPDF_TextObject::Item item = pTextObj->GetCharInfo(char_index);
    
    // Fill output parameters
    if (char_code) {
        *char_code = item.char_code_;
    }
    if (origin_x) {
        *origin_x = item.origin_.x;
    }
    if (origin_y) {
        *origin_y = item.origin_.y;
    }
    
    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetCharBox(FPDF_PAGEOBJECT text_object,
                       int char_index,
                       double* left,
                       double* right,
                       double* bottom,
                       double* top) {
    // Validate output parameters
    if (!left || !right || !bottom || !top) {
        return false;
    }
    
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return false;
    }
    
    // Validate char_index
    if (char_index < 0 || static_cast<size_t>(char_index) >= pTextObj->CharCount()) {
        return false;
    }
    
    // Get character information
    CPDF_TextObject::Item item = pTextObj->GetCharInfo(char_index);
    
    // Get font
    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return false;
    }
    
    // ========================================================================
    // Character Box Calculation (same logic as FPDFText_GetCharBox)
    // ========================================================================
    
    // Step 1: Get character bounding box from font (in font units, 1/1000 em)
    FX_RECT rect = font->GetCharBBox(item.char_code_);
    
    // Step 2: Get font size and calculate scale factor
    // Font size is typically 1.0, actual size is controlled by matrix
    float fFontSize = pTextObj->GetFontSize() / 1000.0f;
    
    // Step 3: Calculate character box in local coordinates
    // Formula: font_bbox * scale + origin
    CFX_FloatRect char_box(
        rect.left * fFontSize + item.origin_.x,      // left
        rect.bottom * fFontSize + item.origin_.y,    // bottom
        rect.right * fFontSize + item.origin_.x,     // right
        rect.top * fFontSize + item.origin_.y        // top
    );
    
    // Step 4: Handle edge cases
    
    // If height is too small, use font size as height
    if (fabsf(char_box.top - char_box.bottom) < kSizeEpsilon) {
        char_box.top = char_box.bottom + fFontSize;
    }
    
    // If width is too small, use character width
    if (fabsf(char_box.right - char_box.left) < kSizeEpsilon) {
        char_box.right = char_box.left + pTextObj->GetCharWidth(item.char_code_);
    }
    
    // Step 5: Apply transformation matrix to convert to page coordinates
    // Get text matrix (without position offset)
    CFX_Matrix matrix = pTextObj->GetTextMatrix();
    
    // Transform the character box
    char_box = matrix.TransformRect(char_box);
    
    // Step 6: Return results
    *left = char_box.left;
    *right = char_box.right;
    *bottom = char_box.bottom;
    *top = char_box.top;
    
    return true;
}

FPDF_EXPORT float FPDF_CALLCONV
FPDFTextObj_GetCharSpacing(FPDF_PAGEOBJECT text_object) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return 0.0f;
    }
    return pTextObj->text_state().GetCharSpace();
}

FPDF_EXPORT float FPDF_CALLCONV
FPDFTextObj_GetWordSpacing(FPDF_PAGEOBJECT text_object) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return 0.0f;
    }
    return pTextObj->text_state().GetWordSpace();
}

FPDF_EXPORT void FPDF_CALLCONV
FPDFTextObj_SetCharSpacing(FPDF_PAGEOBJECT text_object, float char_spacing) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return;
    }
    pTextObj->mutable_text_state().SetCharSpace(char_spacing);
    pTextObj->SetDirty(true);
}

FPDF_EXPORT void FPDF_CALLCONV
FPDFTextObj_SetWordSpacing(FPDF_PAGEOBJECT text_object, float word_spacing) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return;
    }
    pTextObj->mutable_text_state().SetWordSpace(word_spacing);
    pTextObj->SetDirty(true);
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFTextObj_GetItemCount(FPDF_PAGEOBJECT text_object) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return 0;
    }
    return static_cast<int>(pTextObj->CountItems());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetItemInfo(FPDF_PAGEOBJECT text_object,
                        int index,
                        uint32_t* char_code,
                        float* kerning) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj || index < 0 ||
        static_cast<size_t>(index) >= pTextObj->CountItems()) {
        return false;
    }
    CPDF_TextObject::Item item = pTextObj->GetItemInfo(static_cast<size_t>(index));
    if (char_code) {
        *char_code = item.char_code_;
    }
    // Legacy marker items: origin_.x holds the raw kerning value (not
    // overwritten by CalcPositionDataInternal). NEW PDFium moved word-gap
    // kerning into the per-glyph `char_kernings_` array (no more kInvalid
    // placeholder items), so read it from there for real glyphs.
    if (kerning) {
        if (item.char_code_ == CPDF_Font::kInvalidCharCode) {
            *kerning = item.origin_.x;
        } else {
            const auto& kernings = pTextObj->GetCharKernings();
            *kerning = index < static_cast<int>(kernings.size())
                           ? kernings[index]
                           : 0.0f;
        }
    }
    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetTextWithKerning(FPDF_PAGEOBJECT text_object,
                               const FPDF_WCHAR** utf16_segments,
                               const float* kernings,
                               int segment_count) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj || !utf16_segments || segment_count < 1) {
        return false;
    }

    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return false;
    }

    // Convert each UTF-16 segment to a font-encoded ByteString
    std::vector<ByteString> byte_segments;
    byte_segments.reserve(static_cast<size_t>(segment_count));

    for (int i = 0; i < segment_count; ++i) {
        if (!utf16_segments[i]) {
            return false;
        }
        // Measure UTF-16 string length
        const FPDF_WCHAR* p = utf16_segments[i];
        size_t char_count = 0;
        while (p[char_count]) ++char_count;

        ByteString byte_str;
        for (size_t j = 0; j < char_count; ++j) {
            uint32_t charcode = font->CharCodeFromUnicode(
                static_cast<wchar_t>(p[j]));
            if (charcode != CPDF_Font::kInvalidCharCode) {
                font->AppendChar(&byte_str, charcode);
            }
        }
        byte_segments.push_back(std::move(byte_str));
    }

    // Build kerning span (length = segment_count - 1)
    std::vector<float> kerning_vec;
    if (kernings && segment_count > 1) {
        kerning_vec.assign(kernings, kernings + (segment_count - 1));
    } else {
        kerning_vec.resize(static_cast<size_t>(std::max(0, segment_count - 1)), 0.0f);
    }

    pTextObj->SetSegments(
        pdfium::span<const ByteString>(byte_segments),
        pdfium::span<const float>(kerning_vec));
    
    // CRITICAL: Save kerning values before CalcPositionData overwrites them
    // CalcPositionDataInternal modifies char_pos_[i-1] for real characters,
    // but we need to preserve the original kerning values for kInvalidCharCode markers.
    const std::vector<uint32_t>& char_codes = pTextObj->GetCharCodes();
    const std::vector<float>& char_pos = pTextObj->GetCharPositions();
    std::vector<float> saved_kernings;
    saved_kernings.reserve(char_codes.size());
    
    // Save kerning values (stored at char_pos[i-1] for kInvalidCharCode at position i)
    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0) {
            saved_kernings.push_back(char_pos[i - 1]);            
        }
    }
    
    pTextObj->CalcPositionData(1.0f);
    
    // Restore kerning values after CalcPositionData
    size_t kerning_idx = 0;
    std::vector<float>& mutable_char_pos = const_cast<std::vector<float>&>(pTextObj->GetCharPositions());
    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0 && kerning_idx < saved_kernings.size()) {
            mutable_char_pos[i - 1] = saved_kernings[kerning_idx];
            kerning_idx++;
        }
    }
    
    pTextObj->SetDirty(true);
    return true;
}

// ============================================================================
// Raw charcodes with kerning
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetCharcodesWithKerning(FPDF_PAGEOBJECT text_object,
                                     const uint32_t* charcodes,
                                     const int* segment_lengths,
                                     const float* kernings,
                                     int segment_count) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj || !charcodes || !segment_lengths || segment_count < 1) {
        return false;
    }

    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return false;
    }

    // Build ByteString segments from raw charcodes (no Unicode→charcode lookup)
    std::vector<ByteString> byte_segments;
    byte_segments.reserve(static_cast<size_t>(segment_count));

    int offset = 0;
    for (int i = 0; i < segment_count; ++i) {
        int count = segment_lengths[i];
        if (count < 0) {
            return false;
        }
        ByteString byte_str;
        for (int j = 0; j < count; ++j) {
            font->AppendChar(&byte_str, charcodes[offset + j]);
        }
        byte_segments.push_back(std::move(byte_str));
        offset += count;
    }

    // Build kerning span (length = segment_count - 1)
    std::vector<float> kerning_vec;
    if (kernings && segment_count > 1) {
        kerning_vec.assign(kernings, kernings + (segment_count - 1));
    } else {
        kerning_vec.resize(static_cast<size_t>(std::max(0, segment_count - 1)), 0.0f);
    }

    pTextObj->SetSegments(
        pdfium::span<const ByteString>(byte_segments),
        pdfium::span<const float>(kerning_vec));

    // Save and restore kerning values (same pattern as SetTextWithKerning)
    const std::vector<uint32_t>& char_codes = pTextObj->GetCharCodes();
    const std::vector<float>& char_pos = pTextObj->GetCharPositions();
    std::vector<float> saved_kernings;
    saved_kernings.reserve(char_codes.size());

    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0) {
            saved_kernings.push_back(char_pos[i - 1]);
        }
    }

    pTextObj->CalcPositionData(1.0f);

    size_t kerning_idx = 0;
    std::vector<float>& mutable_char_pos = const_cast<std::vector<float>&>(pTextObj->GetCharPositions());
    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0 && kerning_idx < saved_kernings.size()) {
            mutable_char_pos[i - 1] = saved_kernings[kerning_idx];
            kerning_idx++;
        }
    }

    pTextObj->SetDirty(true);
    return true;
}

// Safe variant: takes explicit segment_lengths instead of scanning for a null
// terminator, so non-null-terminated UTF-16 segments cannot read out of bounds.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetTextWithKerning2(FPDF_PAGEOBJECT text_object,
                                const FPDF_WCHAR** utf16_segments,
                                const int* segment_lengths,
                                const float* kernings,
                                int segment_count) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj || !utf16_segments || !segment_lengths || segment_count < 1) {
        return false;
    }

    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return false;
    }

    // Convert each UTF-16 segment (of explicit length) to a font-encoded
    // ByteString. No null-terminator required.
    std::vector<ByteString> byte_segments;
    byte_segments.reserve(static_cast<size_t>(segment_count));

    for (int i = 0; i < segment_count; ++i) {
        if (!utf16_segments[i] || segment_lengths[i] < 0) {
            return false;
        }
        size_t char_count = static_cast<size_t>(segment_lengths[i]);
        const FPDF_WCHAR* p = utf16_segments[i];

        ByteString byte_str;
        for (size_t j = 0; j < char_count; ++j) {
            uint32_t charcode = font->CharCodeFromUnicode(
                static_cast<wchar_t>(p[j]));
            if (charcode != CPDF_Font::kInvalidCharCode) {
                font->AppendChar(&byte_str, charcode);
            }
        }
        byte_segments.push_back(std::move(byte_str));
    }

    // Build kerning span (length = segment_count - 1)
    std::vector<float> kerning_vec;
    if (kernings && segment_count > 1) {
        kerning_vec.assign(kernings, kernings + (segment_count - 1));
    } else {
        kerning_vec.resize(static_cast<size_t>(std::max(0, segment_count - 1)), 0.0f);
    }

    pTextObj->SetSegments(
        pdfium::span<const ByteString>(byte_segments),
        pdfium::span<const float>(kerning_vec));

    // CRITICAL: Save kerning values before CalcPositionData overwrites them.
    const std::vector<uint32_t>& char_codes = pTextObj->GetCharCodes();
    const std::vector<float>& char_pos = pTextObj->GetCharPositions();
    std::vector<float> saved_kernings;
    saved_kernings.reserve(char_codes.size());

    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0) {
            saved_kernings.push_back(char_pos[i - 1]);
        }
    }

    pTextObj->CalcPositionData(1.0f);

    size_t kerning_idx = 0;
    std::vector<float>& mutable_char_pos =
        const_cast<std::vector<float>&>(pTextObj->GetCharPositions());
    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0 &&
            kerning_idx < saved_kernings.size()) {
            mutable_char_pos[i - 1] = saved_kernings[kerning_idx];
            kerning_idx++;
        }
    }

    pTextObj->SetDirty(true);
    return true;
}

// Safe variant: takes charcodes_count to bound the charcodes array, so a
// segment_lengths sum exceeding the array cannot read out of bounds.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetCharcodesWithKerning2(FPDF_PAGEOBJECT text_object,
                                     const uint32_t* charcodes,
                                     int charcodes_count,
                                     const int* segment_lengths,
                                     const float* kernings,
                                     int segment_count) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj || !charcodes || !segment_lengths || segment_count < 1 ||
        charcodes_count < 0) {
        return false;
    }

    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return false;
    }

    // Build ByteString segments from raw charcodes, validating that the
    // cumulative segment length does not exceed charcodes_count.
    std::vector<ByteString> byte_segments;
    byte_segments.reserve(static_cast<size_t>(segment_count));

    int offset = 0;
    for (int i = 0; i < segment_count; ++i) {
        int count = segment_lengths[i];
        if (count < 0 || count > charcodes_count - offset) {
            return false;
        }
        ByteString byte_str;
        for (int j = 0; j < count; ++j) {
            font->AppendChar(&byte_str, charcodes[offset + j]);
        }
        byte_segments.push_back(std::move(byte_str));
        offset += count;
    }

    // Build kerning span (length = segment_count - 1)
    std::vector<float> kerning_vec;
    if (kernings && segment_count > 1) {
        kerning_vec.assign(kernings, kernings + (segment_count - 1));
    } else {
        kerning_vec.resize(static_cast<size_t>(std::max(0, segment_count - 1)), 0.0f);
    }

    pTextObj->SetSegments(
        pdfium::span<const ByteString>(byte_segments),
        pdfium::span<const float>(kerning_vec));

    // Save and restore kerning values (same pattern as SetTextWithKerning)
    const std::vector<uint32_t>& char_codes = pTextObj->GetCharCodes();
    const std::vector<float>& char_pos = pTextObj->GetCharPositions();
    std::vector<float> saved_kernings;
    saved_kernings.reserve(char_codes.size());

    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0) {
            saved_kernings.push_back(char_pos[i - 1]);
        }
    }

    pTextObj->CalcPositionData(1.0f);

    size_t kerning_idx = 0;
    std::vector<float>& mutable_char_pos =
        const_cast<std::vector<float>&>(pTextObj->GetCharPositions());
    for (size_t i = 0; i < char_codes.size(); ++i) {
        if (char_codes[i] == CPDF_Font::kInvalidCharCode && i > 0 &&
            kerning_idx < saved_kernings.size()) {
            mutable_char_pos[i - 1] = saved_kernings[kerning_idx];
            kerning_idx++;
        }
    }

    pTextObj->SetDirty(true);
    return true;
}

// ============================================================================
// Charcode → Unicode conversion
// ============================================================================

FPDF_EXPORT unsigned int FPDF_CALLCONV
FPDFTextObj_GetCharUnicode(FPDF_PAGEOBJECT text_object, int char_index) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return 0;
    }
    if (char_index < 0 || static_cast<size_t>(char_index) >= pTextObj->CharCount()) {
        return 0;
    }
    CPDF_TextObject::Item item = pTextObj->GetCharInfo(char_index);
    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return item.char_code_;
    }
    WideString unicode = font->UnicodeFromCharCode(item.char_code_);
    if (unicode.IsEmpty()) {
        return 0;
    }
    return static_cast<unsigned int>(unicode[0]);
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFTextObj_GetCharUnicodeString(FPDF_PAGEOBJECT text_object,
                                  int char_index,
                                  unsigned short* buffer,
                                  unsigned long buflen) {
    CPDF_TextObject* pTextObj = GetTextObject(text_object);
    if (!pTextObj) {
        return 0;
    }
    if (char_index < 0 || static_cast<size_t>(char_index) >= pTextObj->CharCount()) {
        return 0;
    }
    CPDF_TextObject::Item item = pTextObj->GetCharInfo(char_index);
    RetainPtr<CPDF_Font> font = pTextObj->GetFont();
    if (!font) {
        return 0;
    }
    WideString unicode = font->UnicodeFromCharCode(item.char_code_);
    if (unicode.IsEmpty()) {
        return 0;
    }
    unsigned long len = unicode.GetLength();
    if (buffer && buflen >= len) {
        for (size_t i = 0; i < len; i++) {
            buffer[i] = static_cast<unsigned short>(unicode[i]);
        }
    }
    return len;
}
