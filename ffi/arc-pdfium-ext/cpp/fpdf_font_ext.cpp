#include "fpdf_font_ext.h"

// freetype 完整类型必须最先引入:pdfium 内部头(core/fxge/cfx_face.h)会前向声明
// FT_FaceRec_,若 freetype.h 在其后才展开,FX Face 在本遍历内仍是不完整类型,
// 访问 face->units_per_EM / face->glyph 会报 member access into incomplete type。
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <iomanip>

#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/font/cpdf_cidfont.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

// ============================================================================
// Page Font Enumeration API
// ============================================================================

FPDF_EXPORT int FPDF_CALLCONV
FPDFPage_GetFontCount(FPDF_PAGE page) {
    // 2.1.1 Get the CPDF_Page object from FPDF_PAGE
    CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
    if (!pPage) {
        return -1;
    }
    
    // 2.1.2 Get the page resource dictionary
    RetainPtr<const CPDF_Dictionary> pResources = pPage->GetPageResources();
    if (!pResources) {
        return 0;
    }
    
    // 2.1.3 Get the Font dictionary and return its size
    RetainPtr<const CPDF_Dictionary> pFonts = pResources->GetDictFor("Font");
    if (!pFonts) {
        return 0;
    }
    
    // 2.1.4 Return the size of the Font dictionary
    return static_cast<int>(pFonts->size());
}

FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFPage_GetFont(FPDF_PAGE page, int index) {
    // 2.2.1 Get the page's Font dictionary
    CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
    if (!pPage) {
        return nullptr;
    }
    
    RetainPtr<const CPDF_Dictionary> pResources = pPage->GetPageResources();
    if (!pResources) {
        return nullptr;
    }
    
    RetainPtr<const CPDF_Dictionary> pFonts = pResources->GetDictFor("Font");
    if (!pFonts) {
        return nullptr;
    }
    
    // 2.2.4 Add boundary checks
    if (index < 0 || index >= static_cast<int>(pFonts->size())) {
        return nullptr;
    }
    
    // 2.2.2 Get the font object by index
    // Iterate through the dictionary to find the font at the specified index
    int currentIndex = 0;
    CPDF_DictionaryLocker locker(pFonts);
    for (const auto& it : locker) {
        if (currentIndex == index) {
            // Found the font at the requested index
            RetainPtr<CPDF_Object> pFontObj = it.second;
            if (!pFontObj) {
                return nullptr;
            }
            
            // Get the document to load the font
            CPDF_Document* pDoc = pPage->GetDocument();
            if (!pDoc) {
                return nullptr;
            }
            
            // 2.2.3 Return FPDF_FONT handle
            // Load the font from the document
            // If pFontObj is a reference, dereference it
            if (pFontObj->IsReference()) {
                pFontObj = pdfium::WrapRetain(const_cast<CPDF_Object*>(pFontObj->GetDirect().Get()));
            }
            
            if (!pFontObj || !pFontObj->IsDictionary()) {
                return nullptr;
            }
            
            // Get the CPDF_Font object from the document's page data
            // Cast PageDataIface to CPDF_DocPageData to access GetFont
            auto* pPageData = static_cast<CPDF_DocPageData*>(pDoc->GetPageData());
            if (!pPageData) {
                return nullptr;
            }
            
            // GetFont expects a mutable RetainPtr<CPDF_Dictionary>
            RetainPtr<CPDF_Dictionary> pFontDictMutable = 
                pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(pFontObj->AsDictionary()));
            
            RetainPtr<CPDF_Font> pFontPtr = pPageData->GetFont(pFontDictMutable);
            if (!pFontPtr) {
                return nullptr;
            }
            
            // Return as FPDF_FONT handle
            return FPDFFontFromCPDFFont(pFontPtr.Get());
        }
        currentIndex++;
    }
    
    // Index out of bounds (shouldn't reach here due to boundary check above)
    return nullptr;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDFFont_GetType(FPDF_FONT font) {
    // Get CPDF_Font object
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return FPDF_FONT_TYPE_UNKNOWN;
    }
    
    // Get font type using CPDF_Font public methods
    if (pFont->IsCIDFont()) {
        return FPDF_FONT_TYPE_TYPE0;
    } else if (pFont->IsType1Font()) {
        return FPDF_FONT_TYPE_TYPE1;
    } else if (pFont->IsTrueTypeFont()) {
        return FPDF_FONT_TYPE_TRUETYPE;
    } else if (pFont->IsType3Font()) {
        return FPDF_FONT_TYPE_TYPE3;
    } else {
        return FPDF_FONT_TYPE_UNKNOWN;
    }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_IsSubset(FPDF_FONT font) {
    // Get CPDF_Font object
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }
    
    // Get BaseFont name
    ByteString baseFontName = pFont->GetBaseFontName();
    if (baseFontName.IsEmpty()) {
        // Fallback: try to get from dictionary
        RetainPtr<const CPDF_Dictionary> pFontDict = pFont->GetFontDict();
        if (pFontDict) {
            baseFontName = pFontDict->GetByteStringFor("BaseFont");
        }
    }
    
    // Check if font is a subset
    // A font is typically a subset if the BaseFont name starts with a 6-character prefix followed by '+'
    // Example: "ABCDEF+FontName"
    if (baseFontName.GetLength() > 7 && baseFontName[6] == '+') {
        // Check if the first 6 characters are uppercase letters
        for (int i = 0; i < 6; i++) {
            char c = baseFontName[i];
            if (c < 'A' || c > 'Z') {
                return false;
            }
        }
        return true;
    }
    
    return false;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_HasToUnicode(FPDF_FONT font) {
    // Get CPDF_Font object
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }
    
    // Check if font has ToUnicode mapping
    RetainPtr<const CPDF_Dictionary> pFontDict = pFont->GetFontDict();
    if (pFontDict && pFontDict->KeyExist("ToUnicode")) {
        return true;
    }
    
    return false;
}

