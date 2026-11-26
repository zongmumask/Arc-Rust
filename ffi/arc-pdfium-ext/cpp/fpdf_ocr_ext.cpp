#include "fpdf_ocr_ext.h"

#include <set>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <memory>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// ============================================================================
// Internal Helper Functions
// ============================================================================

namespace {

// Check if text contains only CJK characters (Chinese, Japanese, Korean)
// CJK scripts don't use spaces between words, so skip space insertion
static bool IsCJKOnly(const uint16_t* text, int text_length) {
    if (text_length == 0) return false;
    for (int i = 0; i < text_length; i++) {
        uint16_t cp = text[i];
        if (!((cp >= 0x1100 && cp <= 0x11FF)   // Hangul Jamo
           || (cp >= 0x2E80 && cp <= 0x2EFF)   // CJK Radicals Supplement
           || (cp >= 0x3000 && cp <= 0x303F)   // CJK Symbols and Punctuation
           || (cp >= 0x3040 && cp <= 0x309F)   // Hiragana
           || (cp >= 0x30A0 && cp <= 0x30FF)   // Katakana
           || (cp >= 0x31F0 && cp <= 0x31FF)   // Katakana Phonetic Extensions
           || (cp >= 0x3190 && cp <= 0x319F)   // Kanbun
           || (cp >= 0x3400 && cp <= 0x4DBF)   // CJK Extension A
           || (cp >= 0x4E00 && cp <= 0x9FFF)   // CJK Unified Ideographs
           || (cp >= 0xAC00 && cp <= 0xD7AF)   // Hangul Syllables
           || (cp >= 0xF900 && cp <= 0xFAFF)   // CJK Compatibility Ideographs
           || (cp >= 0xFF00 && cp <= 0xFFEF))) { // Halfwidth and Fullwidth Forms
            return false;
        }
    }
    return true;
}

// Check if two text boxes should have a space inserted between them
bool ShouldInsertSpaceBetween(const FPDF_OCR_TEXTBOX& box1,
                              const FPDF_OCR_TEXTBOX& box2) {
    // CJK scripts don't use spaces between words
    if (IsCJKOnly(box1.text, box1.text_length) && IsCJKOnly(box2.text, box2.text_length)) {
        return false;
    }
    
    // Same line detection
    float midY1 = box1.y + box1.height / 2.0f;
    float midY2 = box2.y + box2.height / 2.0f;
    float averageHeight = (box1.height + box2.height) / 2.0f;
    float yTolerance = averageHeight * 0.4f;
    bool sameLine = std::fabs(midY1 - midY2) <= yTolerance;
    
    // Gap detection
    float right1 = box1.x + box1.width;
    float left2 = box2.x;
    float gap = left2 - right1;
    
    bool hasGap = gap > 0.0f;
    bool gapTooLarge = gap > averageHeight * 1.5f;
    
    return sameLine && hasGap && !gapTooLarge;
}

// Collect unique Unicode characters from text boxes
std::set<uint16_t> CollectUnicodeChars(const FPDF_OCR_TEXTBOX* text_boxes,
                                       int box_count) {
    std::set<uint16_t> chars;
    
    for (int i = 0; i < box_count; i++) {
        const FPDF_OCR_TEXTBOX& box = text_boxes[i];
        for (int j = 0; j < box.text_length; j++) {
            chars.insert(box.text[j]);
        }
    }
    
    // Always include space character
    chars.insert(0x0020);
    
    return chars;
}

// Generate ToUnicode CMap stream content
std::string GenerateToUnicodeCMap(const std::set<uint16_t>& unicode_chars) {
    std::ostringstream cmap;
    cmap << "/CIDInit /ProcSet findresource begin\n"
         << "12 dict begin\n"
         << "begincmap\n"
         << "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
         << "/CMapName /Adobe-Identity-UCS def\n"
         << "/CMapType 2 def\n"
         << "1 begincodespacerange\n"
         << "<0000> <FFFF>\n"
         << "endcodespacerange\n"
         << unicode_chars.size() << " beginbfchar\n";
    
    for (uint16_t cid : unicode_chars) {
        cmap << "<" << std::setw(4) << std::setfill('0') << std::hex << cid << "> <"
             << std::setw(4) << std::setfill('0') << std::hex << cid << ">\n";
    }
    
    cmap << "endbfchar\n"
         << "endcmap\n"
         << "CMapName currentdict /CMap defineresource pop\n"
         << "end\n"
         << "end\n";
    
    return cmap.str();
}

// Generate CIDToGIDMap data (all CIDs map to GID=1)
std::string GenerateCIDToGIDMap(uint16_t max_cid) {
    std::string data;
    data.resize((max_cid + 1) * 2, '\0');
    
    for (uint16_t cid = 0; cid <= max_cid; ++cid) {
        data[cid * 2] = 0;
        data[cid * 2 + 1] = 1;  // All map to GID=1
    }
    
    return data;
}

} // namespace

