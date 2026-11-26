#include "fpdf_font_subset.h"
#include "fpdf_font_ext.h"

#include <cstring>
#include <memory>
#include <set>
#include <vector>

// HarfBuzz headers
#include <hb.h>
#include <hb-subset.h>

// Link HarfBuzz libraries
#pragma comment(lib, "harfbuzz")
#pragma comment(lib, "harfbuzz-subset")

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// Helper function to create a font subset using HarfBuzz
std::vector<uint8_t> CreateFontSubset(
    const void* font_data,
    unsigned long font_data_size,
    const std::set<uint16_t>& unicode_chars) {
    
    // Create HarfBuzz blob from font data
    hb_blob_t* blob = hb_blob_create(
        static_cast<const char*>(font_data),
        font_data_size,
        HB_MEMORY_MODE_READONLY,
        nullptr,
        nullptr
    );
    
    if (!blob) {
        return {};
    }
    
    // Create HarfBuzz face
    hb_face_t* face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    
    if (!face) {
        return {};
    }
    
    // Create subset input
    hb_subset_input_t* input = hb_subset_input_create_or_fail();
    if (!input) {
        hb_face_destroy(face);
        return {};
    }
    
    // Add Unicode code points to subset
    hb_set_t* unicodes = hb_subset_input_unicode_set(input);
    for (uint16_t unicode : unicode_chars) {
        hb_set_add(unicodes, unicode);
    }
    
    // Configure subset options for minimal size
    hb_subset_input_set_flags(input,
        HB_SUBSET_FLAGS_RETAIN_GIDS |           // Retain glyph IDs
        HB_SUBSET_FLAGS_NOTDEF_OUTLINE |        // Keep .notdef glyph
        HB_SUBSET_FLAGS_NO_HINTING              // Remove hinting for smaller size
    );
    
    // Perform subsetting
    hb_face_t* subset_face = hb_subset_or_fail(face, input);
    
    // Clean up input and original face
    hb_subset_input_destroy(input);
    hb_face_destroy(face);
    
    if (!subset_face) {
        return {};
    }
    
    // Get subset font data
    hb_blob_t* subset_blob = hb_face_reference_blob(subset_face);
    hb_face_destroy(subset_face);
    
    if (!subset_blob) {
        return {};
    }
    
    // Copy subset data to vector
    unsigned int subset_size = 0;
    const char* subset_data = hb_blob_get_data(subset_blob, &subset_size);
    
    std::vector<uint8_t> result;
    if (subset_data && subset_size > 0) {
        result.assign(subset_data, subset_data + subset_size);
    }
    
    hb_blob_destroy(subset_blob);
    
    return result;
}

} // namespace

FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFFont_CreateType0FromSubset(FPDF_DOCUMENT document,
                                const void* font_data,
                                unsigned long font_data_size,
                                const unsigned short* unicode_chars,
                                int char_count,
                                const char* font_name) {
    // Validate input parameters
    if (!document || !font_data || font_data_size == 0 || !unicode_chars || char_count <= 0) {
        return nullptr;
    }
    
    // Collect Unicode characters into a set
    std::set<uint16_t> unicode_set;
    for (int i = 0; i < char_count; i++) {
        unicode_set.insert(unicode_chars[i]);
    }
    
    // Create font subset using HarfBuzz
    std::vector<uint8_t> subset_data = CreateFontSubset(font_data, font_data_size, unicode_set);
    
    if (subset_data.empty()) {
        // Fallback to full font if subsetting fails
        return FPDFFont_CreateType0FromData(
            document,
            font_data,
            font_data_size,
            unicode_chars,
            char_count,
            font_name
        );
    }
    
    // Create Type0 font using the subsetted font data
    return FPDFFont_CreateType0FromData(
        document,
        subset_data.data(),
        subset_data.size(),
        unicode_chars,
        char_count,
        font_name
    );
}