// ============================================================================
// Font Creation API
// ============================================================================

namespace {

// Helper function to generate subset prefix (6 uppercase letters)
std::string GenerateSubsetPrefix(const void* font_data, size_t data_size) {
    // Calculate a simple hash of the font data
    uint32_t hash = 0;
    const uint8_t* bytes = static_cast<const uint8_t*>(font_data);
    
    // Hash first 1KB of font data for performance
    size_t hash_size = std::min(data_size, size_t(1024));
    for (size_t i = 0; i < hash_size; i++) {
        hash = hash * 31 + bytes[i];
    }
    
    // Convert hash to 6 uppercase letters (A-Z)
    std::string prefix;
    prefix.reserve(6);
    
    for (int i = 0; i < 6; i++) {
        prefix += 'A' + (hash % 26);
        hash /= 26;
    }
    
    return prefix;
}

// Helper function to get GID for Unicode using CoreText (macOS)
#ifdef __APPLE__
#include <CoreText/CoreText.h>
#include <CoreFoundation/CoreFoundation.h>

std::map<uint16_t, uint16_t> GetUnicodeToGIDMap(
    const void* font_data,
    size_t data_size,
    const std::set<uint16_t>& unicode_chars) {
    
    std::map<uint16_t, uint16_t> unicode_to_gid;
    
    // Create CGDataProvider from font data
    CGDataProviderRef dataProvider = CGDataProviderCreateWithData(
        nullptr,
        font_data,
        data_size,
        nullptr
    );
    
    if (!dataProvider) {
        return unicode_to_gid;
    }
    
    // Create CGFont from data
    CGFontRef cgFont = CGFontCreateWithDataProvider(dataProvider);
    CGDataProviderRelease(dataProvider);
    
    if (!cgFont) {
        return unicode_to_gid;
    }
    
    // Create CTFont from CGFont
    CTFontRef ctFont = CTFontCreateWithGraphicsFont(cgFont, 12.0, nullptr, nullptr);
    CGFontRelease(cgFont);
    
    if (!ctFont) {
        return unicode_to_gid;
    }
    
    // Get GID for each Unicode character
    for (uint16_t unicode : unicode_chars) {
        UniChar unichar = unicode;
        CGGlyph glyph = 0;
        
        if (CTFontGetGlyphsForCharacters(ctFont, &unichar, &glyph, 1)) {
            unicode_to_gid[unicode] = static_cast<uint16_t>(glyph);
        }
    }
    
    CFRelease(ctFont);
    
    return unicode_to_gid;
}

// Helper function to get glyph advance widths using FreeType
std::map<uint16_t, int> GetGlyphAdvanceWidths(
    const void* font_data,
    size_t data_size,
    const std::map<uint16_t, uint16_t>& unicode_to_gid) {
    
    std::map<uint16_t, int> gid_to_width;
    
    // Initialize FreeType
    FT_Library library;
    if (FT_Init_FreeType(&library) != 0) {
        return gid_to_width;
    }
    
    // Load font from memory
    FT_Face face;
    FT_Error error = FT_New_Memory_Face(
        library,
        static_cast<const FT_Byte*>(font_data),
        static_cast<FT_Long>(data_size),
        0,
        &face
    );
    
    if (error != 0) {
        FT_Done_FreeType(library);
        return gid_to_width;
    }
    
    // Get units per EM for normalization
    int units_per_em = face->units_per_EM;
    if (units_per_em == 0) {
        units_per_em = 1000;  // Fallback
    }
    
    // Get advance width for each GID
    for (const auto& pair : unicode_to_gid) {
        uint16_t gid = pair.second;
        
        // Load glyph to get metrics
        error = FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE);
        if (error == 0) {
            // Get advance width in font units
            int advance = static_cast<int>(face->glyph->metrics.horiAdvance);
            
            // Normalize to 1000 units (PDF standard)
            int normalized_width = (advance * 1000) / units_per_em;
            
            gid_to_width[gid] = normalized_width;
        }
    }
    
