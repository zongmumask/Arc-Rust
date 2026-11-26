// Arc PDF — PDF Standard Security extension
// Bridges Core C++ encryption primitives to a public C API.

#include "fpdf_security_ext.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "cpdf_creator.h"
#include "cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fxcrt/fx_stream.h"
#include "fpdfsdk/cpdfsdk_filewriteadapter.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

// PDF spec: reserved permission bits MUST be 1.
// Bits 0-2, 7-9, 14-15 are reserved.
constexpr uint32_t kReservedPermissionMask =
    0b111000001111111;  // bits 0-2, 7-9, 14-15 → OR mask

uint32_t NormalizePermissionFlags(uint32_t user_flags) {
  return user_flags | kReservedPermissionMask;
}

bool IsEmptyOrNull(const char* s) {
  return !s || s[0] == '\0';
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_SetEncryption(FPDF_DOCUMENT document,
                   FPDF_FILEWRITE* file_write,
                   const char* user_password,
                   const char* owner_password,
                   uint32_t permission_flags,
                   FPDF_BOOL encrypt_metadata) {
  // Validate inputs.
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || !file_write) {
    return 0;
  }
  if (IsEmptyOrNull(owner_password)) {
    return 0;  // Owner password is required.
  }

  CPDF_Parser* parser = doc->GetParser();

  // ----- Step 1: Build Standard Encrypt Dictionary (AES-256 / V=5 / R=6) -----
  auto encrypt_dict = pdfium::MakeRetain<CPDF_Dictionary>();

  encrypt_dict->SetNewFor<CPDF_Name>("Filter", "Standard");
  encrypt_dict->SetNewFor<CPDF_Number>("V", 5);
  encrypt_dict->SetNewFor<CPDF_Number>("R", 6);
  encrypt_dict->SetNewFor<CPDF_Number>("Length", 256);

  // CF dictionary — AESV3 crypt filter.
  auto std_cf = encrypt_dict->SetNewFor<CPDF_Dictionary>("CF");

  // StdCF is the named crypt filter under CF.
  auto std_cf_dict = std_cf->SetNewFor<CPDF_Dictionary>("StdCF");
  std_cf_dict->SetNewFor<CPDF_Name>("CFM", "AESV3");
  std_cf_dict->SetNewFor<CPDF_Name>("AuthEvent", "DocOpen");
  std_cf_dict->SetNewFor<CPDF_Number>("Length", 32);

  // Set StmF / StrF to reference the crypt filter.
  encrypt_dict->SetNewFor<CPDF_Name>("StmF", "StdCF");
  encrypt_dict->SetNewFor<CPDF_Name>("StrF", "StdCF");

  // Permission flags (with reserved bits forced to 1).
  uint32_t perms = NormalizePermissionFlags(permission_flags);
  encrypt_dict->SetNewFor<CPDF_Number>("P", static_cast<int>(perms));

  // EncryptMetadata (PDF 1.4 extension).
  encrypt_dict->SetNewFor<CPDF_Boolean>(
      "EncryptMetadata", static_cast<bool>(encrypt_metadata));

  // ----- Step 2: Create SecurityHandler and populate crypto fields -----
  auto handler = pdfium::MakeRetain<CPDF_SecurityHandler>();

  // OnCreate:
  //   - Randomly generates a 256-bit file encryption key (encrypt_key_).
  //   - Calls AES256_SetPassword → writes /U and /UE using user_password.
  //   - Calls AES256_SetPerms → writes /Perms (16-byte block with CRC, AES-encrypted).
  // Important: we pass the USER password here (so /U /UE unlock with user pwd).
  const ByteString user_pwd(user_password ? user_password : "");
  const ByteString owner_pwd(owner_password);

  handler->OnCreate(
      encrypt_dict.Get(),
      parser ? parser->GetIDArray().Get() : nullptr,
      user_pwd);

  // AES256_SetOwnerPassword: writes /O and /OE using the SAME encrypt_key_
  // (generated in OnCreate), so either password can decrypt the file key.
  handler->AES256_SetOwnerPassword(encrypt_dict.Get(), owner_pwd);

  // ----- Step 3: Save document with modified creator -----
  // Wrap FPDF_FILEWRITE in a RetainPtr stream (CPDF_Creator ctor accepts RetainPtr).
  auto archive = pdfium::MakeRetain<CPDFSDK_FileWriteAdapter>(file_write);

  // Use the Extensions copy of CPDF_Creator which accepts a security override.
  CPDF_Creator creator(doc, archive);
  creator.SetNewSecurityHandler(std::move(encrypt_dict), std::move(handler));

  // kNoOriginal → full rewrite (not incremental), forces new ID generation.
  return creator.Create(CPDF_Creator::CreateFlags::kNoOriginal, 7) ? 1 : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_RemoveEncryption(FPDF_DOCUMENT document, FPDF_FILEWRITE* file_write) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || !file_write) {
    return 0;
  }

  auto archive = pdfium::MakeRetain<CPDFSDK_FileWriteAdapter>(file_write);
  CPDF_Creator creator(doc, archive);
  creator.RemoveSecurity();
  return creator.Create(CPDF_Creator::CreateFlags::kNoOriginal, 7) ? 1 : 0;
}