// ============================================================================
// Font Stream Management
// ============================================================================

FPDF_EXPORT FPDF_FONT_STREAM FPDF_CALLCONV
FPDF_LoadFontStreamFromData(FPDF_DOCUMENT document,
                            const void* font_data,
                            unsigned long font_data_size) {
    if (!document || !font_data || font_data_size == 0) {
        return nullptr;
    }
    
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc) {
        return nullptr;
    }
    
    // Create font file stream
    auto fontFileDict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto fontFileStream = doc->NewIndirect<CPDF_Stream>(std::move(fontFileDict));
    
    ByteString fontBytes(static_cast<const char*>(font_data), font_data_size);
    fontFileStream->SetData(fontBytes.unsigned_span());
    
    // Return as opaque handle (caller must not delete, managed by document)
    return static_cast<FPDF_FONT_STREAM>(fontFileStream.Get());
}

FPDF_EXPORT void FPDF_CALLCONV
FPDF_CloseFontStream(FPDF_FONT_STREAM font_stream) {
    // Font stream is managed by the document, no explicit cleanup needed
    // This function is provided for API symmetry
}

// ============================================================================
// OCR Font Creation
// ============================================================================

FPDF_EXPORT FPDF_OCR_FONT FPDF_CALLCONV
FPDF_CreateOCRFontForPage(FPDF_PAGE page,
                         FPDF_FONT_STREAM font_stream,
                         const unsigned short* unicode_chars,
                         int char_count,
                         const char* font_name) {
    if (!page || !font_stream || !unicode_chars || char_count <= 0 || !font_name) {
        return nullptr;
    }
    
    CPDF_Page* cpdfPage = CPDFPageFromFPDFPage(page);
    if (!cpdfPage) {
        return nullptr;
    }
    
    CPDF_Document* doc = cpdfPage->GetDocument();
    auto resources = cpdfPage->GetMutableResources();
    if (!resources) {
        return nullptr;
    }
    
    CPDF_Stream* fontFileStream = static_cast<CPDF_Stream*>(font_stream);
    
    // Step 1: Collect Unicode characters into a set
    std::set<uint16_t> usedUnicodeSet;
    for (int i = 0; i < char_count; i++) {
        usedUnicodeSet.insert(unicode_chars[i]);
    }
    
    // Step 2: Generate ToUnicode CMap
    std::string cmapContent = GenerateToUnicodeCMap(usedUnicodeSet);
    auto cmapDict = pdfium::MakeRetain<CPDF_Dictionary>();
    cmapDict->SetNewFor<CPDF_Name>("Type", "CMap");
    auto cmapStream = doc->NewIndirect<CPDF_Stream>(std::move(cmapDict));
    ByteString cmapBytes(cmapContent.c_str(), cmapContent.size());
    cmapStream->SetData(cmapBytes.unsigned_span());
    
    // Step 3: Generate CIDToGIDMap
    uint16_t maxCID = 0;
    for (uint16_t ch : usedUnicodeSet) {
        if (ch > maxCID) {
            maxCID = ch;
        }
    }
    
    std::string cidToGidData = GenerateCIDToGIDMap(maxCID);
    auto cidToGidDict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto cidToGidStream = doc->NewIndirect<CPDF_Stream>(std::move(cidToGidDict));
    ByteString gidBytes(cidToGidData.c_str(), cidToGidData.length());
    cidToGidStream->SetData(gidBytes.unsigned_span());
    
    // Step 4: Create CIDFont
    auto descendantFont = doc->NewIndirect<CPDF_Dictionary>();
    descendantFont->SetNewFor<CPDF_Name>("BaseFont", "GlyphLessFont");
    descendantFont->SetNewFor<CPDF_Name>("Subtype", "CIDFontType2");
    descendantFont->SetNewFor<CPDF_Name>("Type", "Font");
    descendantFont->SetNewFor<CPDF_Reference>("CIDToGIDMap", doc, cidToGidStream->GetObjNum());
    
    auto cidSystemInfo = doc->NewIndirect<CPDF_Dictionary>();
    cidSystemInfo->SetNewFor<CPDF_String>("Registry", "Adobe");
    cidSystemInfo->SetNewFor<CPDF_String>("Ordering", "Identity");
    cidSystemInfo->SetNewFor<CPDF_Number>("Supplement", 0);
    descendantFont->SetNewFor<CPDF_Reference>("CIDSystemInfo", doc, cidSystemInfo->GetObjNum());
    
    auto widthsArray = pdfium::MakeRetain<CPDF_Array>();
    widthsArray->AppendNew<CPDF_Number>(0);
    widthsArray->AppendNew<CPDF_Number>(maxCID);
    widthsArray->AppendNew<CPDF_Number>(500);
    descendantFont->SetFor("W", widthsArray);
    
    // FontDescriptor + FontFile2
    auto fontDescriptor = doc->NewIndirect<CPDF_Dictionary>();
    fontDescriptor->SetNewFor<CPDF_Name>("Type", "FontDescriptor");
    fontDescriptor->SetNewFor<CPDF_Name>("FontName", "GlyphLessFont");
    fontDescriptor->SetNewFor<CPDF_Number>("Flags", 4);
    fontDescriptor->SetNewFor<CPDF_Number>("Ascent", 1000);
    fontDescriptor->SetNewFor<CPDF_Number>("Descent", 0);
    fontDescriptor->SetNewFor<CPDF_Number>("CapHeight", 1000);
    fontDescriptor->SetNewFor<CPDF_Number>("ItalicAngle", 0);
    fontDescriptor->SetNewFor<CPDF_Number>("StemV", 75);
    fontDescriptor->SetNewFor<CPDF_Number>("AvgWidth", 500);
    fontDescriptor->SetNewFor<CPDF_Number>("MaxWidth", 500);
    fontDescriptor->SetNewFor<CPDF_Number>("MissingWidth", 500);
    
    auto fontBBox = pdfium::MakeRetain<CPDF_Array>();
    fontBBox->AppendNew<CPDF_Number>(0);
    fontBBox->AppendNew<CPDF_Number>(0);
    fontBBox->AppendNew<CPDF_Number>(500);
    fontBBox->AppendNew<CPDF_Number>(1000);
    fontDescriptor->SetFor("FontBBox", fontBBox);
    
    fontDescriptor->SetNewFor<CPDF_Reference>("FontFile2", doc, fontFileStream->GetObjNum());
    descendantFont->SetNewFor<CPDF_Reference>("FontDescriptor", doc, fontDescriptor->GetObjNum());
    
    // Step 5: Create Type0 Font
    auto type0Font = doc->NewIndirect<CPDF_Dictionary>();
    type0Font->SetNewFor<CPDF_Name>("Type", "Font");
    type0Font->SetNewFor<CPDF_Name>("Subtype", "Type0");
    type0Font->SetNewFor<CPDF_Name>("BaseFont", "GlyphLessFont");
    type0Font->SetNewFor<CPDF_Name>("Encoding", "Identity-H");
    type0Font->SetNewFor<CPDF_Reference>("ToUnicode", doc, cmapStream->GetObjNum());
    
    auto descendants = pdfium::MakeRetain<CPDF_Array>();
    descendants->AppendNew<CPDF_Reference>(doc, descendantFont->GetObjNum());
    type0Font->SetFor("DescendantFonts", descendants);
    
    // Step 6: Add to page resources
    auto fontDict = resources->GetOrCreateDictFor("Font");
    fontDict->SetNewFor<CPDF_Reference>(font_name, doc, type0Font->GetObjNum());
    
    // Create CPDF_Font object
    RetainPtr<CPDF_Font> font = CPDF_Font::Create(doc, type0Font, nullptr);
    if (!font) {
        return nullptr;
    }
    
    // Transfer ownership to caller using Leak()
    // Caller is responsible for calling FPDF_CloseOCRFont to release
    return static_cast<FPDF_OCR_FONT>(font.Leak());
}