    // Clean up
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    
    return gid_to_width;
}

#else
// Fallback for non-macOS platforms: assume GID = Unicode
std::map<uint16_t, uint16_t> GetUnicodeToGIDMap(
    const void* font_data,
    size_t data_size,
    const std::set<uint16_t>& unicode_chars) {
    
    std::map<uint16_t, uint16_t> unicode_to_gid;
    for (uint16_t unicode : unicode_chars) {
        unicode_to_gid[unicode] = unicode;
    }
    return unicode_to_gid;
}

// Fallback: return default width
std::map<uint16_t, int> GetGlyphAdvanceWidths(
    const void* font_data,
    size_t data_size,
    const std::map<uint16_t, uint16_t>& unicode_to_gid) {
    
    std::map<uint16_t, int> gid_to_width;
    for (const auto& pair : unicode_to_gid) {
        gid_to_width[pair.second] = 1000;  // Default width
    }
    return gid_to_width;
}
#endif

// Helper function to collect unique Unicode characters
std::set<uint16_t> CollectUnicodeChars(const unsigned short* unicode_chars, int char_count) {
    std::set<uint16_t> chars;
    for (int i = 0; i < char_count; i++) {
        chars.insert(unicode_chars[i]);
    }
    return chars;
}

// Helper function to generate ToUnicode CMap stream content
std::string GenerateToUnicodeCMap(const std::map<uint16_t, uint16_t>& unicode_to_gid) {
    std::ostringstream cmap;
    cmap << "/CIDInit /ProcSet findresource begin\n"
         << "12 dict begin\n"
         << "begincmap\n"
         << "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
         << "/CMapName /Adobe-Identity-UCS def\n"
         << "/CMapType 2 def\n"
         << "1 begincodespacerange\n"
         << "<0000> <FFFF>\n"
         << "endcodespacerange\n"
         << unicode_to_gid.size() << " beginbfchar\n";
    
    // Map GID → Unicode
    for (const auto& pair : unicode_to_gid) {
        uint16_t unicode = pair.first;
        uint16_t gid = pair.second;
        cmap << "<" << std::setw(4) << std::setfill('0') << std::hex << gid << "> <"
             << std::setw(4) << std::setfill('0') << std::hex << unicode << ">\n";
    }
    
    cmap << "endbfchar\n"
         << "endcmap\n"
         << "CMapName currentdict /CMap defineresource pop\n"
         << "end\n"
         << "end\n";
    
    return cmap.str();
}

// Helper function to generate CIDToGIDMap data
// In this implementation: CID = GID (we use GID directly as CID)
std::string GenerateCIDToGIDMap(const std::map<uint16_t, uint16_t>& unicode_to_gid) {
    // Find max GID to determine map size
    uint16_t max_gid = 0;
    for (const auto& pair : unicode_to_gid) {
        if (pair.second > max_gid) {
            max_gid = pair.second;
        }
    }
    
    std::string data;
    data.resize((max_gid + 1) * 2, '\0');
    
    // Identity mapping: CID = GID
    for (uint16_t gid = 0; gid <= max_gid; ++gid) {
        // Big-endian 16-bit encoding
        data[gid * 2] = (gid >> 8) & 0xFF;
        data[gid * 2 + 1] = gid & 0xFF;
    }
    
    return data;
}

} // namespace

FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFFont_CreateType0FromData(FPDF_DOCUMENT document,
                             const void* font_data,
                             unsigned long font_data_size,
                             const unsigned short* unicode_chars,
                             int char_count,
                             const char* font_name) {
    // Validate input parameters
    if (!document || !font_data || font_data_size == 0 || !unicode_chars || char_count <= 0) {
        return nullptr;
    }
    
    CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
    if (!pDoc) {
        return nullptr;
    }
    
    // 4.1.1 创建字体流对象（嵌入字体数据）
    // Create font file stream (FontFile2 for TrueType/OpenType)
    auto fontFileDict = pdfium::MakeRetain<CPDF_Dictionary>();
    auto fontFileStream = pDoc->NewIndirect<CPDF_Stream>(std::move(fontFileDict));
    
    // Embed font data into the stream
    ByteString fontBytes(static_cast<const char*>(font_data), font_data_size);
    fontFileStream->SetData(fontBytes.unsigned_span());
    
    // Generate subset prefix for font name (6 uppercase letters + "+")
    std::string subsetPrefix = GenerateSubsetPrefix(font_data, font_data_size);
    
    // Use provided font name or default to "CustomFont"
    std::string originalFontName = font_name ? font_name : "CustomFont";
    std::string baseFontName = subsetPrefix + "+" + originalFontName;
    
    // Collect Unicode characters into a set
    std::set<uint16_t> usedUnicodeSet = CollectUnicodeChars(unicode_chars, char_count);
    
    // 4.1.2 获取 Unicode → GID 映射
    // Get Unicode to GID mapping using CoreText
    std::map<uint16_t, uint16_t> unicode_to_gid = GetUnicodeToGIDMap(
        font_data, font_data_size, usedUnicodeSet
    );
    
    // 4.1.3 生成 ToUnicode CMap (GID → Unicode)
    // Generate ToUnicode CMap stream
    std::string cmapContent = GenerateToUnicodeCMap(unicode_to_gid);
    auto cmapDict = pdfium::MakeRetain<CPDF_Dictionary>();
    cmapDict->SetNewFor<CPDF_Name>("Type", "CMap");
    auto cmapStream = pDoc->NewIndirect<CPDF_Stream>(std::move(cmapDict));
    ByteString cmapBytes(cmapContent.c_str(), cmapContent.size());
    cmapStream->SetData(cmapBytes.unsigned_span());
    
    // 4.1.4 使用 Identity CIDToGIDMap
    // For Identity mapping (CID = GID), we can use the /Identity keyword
    // or omit CIDToGIDMap entirely for CIDFontType2
    // We'll use Identity to indicate CID = GID mapping
    
    // Create CIDSystemInfo dictionary
    auto cidSystemInfo = pDoc->NewIndirect<CPDF_Dictionary>();
    cidSystemInfo->SetNewFor<CPDF_String>("Registry", "Adobe");
    cidSystemInfo->SetNewFor<CPDF_String>("Ordering", "Identity");
    cidSystemInfo->SetNewFor<CPDF_Number>("Supplement", 0);
    
    // Create FontDescriptor dictionary
    auto fontDescriptor = pDoc->NewIndirect<CPDF_Dictionary>();
    fontDescriptor->SetNewFor<CPDF_Name>("Type", "FontDescriptor");
    fontDescriptor->SetNewFor<CPDF_Name>("FontName", baseFontName.c_str());
    fontDescriptor->SetNewFor<CPDF_Number>("Flags", 4);  // Symbolic font
    fontDescriptor->SetNewFor<CPDF_Number>("Ascent", 1000);
    fontDescriptor->SetNewFor<CPDF_Number>("Descent", -200);
    fontDescriptor->SetNewFor<CPDF_Number>("CapHeight", 1000);
    fontDescriptor->SetNewFor<CPDF_Number>("ItalicAngle", 0);
    fontDescriptor->SetNewFor<CPDF_Number>("StemV", 80);
    fontDescriptor->SetNewFor<CPDF_Number>("AvgWidth", 500);
    fontDescriptor->SetNewFor<CPDF_Number>("MaxWidth", 1000);
    fontDescriptor->SetNewFor<CPDF_Number>("MissingWidth", 500);
    
    // Set FontBBox
    auto fontBBox = pdfium::MakeRetain<CPDF_Array>();
    fontBBox->AppendNew<CPDF_Number>(0);
    fontBBox->AppendNew<CPDF_Number>(-200);
    fontBBox->AppendNew<CPDF_Number>(1000);
    fontBBox->AppendNew<CPDF_Number>(1000);
    fontDescriptor->SetFor("FontBBox", fontBBox);
    
    // Link FontFile2 (embedded font data)
    fontDescriptor->SetNewFor<CPDF_Reference>("FontFile2", pDoc, fontFileStream->GetObjNum());
    
    // Create CIDFont (descendant font) dictionary
    auto descendantFont = pDoc->NewIndirect<CPDF_Dictionary>();
    descendantFont->SetNewFor<CPDF_Name>("Type", "Font");
    descendantFont->SetNewFor<CPDF_Name>("Subtype", "CIDFontType2");
    descendantFont->SetNewFor<CPDF_Name>("BaseFont", baseFontName.c_str());
    descendantFont->SetNewFor<CPDF_Reference>("CIDSystemInfo", pDoc, cidSystemInfo->GetObjNum());
    descendantFont->SetNewFor<CPDF_Reference>("FontDescriptor", pDoc, fontDescriptor->GetObjNum());
    
    // Use /Identity for CIDToGIDMap (CID = GID)
    descendantFont->SetNewFor<CPDF_Name>("CIDToGIDMap", "Identity");
    
    // Get glyph advance widths using FreeType
    std::map<uint16_t, int> gid_to_width = GetGlyphAdvanceWidths(
        font_data, font_data_size, unicode_to_gid
    );
    
    // Build /W array with actual glyph widths
    // Format: [startGID [width1 width2 ...]]
    // We'll group consecutive GIDs for efficiency
    auto widthsArray = pdfium::MakeRetain<CPDF_Array>();
    
    if (!gid_to_width.empty()) {
        // Sort GIDs for grouping
        std::vector<uint16_t> sorted_gids;
        for (const auto& pair : gid_to_width) {
            sorted_gids.push_back(pair.first);
        }
        std::sort(sorted_gids.begin(), sorted_gids.end());
        
        // Group consecutive GIDs
        size_t i = 0;
        while (i < sorted_gids.size()) {
            uint16_t start_gid = sorted_gids[i];
            auto widthSubArray = pdfium::MakeRetain<CPDF_Array>();
            
            // Add consecutive GIDs to the same group
            size_t j = i;
            while (j < sorted_gids.size() && 
                   (j == i || sorted_gids[j] == sorted_gids[j-1] + 1)) {
                uint16_t gid = sorted_gids[j];
                int width = gid_to_width[gid];
                widthSubArray->AppendNew<CPDF_Number>(width);
                j++;
            }
            
            // Add [startGID [widths...]] to main array
            widthsArray->AppendNew<CPDF_Number>(start_gid);
            widthsArray->Append(widthSubArray);
            
            i = j;
        }
    } else {
        // Fallback: use default width for all glyphs
        uint16_t maxGID = 0;
        for (const auto& pair : unicode_to_gid) {
            if (pair.second > maxGID) {
                maxGID = pair.second;
            }
        }
        widthsArray->AppendNew<CPDF_Number>(0);
        widthsArray->AppendNew<CPDF_Number>(maxGID);
        widthsArray->AppendNew<CPDF_Number>(1000);  // Default width
    }
    
    descendantFont->SetFor("W", widthsArray);
    
    // 4.1.5 创建 Type0 字体字典
    // Create Type0 font dictionary
    auto type0Font = pDoc->NewIndirect<CPDF_Dictionary>();
    type0Font->SetNewFor<CPDF_Name>("Type", "Font");
    type0Font->SetNewFor<CPDF_Name>("Subtype", "Type0");
    type0Font->SetNewFor<CPDF_Name>("BaseFont", baseFontName.c_str());
    type0Font->SetNewFor<CPDF_Name>("Encoding", "Identity-H");
    type0Font->SetNewFor<CPDF_Reference>("ToUnicode", pDoc, cmapStream->GetObjNum());
    
    // Set DescendantFonts array
    auto descendants = pdfium::MakeRetain<CPDF_Array>();
    descendants->AppendNew<CPDF_Reference>(pDoc, descendantFont->GetObjNum());
    type0Font->SetFor("DescendantFonts", descendants);
    
    // 4.1.6 创建 CPDF_Font 对象
    // Create CPDF_Font object from the Type0 font dictionary
    RetainPtr<CPDF_Font> pFont = CPDF_Font::Create(pDoc, type0Font, nullptr);
    if (!pFont) {
        return nullptr;
    }
    
    // 4.1.7 添加内存管理和错误处理
    // Transfer ownership to caller using Leak()
    // Caller is responsible for calling FPDFFont_Close to release
    return FPDFFontFromCPDFFont(pFont.Leak());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFPage_AddFont(FPDF_PAGE page,
                 FPDF_FONT font,
                 char* font_name,
                 unsigned long* font_name_len) {
    // Validate input parameters
    if (!page || !font || !font_name_len) {
        return false;
    }
    
    CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
    if (!pPage) {
        return false;
    }
    
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }
    
    CPDF_Document* pDoc = pPage->GetDocument();
    if (!pDoc) {
        return false;
    }
    
    // Get or create page resources
    RetainPtr<CPDF_Dictionary> pResources = pPage->GetMutableResources();
    if (!pResources) {
        return false;
    }
    
    // Get or create Font dictionary
    RetainPtr<CPDF_Dictionary> pFonts = pResources->GetOrCreateDictFor("Font");
    if (!pFonts) {
        return false;
    }
    
    // 4.2.1 生成唯一的字体名称（F1, F2, ...）
    // Generate unique font name
    ByteString generatedName;
    int fontIndex = 1;
    do {
        generatedName = ByteString::Format("F%d", fontIndex++);
    } while (pFonts->KeyExist(generatedName.AsStringView()));
    
    // Calculate required buffer size (including null terminator)
    unsigned long required_len = generatedName.GetLength() + 1;

    // Save the caller-provided capacity before overwriting *font_name_len,
    // otherwise the too-small check below compares required_len < required_len
    // and never triggers, allowing memcpy to overflow the buffer.
    unsigned long capacity = *font_name_len;
    *font_name_len = required_len;

    // If buffer is NULL, this is a length query - return TRUE
    if (!font_name) {
        return true;
    }

    // If buffer is provided but too small, return FALSE
    if (capacity < required_len) {
        return false;
    }
    
    // 4.2.2 添加字体到页面资源字典
    // Get the font dictionary
    RetainPtr<const CPDF_Dictionary> pFontDict = pFont->GetFontDict();
    if (!pFontDict) {
        return false;
    }
    
    // Add font reference to page resources
    uint32_t fontObjNum = pFontDict->GetObjNum();
    pFonts->SetNewFor<CPDF_Reference>(generatedName.c_str(), pDoc, fontObjNum);
    
    // 4.2.3 返回分配的字体名称
    // Copy the font name to the buffer
    std::memcpy(font_name, generatedName.c_str(), required_len);
    
    return true;
}

