#ifndef FPDF_SECURITY_EXT_H_
#define FPDF_SECURITY_EXT_H_

#include <stdint.h>

#include "fpdf_save.h"
#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// AES-256 Standard Security revision (V=5, R=6)
// PDF 1.7 ExtensionLevel 3+
#define FPDF_ENCRYPT_AES_256  0

// Encryption bits
#define FPDF_ENCRYPT_BITS_128  128
#define FPDF_ENCRYPT_BITS_256  256

// Permission bit flags (PDF 1.7 Table 3.20 — "Document Encryption Permissions")
// Combine with bitwise OR. Missing bits (0-2, 7-9, 14-15) MUST be set to 1.
#define FPDF_PERM_UNKNOWN            ((uint32_t)-33)  // -33 in signed = 0xFFFFFFDF → 0b...11011111

// Bit 3: Print document (low quality)
#define FPDF_PERM_PRINT_LOW          (1u << 3)
// Bit 4: Modify document contents (assemble, insert/delete pages)
#define FPDF_PERM_MODIFY             (1u << 4)
// Bit 5: Extract text and graphics
#define FPDF_PERM_EXTRACT            (1u << 5)
// Bit 6: Add or modify annotations, fill form fields
#define FPDF_PERM_ANNOTATE           (1u << 6)
// Bit 10: Print document (high quality)
#define FPDF_PERM_PRINT_HIGH         (1u << 10)
// Bit 11: Accessibility — extract text for screen readers
#define FPDF_PERM_ACCESSIBILITY      (1u << 11)
// Bit 12: Document assembly — insert/delete/rotate pages, create bookmarks
#define FPDF_PERM_ASSEMBLE           (1u << 12)
// Bit 13: Fill form fields
#define FPDF_PERM_FORM_FILL          (1u << 13)

// Set encryption (Standard Security, AES-256) on a document.
//
// Parameters:
//   document           — Handle to the document (must be successfully loaded)
//   file_write         — Output destination
//   user_password      — User password (may be NULL/empty for "open without password")
//   owner_password     — Owner password (must NOT be empty; must differ from user_password)
//   permission_flags   — Bitwise OR of FPDF_PERM_* constants. Reserved bits (0-2, 7-9, 14-15)
//                        are forced to 1 internally.
//   encrypt_metadata   — If TRUE, metadata stream is also encrypted.
//
// Returns:
//   TRUE on success, FALSE on failure.
//
// Encryption algorithm: PDF 1.7 ExtensionLevel 3+ Standard Security, V=5, R=6, AES-256.
// Produces an Encrypt dictionary compatible with qpdf, PDF Reader Pro, Adobe Acrobat.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_SetEncryption(FPDF_DOCUMENT document,
                   FPDF_FILEWRITE* file_write,
                   const char* user_password,
                   const char* owner_password,
                   uint32_t permission_flags,
                   FPDF_BOOL encrypt_metadata);

// Remove Standard Security from a document (save without encryption).
//
// Parameters:
//   document         — Handle to the document (must have been opened with correct owner password)
//   file_write       — Output destination
//
// Returns:
//   TRUE on success, FALSE on failure.
//
// Equivalent to FPDF_SaveAsCopy(doc, file_write, FPDF_REMOVE_SECURITY), provided as a
// convenience API that returns a clearer semantic intent.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_RemoveEncryption(FPDF_DOCUMENT document, FPDF_FILEWRITE* file_write);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_SECURITY_EXT_H_