FPDF_EXPORT void FPDF_CALLCONV
FPDF_CloseOCRFont(FPDF_OCR_FONT font) {
    if (!font) {
        return;
    }
    
    // Adopt the leaked pointer back into a RetainPtr to properly release it
    CPDF_Font* cpdfFont = static_cast<CPDF_Font*>(font);
    // Use pdfium::WrapRetain to adopt without incrementing reference count
    [[maybe_unused]] RetainPtr<CPDF_Font> fontPtr = pdfium::WrapRetain(cpdfFont);
    // fontPtr will automatically release when it goes out of scope
}

// ============================================================================
// Text Object Addition
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_AddInvisibleTextObject(FPDF_PAGE page,
                            FPDF_OCR_FONT font,
                            const unsigned short* text_utf16,
                            int text_length,
                            float x,
                            float y,
                            float width,
                            float height,
                            float font_size) {
    if (!page || !font || !text_utf16 || text_length <= 0) {
        return false;
    }
    
    CPDF_Page* cpdfPage = CPDFPageFromFPDFPage(page);
    if (!cpdfPage) {
        return false;
    }
    
    CPDF_Font* cpdfFont = static_cast<CPDF_Font*>(font);
    RetainPtr<CPDF_Font> fontPtr(cpdfFont);
    
    // Encode text as UTF-16BE (big-endian)
    std::string encoded;
    encoded.reserve(text_length * 2);
    for (int i = 0; i < text_length; i++) {
        uint16_t ch = text_utf16[i];
        encoded.push_back((ch >> 8) & 0xFF);  // High byte
        encoded.push_back(ch & 0xFF);         // Low byte
    }

    // Calculate text matrix
    float charWidth = width / text_length;
    float scaleX = charWidth / height * 2.0f;
    CFX_Matrix matrix(scaleX, 0, 0, 1, x, y);

    // Create text object
    auto textObj = std::make_unique<CPDF_TextObject>();
    textObj->SetTextRenderMode(TextRenderingMode::MODE_INVISIBLE);
    textObj->mutable_text_state().SetFont(fontPtr);
    textObj->mutable_text_state().SetFontSize(font_size);
    textObj->SetTextMatrix(matrix);
    textObj->SetText(ByteString(encoded.c_str(), encoded.length()));
    textObj->SetDirty(true);
    
    cpdfPage->AppendPageObject(std::move(textObj));
    
    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_AddInvisibleTextObjectsWithSpacing(FPDF_PAGE page,
                                        FPDF_OCR_FONT font,
                                        const FPDF_OCR_TEXTBOX* text_boxes,
                                        int box_count) {
    if (!page || !font || !text_boxes || box_count <= 0) {
        return false;
    }

    CPDF_Page* cpdfPage = CPDFPageFromFPDFPage(page);
    if (!cpdfPage) {
        return false;
    }
    
    CPDF_Font* cpdfFont = static_cast<CPDF_Font*>(font);
    RetainPtr<CPDF_Font> fontPtr(cpdfFont);
    
    const FPDF_OCR_TEXTBOX* prevBox = nullptr;
    
    for (int i = 0; i < box_count; i++) {
        const FPDF_OCR_TEXTBOX& box = text_boxes[i];
        
        // Insert space between boxes if needed
        if (prevBox && ShouldInsertSpaceBetween(*prevBox, box)) {
            float prevRight = prevBox->x + prevBox->width;
            float currLeft = box.x;
            float spaceX = prevRight;
            float spaceY = box.y;
            float spaceHeight = std::max(prevBox->height, box.height);
            float spaceWidth = currLeft - prevRight;
            
            if (spaceWidth > 0) {
                // UTF-16BE encoded space character: U+0020 => 00 20
                char spaceEncoded[2] = {0x00, 0x20};
                
                float scaleX = spaceWidth / spaceHeight * 2.0f;
                CFX_Matrix spaceMatrix(scaleX, 0, 0, 1, spaceX, spaceY);
                
                auto spaceObj = std::make_unique<CPDF_TextObject>();
                spaceObj->SetTextRenderMode(TextRenderingMode::MODE_INVISIBLE);
                spaceObj->mutable_text_state().SetFont(fontPtr);
                spaceObj->mutable_text_state().SetFontSize(spaceHeight);
                spaceObj->SetTextMatrix(spaceMatrix);
                spaceObj->SetText(ByteString(spaceEncoded, 2));
                spaceObj->SetDirty(true);
                cpdfPage->AppendPageObject(std::move(spaceObj));
            }
        }
        
        // Add current text box
        if (!FPDF_AddInvisibleTextObject(page, font, box.text, box.text_length,
                                        box.x, box.y, box.width, box.height,
                                        box.font_size)) {
            return false;
        }
        
        prevBox = &box;
    }
    
    return true;
}

// ============================================================================
// High-Level Convenience API
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_AddInvisibleTextLayer(FPDF_PAGE page,
                           const void* font_data,
                           unsigned long font_data_size,
                           const FPDF_OCR_TEXTBOX* text_boxes,
                           int box_count) {
    if (!page || !font_data || font_data_size == 0 || !text_boxes || box_count <= 0) {
        return false;
    }

    CPDF_Page* cpdfPage = CPDFPageFromFPDFPage(page);
    if (!cpdfPage) {
        return false;
    }
    
    CPDF_Document* doc = cpdfPage->GetDocument();
    if (!doc) {
        return false;
    }
    
    // Step 1: Load font stream
    FPDF_DOCUMENT fpdfDoc = FPDFDocumentFromCPDFDocument(doc);
    FPDF_FONT_STREAM fontStream = FPDF_LoadFontStreamFromData(fpdfDoc, font_data, font_data_size);
    if (!fontStream) {
        return false;
    }
    
    // Step 2: Collect Unicode characters
    std::set<uint16_t> unicodeChars = CollectUnicodeChars(text_boxes, box_count);
    std::vector<uint16_t> charArray(unicodeChars.begin(), unicodeChars.end());
    
    // Step 3: Create OCR font
    FPDF_OCR_FONT font = FPDF_CreateOCRFontForPage(page, fontStream,
                                                   charArray.data(),
                                                   charArray.size(),
                                                   "F1");
    if (!font) {
        FPDF_CloseFontStream(fontStream);
        return false;
    }
    
    // Step 4: Add text objects with spacing
    FPDF_BOOL result = FPDF_AddInvisibleTextObjectsWithSpacing(page, font, text_boxes, box_count);
    
    // Step 5: Generate page content
    if (result) {
        FPDFPage_GenerateContent(page);
    }
    
    // Step 6: Cleanup
    FPDF_CloseOCRFont(font);
    FPDF_CloseFontStream(fontStream);
    
    return result;
}