// ============================================================================
// Text Object Font Operations API
// ============================================================================

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_SetFont(FPDF_PAGEOBJECT text_object, FPDF_FONT font) {
    // Validate input parameters
    if (!text_object || !font) {
        return false;
    }
    
    CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(text_object);
    if (!pPageObj || !pPageObj->IsText()) {
        return false;
    }
    
    CPDF_TextObject* pTextObj = pPageObj->AsText();
    if (!pTextObj) {
        return false;
    }
    
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }
    
    // 5.2.2 设置文本对象的字体
    // Set the font in the text object's text state
    RetainPtr<CPDF_Font> fontPtr(pFont);
    pTextObj->mutable_text_state().SetFont(fontPtr);
    
    // 5.2.3 更新文本对象状态
    pTextObj->SetDirty(true);
    
    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_SupportsUnicode(FPDF_FONT font, unsigned int unicode) {
    // 3.3.1 Get CPDF_Font object and call CharCodeFromUnicode
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }
    
    // Get the character code from Unicode
    uint32_t charcode = pFont->CharCodeFromUnicode(static_cast<wchar_t>(unicode));
    
    // If CharCodeFromUnicode returns 0, the character is not supported
    // (0 is typically used as a sentinel value for "not found")
    if (charcode == 0 && unicode != 0) {
        return false;
    }
    
    // 3.3.2 Check if glyph exists
    // Get the glyph index for the character code
    bool pVertGlyph = false;
    int glyph_index = pFont->GlyphFromCharCode(charcode, &pVertGlyph);
    
    // 3.3.3 Return support status
    // A glyph index > 0 indicates the glyph exists
    return (glyph_index > 0) ? true : false;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_IsStandard14(const char* base_font_name) {
    if (!base_font_name) {
        return false;
    }
    
    // Standard 14 fonts list
    static const char* standard14[] = {
        "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
        "Symbol",
        "ZapfDingbats"
    };
    
    // Handle subset prefix (e.g., "ABCDEF+Times-Roman")
    const char* fontName = base_font_name;
    const char* plusSign = strchr(base_font_name, '+');
    if (plusSign && plusSign > base_font_name) {
        // Check if prefix is 6 uppercase letters
        bool validPrefix = (plusSign - base_font_name) == 6;
        if (validPrefix) {
            for (int i = 0; i < 6; i++) {
                char c = base_font_name[i];
                if (c < 'A' || c > 'Z') {
                    validPrefix = false;
                    break;
                }
            }
        }
        
        // If valid subset prefix, skip it
        if (validPrefix) {
            fontName = plusSign + 1;
        }
    }
    
    // Check against Standard 14 list
    for (const char* standard_font : standard14) {
        if (strcmp(fontName, standard_font) == 0) {
            return true;
        }
    }
    
    return false;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_IsStandard14Font(FPDF_FONT font) {
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }
    
    return pFont->IsStandardFont() ? true : false;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_Standard14SupportsChar(const char* base_font_name,
                                unsigned int unicode) {
    if (!FPDFFont_IsStandard14(base_font_name)) {
        return false;
    }
    
    // Get clean font name (without subset prefix)
    const char* fontName = base_font_name;
    const char* plusSign = strchr(base_font_name, '+');
    if (plusSign && plusSign > base_font_name) {
        fontName = plusSign + 1;
    }
    
    // Symbol and ZapfDingbats have special character ranges
    if (strcmp(fontName, "Symbol") == 0) {
        // Symbol font supports mathematical symbols
        // Typically maps to Unicode range 0x0020-0x00FF
        return (unicode >= 0x0020 && unicode <= 0x00FF);
    }
    
    if (strcmp(fontName, "ZapfDingbats") == 0) {
        // ZapfDingbats supports decorative symbols
        // Typically maps to Unicode range 0x0020-0x00FF
        return (unicode >= 0x0020 && unicode <= 0x00FF);
    }
    
    // Other Standard 14 fonts support ASCII and Latin-1
    return (unicode >= 0x0020 && unicode <= 0x007E) ||  // ASCII printable
           (unicode >= 0x00A0 && unicode <= 0x00FF);    // Latin-1 supplement
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_GetCharWidth(FPDF_FONT font,
                      unsigned int charcode,
                      float font_size,
                      float* width) {
    if (!font || !width || font_size <= 0) {
        return false;
    }

    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return false;
    }

    // Direct glyph-width lookup — matches CPDF_Font::GetCharWidth(charcode)
    // used inside CalcPositionDataInternal.  No Unicode round-trip.
    int char_width = pFont->GetCharWidth(charcode);

    // GetCharWidthF returns width in 1/1000 em units; scale by font_size and
    // divide by 1000 to get the text-space advance.
    *width = (static_cast<float>(char_width) * font_size) / 1000.0f;

    return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFTextObj_GetCharWidth(FPDF_PAGEOBJECT text_object,
                         uint32_t charcode,
                         float* width) {
    // Validate input parameters
    if (!text_object || !width) {
        return false;
    }
    
    CPDF_PageObject* pPageObj = CPDFPageObjectFromFPDFPageObject(text_object);
    if (!pPageObj || !pPageObj->IsText()) {
        return false;
    }
    
    CPDF_TextObject* pTextObj = pPageObj->AsText();
    if (!pTextObj) {
        return false;
    }
    
    // Get character width from text object
    // This method handles all font types (Standard 14, embedded, etc.)
    // and returns the width already scaled by font size
    *width = pTextObj->GetCharWidth(charcode);
    
    return true;
}

FPDF_EXPORT FPDF_FONT FPDF_CALLCONV
FPDFFont_CreateStandard14(FPDF_DOCUMENT document,
                          const char* base_font_name) {
    CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
    if (!pDoc || !base_font_name) {
        return nullptr;
    }
    
    // Verify it's a Standard 14 font
    if (!FPDFFont_IsStandard14(base_font_name)) {
        return nullptr;
    }
    
    // Get clean font name (without subset prefix)
    const char* fontName = base_font_name;
    const char* plusSign = strchr(base_font_name, '+');
    if (plusSign && plusSign > base_font_name) {
        fontName = plusSign + 1;
    }
    
    // Create a simple Type1 font dictionary
    auto fontDict = pDoc->NewIndirect<CPDF_Dictionary>();
    fontDict->SetNewFor<CPDF_Name>("Type", "Font");
    fontDict->SetNewFor<CPDF_Name>("Subtype", "Type1");
    fontDict->SetNewFor<CPDF_Name>("BaseFont", fontName);
    
    // Create CPDF_Font object
    RetainPtr<CPDF_Font> pFont = CPDF_Font::Create(pDoc, fontDict, nullptr);
    if (!pFont) {
        return nullptr;
    }
    
    // Transfer ownership to caller
    return FPDFFontFromCPDFFont(pFont.Leak());
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFFont_GetResourceName(FPDF_FONT font,
                         char* buffer,
                         unsigned long* buflen) {
    // Validate input parameters
    CPDF_Font* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont || !buflen) {
        return false;
    }
    
    // Get the font dictionary
    RetainPtr<const CPDF_Dictionary> pFontDict = pFont->GetFontDict();
    if (!pFontDict) {
        return false;
    }
    
    // 3.2.1 Get font resource name from page resources dictionary
    // We need to search through all pages to find where this font is referenced
    
    // Get the document from the font
    CPDF_Document* pDoc = pFont->GetDocument();
    if (!pDoc) {
        return false;
    }
    
    // Get the font dictionary's object number
    uint32_t fontObjNum = pFontDict->GetObjNum();
    
    // Search through all pages to find the resource name
    ByteString resourceName;
    bool found = false;
    
    // Iterate through all pages in the document
    int pageCount = pDoc->GetPageCount();
    for (int i = 0; i < pageCount && !found; i++) {
        RetainPtr<CPDF_Dictionary> pPageDict = pDoc->GetMutablePageDictionary(i);
        if (!pPageDict) {
            continue;
        }
        
        // Get the page resources
        RetainPtr<CPDF_Dictionary> pResources = pPageDict->GetMutableDictFor("Resources");
        if (!pResources) {
            continue;
        }
        
        // Get the Font dictionary
        RetainPtr<CPDF_Dictionary> pFonts = pResources->GetMutableDictFor("Font");
        if (!pFonts) {
            continue;
        }
        
        // Search for the font in the Font dictionary
        CPDF_DictionaryLocker locker(pFonts);
        for (const auto& it : locker) {
            RetainPtr<CPDF_Object> pFontRef = it.second;
            if (!pFontRef) {
                continue;
            }
            
            // Check if this is a reference to our font
            uint32_t refObjNum = 0;
            if (pFontRef->IsReference()) {
                refObjNum = pFontRef->AsReference()->GetRefObjNum();
            } else if (pFontRef->IsDictionary()) {
                refObjNum = pFontRef->GetObjNum();
            }
            
            // If the object numbers match, we found the resource name
            if (refObjNum == fontObjNum) {
                resourceName = it.first;
                found = true;
                break;
            }
        }
    }
    
    // If not found in any page, return FALSE
    if (!found || resourceName.IsEmpty()) {
        return false;
    }
    
    // 3.2.2 Implement buffer length query mode
    // Calculate required buffer size (including null terminator)
    unsigned long required_len = resourceName.GetLength() + 1;

    // Save the caller-provided capacity before overwriting *buflen, otherwise
    // the too-small check below never triggers (see FPDFPage_AddFont).
    unsigned long capacity = *buflen;
    *buflen = required_len;

    // If buffer is NULL, this is a length query - return TRUE
    if (!buffer) {
        return true;
    }

    // If buffer is provided but too small, return FALSE
    if (capacity < required_len) {
        return false;
    }
    
    // Copy the resource name to the buffer
    std::memcpy(buffer, resourceName.c_str(), required_len);
    
    return true;
}

// ============================================================================
// Unicode → Charcode lookup
// ============================================================================

FPDF_EXPORT uint32_t FPDF_CALLCONV
FPDFFont_GetCharCode(FPDF_FONT font, uint32_t unicode) {
    auto* pFont = CPDFFontFromFPDFFont(font);
    if (!pFont) {
        return CPDF_Font::kInvalidCharCode;
    }
    uint32_t charcode = pFont->CharCodeFromUnicode(static_cast<wchar_t>(unicode));
    return charcode;
}
