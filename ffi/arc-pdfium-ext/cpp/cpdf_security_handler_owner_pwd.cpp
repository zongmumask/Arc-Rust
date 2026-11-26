// Arc PDF — AES256_SetOwnerPassword standalone implementation.
//
// This file exists because we cannot recompile the full Core
// cpdf_security_handler.cpp: the Core build relies on `extern template`
// suppressing implicit StringViewTemplate<char> instantiation, with the
// sole explicit instantiation living in bytestring.cpp (inside the
// vendored Pdfium.xcframework). A standalone recompile of
// cpdf_security_handler.cpp fails at link time with unresolved
// StringViewTemplate symbols.
//
// Strategy: keep the modified header (cpdf_security_handler.h) with the
// new public method declaration, but provide ONLY this one new method in a
// tiny .cpp that avoids ByteStringView(pdfium::span<...>) — instead using
// ByteString(const uint8_t*, size_t) which does NOT require the suppressed
// template instantiation.

#include "cpdf_security_handler.h"

#include <stdint.h>

#include <array>
#include <optional>

#include "core/fdrm/fx_crypt.h"
#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fxcrt/byteorder.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/span_util.h"
#include "core/fxcrt/stl_util.h"

namespace {

// Mirrors Revision6_Hash from Core cpdf_security_handler.cpp — but since
// that function is in an anonymous namespace in the Core .cpp, we cannot
// call it. We reimplement here using the same algorithm.
//
// PDF 1.7 ExtensionLevel 3, Algorithm 3.1: Compute the hash for revision 6
// of the Standard Security handler.
void Revision6_Hash(
    const ByteString& password,
    pdfium::span<const uint8_t, 8> salt,
    std::optional<pdfium::span<const uint8_t, 48>> vector,
    pdfium::span<uint8_t, 32> hash) {
  CryptSha2Context sha;
  CryptSha256Start(&sha);
  CryptSha256Update(&sha, password.unsigned_span());
  CryptSha256Update(&sha, salt);
  if (vector.has_value()) {
    CryptSha256Update(&sha, vector.value());
  }

  std::array<uint8_t, 32> digest;
  CryptSha256Finish(&sha, digest);

  // BigOrder64BitsMod3 — compute (val mod 3) where val is a big-endian
  // 64-bit integer built from 4 bytes.
  auto BigOrder64BitsMod3 = [](pdfium::span<const uint8_t> data) -> int {
    uint64_t ret = 0;
    for (int i = 0; i < 4; ++i) {
      ret <<= 32;
      ret |= fxcrt::GetUInt32MSBFirst(data.first<4u>());
      ret %= 3;
      data = data.subspan<4u>();
    }
    return static_cast<int>(ret);
  };

  DataVector<uint8_t> encrypted_output;
  DataVector<uint8_t> inter_digest;
  pdfium::span<uint8_t> input(digest);
  auto key = input.first<16u>();
  auto iv = input.subspan<16u, 16u>();
  int i = 0;
  size_t block_size = 32;
  CRYPT_aes_context aes = {};
  do {
    size_t round_size = password.GetLength() + block_size;
    if (vector.has_value()) {
      round_size += vector.value().size();
    }
    encrypted_output.resize(round_size * 64);
    auto encrypted_output_span = pdfium::span(encrypted_output);
    auto password_span = password.unsigned_span();
    auto input_span = pdfium::span(input).first(block_size);
    DataVector<uint8_t> content;
    for (int j = 0; j < 64; ++j) {
      content.insert(std::end(content), std::begin(password_span),
                     std::end(password_span));
      content.insert(std::end(content), std::begin(input_span),
                     std::end(input_span));
      if (vector.has_value()) {
        content.insert(std::end(content), std::begin(vector.value()),
                       std::end(vector.value()));
      }
    }
    CHECK_EQ(content.size(), encrypted_output.size());
    CRYPT_AESSetKey(&aes, key);
    CRYPT_AESSetIV(&aes, iv);
    CRYPT_AESEncrypt(&aes, encrypted_output_span, content);

    switch (BigOrder64BitsMod3(encrypted_output_span)) {
      case 0:
        block_size = 32;
        inter_digest = CryptSha256Generate(encrypted_output_span);
        break;
      case 1:
        block_size = 48;
        inter_digest = CryptSha384Generate(encrypted_output_span);
        break;
      default:
        block_size = 64;
        inter_digest = CryptSha512Generate(encrypted_output_span);
        break;
    }
    input = inter_digest;
    key = input.first<16u>();
    iv = input.subspan<16u, 16u>();
    ++i;
  } while (i < 64 || i - 32 < encrypted_output.back());

  fxcrt::Copy(input.first(32u), hash);
}

}  // namespace

