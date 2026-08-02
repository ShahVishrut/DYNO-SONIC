#ifndef DYNO_UTILS_CRYPTO_H
#define DYNO_UTILS_CRYPTO_H

#include <array>
#include <cassert>
#include <cstdint>
#include <cstddef>

#include <openssl/aes.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace dyno::crypto {

const auto kDigest = EVP_sha256;
const auto kCipher = EVP_aes_256_cbc;
const unsigned int kKeySize = 32;
static const int kDigestSize = 32; // EVP_MD_size(kDigest());
static const unsigned int kBlockSize = AES_BLOCK_SIZE;
static const unsigned int kIvSize = AES_BLOCK_SIZE;

using Key = std::array<uint8_t, kKeySize>;
using Iv = std::array<uint8_t, kIvSize>;

template<size_t n>
inline std::array<uint8_t, n> GenRandBytes() {
  std::array<uint8_t, n> res;
  RAND_bytes(res.data(), n);
  return res;
}

inline auto GenerateKey = GenRandBytes<sizeof(Key)>;
inline auto GenerateIv = GenRandBytes<sizeof(Iv)>;

inline bool Hash(const uint8_t *val, const size_t val_len, uint8_t *res) {
  EVP_MD_CTX *ctx;

  if ((ctx = EVP_MD_CTX_new()) == nullptr) {
    ERR_print_errors_fp(stderr);
    return false;
  }

  if (EVP_DigestInit_ex(ctx, kDigest(), nullptr) != 1) {
    ERR_print_errors_fp(stderr);
    return false;
  }

  if (EVP_DigestUpdate(ctx, val, val_len) != 1) {
    ERR_print_errors_fp(stderr);
    return false;
  }

  unsigned int res_len = 0;
  if (EVP_DigestFinal_ex(ctx, res, &res_len) != 1) {
    ERR_print_errors_fp(stderr);
    return false;
  }
  assert(res_len == kDigestSize);

  EVP_MD_CTX_free(ctx);
  return true;
}

constexpr inline size_t CiphertextLen(size_t plaintext_len) {
  return (((plaintext_len + kBlockSize) / kBlockSize) * kBlockSize) + kIvSize;
}

// Chooses a random IV, and returns the ciphertext with the IV appended to the end of it.
inline bool Encrypt(const uint8_t *val, const size_t val_len,
                    const Key key, uint8_t *res) {
  EVP_CIPHER_CTX *ctx;
  Iv iv = GenerateIv();
  Iv iv_copy = Iv(iv); // Because OpenSSL may mess with the IV passed to it.

  if (!(ctx = EVP_CIPHER_CTX_new())) {
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  if (1 != EVP_EncryptInit_ex(ctx, kCipher(), nullptr,
                              key.data(), iv_copy.data())) {
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  int len;
  size_t done = 0;
  size_t res_offset = 0;
  while (done < val_len) {
    size_t to_encrypt = val_len - done;
    if (to_encrypt > INT_MAX) {
      to_encrypt = INT_MAX - (1UL << 10);
    }

    if (1 != EVP_EncryptUpdate(
        ctx, res + res_offset, &len, val + done, to_encrypt)) {
      ERR_print_errors_fp(stderr);
      EVP_CIPHER_CTX_free(ctx);
      return false;
    }

    done += to_encrypt;
    res_offset += len;
  }

  if (1 != EVP_EncryptFinal_ex(ctx, res + res_offset, &len)) {
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  res_offset += len;
  assert(res_offset == (CiphertextLen(val_len) - kIvSize));

  // Append IV to the end of the ciphertext.
  std::copy(iv.begin(), iv.end(), res + res_offset);

  EVP_CIPHER_CTX_free(ctx);
  return true;
}

// Assumes the last bytes of val are the IV.
// Returns plaintext len.
inline size_t Decrypt(const uint8_t *val, const size_t val_len,
                      const Key key, uint8_t *res) {
  EVP_CIPHER_CTX *ctx;
  size_t clen = val_len - kIvSize;

  if (!(ctx = EVP_CIPHER_CTX_new())) {
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return 0;
  }

  if (1 != EVP_DecryptInit_ex(
      ctx, kCipher(), nullptr, key.data(), val + clen)) {
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return 0;
  }

  int len;
  size_t done = 0;
  size_t res_offset = 0;
  while (done < clen) {
    size_t to_decrypt = clen - done;
    if (to_decrypt > INT_MAX) {
      to_decrypt = INT_MAX - (1UL << 10);
    }
    if (1 != EVP_DecryptUpdate(
        ctx, res + res_offset, &len, val + done, to_decrypt)) {
      ERR_print_errors_fp(stderr);
      EVP_CIPHER_CTX_free(ctx);
      return 0;
    }

    done += to_decrypt;
    res_offset += len;
  }

  if (1 != EVP_DecryptFinal_ex(ctx, res + res_offset, &len)) {
    ERR_print_errors_fp(stderr);
    EVP_CIPHER_CTX_free(ctx);
    return 0;
  }
  res_offset += len;

  EVP_CIPHER_CTX_free(ctx);

  assert(CiphertextLen(res_offset) == val_len);
  return res_offset;
}
} // namespace dyno::crypto

#endif //DYNO_UTILS_CRYPTO_H