// [Arc PDF] Write /O and /OE fields using owner password.
//
// Algorithm: identical to Core's AES256_SetPassword, but writes "O" / "OE"
// instead of "U" / "UE". Must be called AFTER OnCreate() which sets
// encrypt_key_ and revision_.
//
// NOTE: We use ByteString(const uint8_t*, size_t) instead of
// ByteString(ByteStringView(pdfium::span(...))) to avoid triggering the
// suppressed StringViewTemplate<char> explicit template instantiation.
void CPDF_SecurityHandler::AES256_SetOwnerPassword(
    CPDF_Dictionary* pEncryptDict,
    const ByteString& owner_password) {
  CryptSha1Context sha;
  CryptSha1Start(&sha);
  CryptSha1Update(&sha, encrypt_key_);
  CryptSha1Update(&sha, pdfium::as_byte_span("hello").first<5u>());

  uint8_t digest[20];
  CryptSha1Finish(&sha, digest);

  CryptSha2Context sha2;
  uint8_t digest1[48];
  if (revision_ >= 6) {
    Revision6_Hash(owner_password, pdfium::span(digest).first<8u>(),
                   std::nullopt, pdfium::span(digest1).first<32u>());
  } else {
    CryptSha256Start(&sha2);
    CryptSha256Update(&sha2, owner_password.unsigned_span());
    CryptSha256Update(&sha2, pdfium::span(digest).first<8u>());
    CryptSha256Finish(&sha2, pdfium::span(digest1).first<32u>());
  }
  fxcrt::Copy(pdfium::span(digest).first<16u>(),
              pdfium::span(digest1).subspan<32u>());

  // Write "O" — 48 bytes (32-byte hash + 16-byte copy of SHA1 prefix).
  // Use raw pointer + length to bypass ByteStringView path.
  pEncryptDict->SetNewFor<CPDF_String>(
      "O", ByteString(digest1, sizeof(digest1)));

  if (revision_ >= 6) {
    Revision6_Hash(owner_password, pdfium::span(digest).subspan<8u, 8u>(),
                   std::nullopt, pdfium::span(digest1).first<32u>());
  } else {
    CryptSha256Start(&sha2);
    CryptSha256Update(&sha2, owner_password.unsigned_span());
    CryptSha256Update(&sha2, pdfium::span(digest).subspan<8, 8>());
    CryptSha256Finish(&sha2, pdfium::span(digest1).first<32>());
  }
  CRYPT_aes_context aes = {};
  CRYPT_AESSetKey(&aes, pdfium::span(digest1).first<32>());
  uint8_t iv[16] = {};
  CRYPT_AESSetIV(&aes, iv);
  CRYPT_AESEncrypt(&aes, digest1, encrypt_key_);

  // Write "OE" — 32 bytes (AES-encrypted file encryption key).
  pEncryptDict->SetNewFor<CPDF_String>(
      "OE", ByteString(digest1, 32));

  // Core `OnCreate`(R>=5) 在写入 /U /UE /Perms 后直接 return，从不创建
  // `crypto_handler_`——若无它，CPDF_Creator::GetCryptoHandler() 返回 null，
  // 导致流和字符串被明文写出（阅读器按密文解密 → 内容空白）。这里补上，
  // 使注入 Creator 前 handler 已具备可加密的 AES 处理器。
  InitCryptoHandler();
}
