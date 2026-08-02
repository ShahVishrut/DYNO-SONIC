#include "sonic/crypto/cipher.hpp"

#include <algorithm>
#include <cstring>

#include <openssl/evp.h>

namespace sn::crypto {

namespace {
void reset_ctx(detail::evp_cipher_ctx& ctx) {
  detail::check_evp(EVP_CIPHER_CTX_reset(ctx.get()), "EVP_CIPHER_CTX_reset");
}

void ensure_equal(std::size_t lhs, std::size_t rhs, const char* label) {
  if (lhs != rhs) {
    throw error(label);
  }
}
}

void ctr_cipher::encrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> plaintext,
    sn::util::span<std::uint8_t> ciphertext
) const {
  ensure_equal(plaintext.size(), ciphertext.size(), "ctr encrypt size mismatch");

  reset_ctx(ctx_);
  detail::check_evp(
      EVP_EncryptInit_ex(ctx_.get(), EVP_aes_256_ctr(), nullptr, key.bytes.data(), nonce.bytes.data()),
      "EVP_EncryptInit_ex"
  );
  detail::check_evp(EVP_CIPHER_CTX_set_padding(ctx_.get(), 0), "EVP_CIPHER_CTX_set_padding");

  int produced = 0;
  detail::check_evp(
      EVP_EncryptUpdate(ctx_.get(), ciphertext.data(), &produced, plaintext.data(), static_cast<int>(plaintext.size())),
      "EVP_EncryptUpdate"
  );
  ensure_equal(static_cast<std::size_t>(produced), plaintext.size(), "ctr encrypt short output");

  int final_bytes = 0;
  detail::check_evp(EVP_EncryptFinal_ex(ctx_.get(), ciphertext.data() + produced, &final_bytes), "EVP_EncryptFinal_ex");
  ensure_equal(static_cast<std::size_t>(final_bytes), 0u, "ctr encrypt final bytes");
}

void ctr_cipher::decrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> ciphertext,
    sn::util::span<std::uint8_t> plaintext
) const {
  ensure_equal(ciphertext.size(), plaintext.size(), "ctr decrypt size mismatch");

  reset_ctx(ctx_);
  detail::check_evp(
      EVP_DecryptInit_ex(ctx_.get(), EVP_aes_256_ctr(), nullptr, key.bytes.data(), nonce.bytes.data()),
      "EVP_DecryptInit_ex"
  );
  detail::check_evp(EVP_CIPHER_CTX_set_padding(ctx_.get(), 0), "EVP_CIPHER_CTX_set_padding");

  int produced = 0;
  detail::check_evp(
      EVP_DecryptUpdate(
          ctx_.get(), plaintext.data(), &produced, ciphertext.data(), static_cast<int>(ciphertext.size())
      ),
      "EVP_DecryptUpdate"
  );
  ensure_equal(static_cast<std::size_t>(produced), ciphertext.size(), "ctr decrypt short output");

  int final_bytes = 0;
  detail::check_evp(EVP_DecryptFinal_ex(ctx_.get(), plaintext.data() + produced, &final_bytes), "EVP_DecryptFinal_ex");
  ensure_equal(static_cast<std::size_t>(final_bytes), 0u, "ctr decrypt final bytes");
}

ctr_cipher::key_type ctr_cipher::generate_key(prng& rng) {
  key_type key;
  rng.random_bytes(key.bytes.data(), key.bytes.size());
  return key;
}

ctr_cipher::nonce_type ctr_cipher::generate_nonce(prng& rng) {
  nonce_type nonce;
  rng.random_bytes(nonce.bytes.data(), 12);
  std::memset(nonce.bytes.data() + 12, 0, 4);
  return nonce;
}

void gcm_cipher::encrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> aad,
    sn::util::span<const std::uint8_t> plaintext, sn::util::span<std::uint8_t> ciphertext, tag_type& tag
) const {
  ensure_equal(plaintext.size(), ciphertext.size(), "gcm encrypt size mismatch");

  reset_ctx(ctx_);
  detail::check_evp(EVP_EncryptInit_ex(ctx_.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EVP_EncryptInit_ex");
  detail::check_evp(
      EVP_CIPHER_CTX_ctrl(ctx_.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_size), nullptr),
      "EVP_CIPHER_CTX_ctrl set ivlen"
  );
  detail::check_evp(
      EVP_EncryptInit_ex(ctx_.get(), nullptr, nullptr, key.bytes.data(), nonce.bytes.data()), "EVP_EncryptInit_ex key"
  );

  int produced = 0;
  if (!aad.empty()) {
    detail::check_evp(
        EVP_EncryptUpdate(ctx_.get(), nullptr, &produced, aad.data(), static_cast<int>(aad.size())),
        "EVP_EncryptUpdate aad"
    );
  }
  produced = 0;
  detail::check_evp(
      EVP_EncryptUpdate(ctx_.get(), ciphertext.data(), &produced, plaintext.data(), static_cast<int>(plaintext.size())),
      "EVP_EncryptUpdate"
  );
  ensure_equal(static_cast<std::size_t>(produced), plaintext.size(), "gcm encrypt short output");

  int final_bytes = 0;
  detail::check_evp(EVP_EncryptFinal_ex(ctx_.get(), ciphertext.data() + produced, &final_bytes), "EVP_EncryptFinal_ex");
  ensure_equal(static_cast<std::size_t>(final_bytes), 0u, "gcm encrypt final bytes");

  detail::check_evp(
      EVP_CIPHER_CTX_ctrl(ctx_.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag_size), tag.bytes.data()),
      "EVP_CIPHER_CTX_ctrl get tag"
  );
}

bool gcm_cipher::decrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> aad,
    sn::util::span<const std::uint8_t> ciphertext, sn::util::span<std::uint8_t> plaintext, const tag_type& tag
) const {
  ensure_equal(ciphertext.size(), plaintext.size(), "gcm decrypt size mismatch");

  reset_ctx(ctx_);
  detail::check_evp(EVP_DecryptInit_ex(ctx_.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EVP_DecryptInit_ex");
  detail::check_evp(
      EVP_CIPHER_CTX_ctrl(ctx_.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_size), nullptr),
      "EVP_CIPHER_CTX_ctrl set ivlen"
  );
  detail::check_evp(
      EVP_DecryptInit_ex(ctx_.get(), nullptr, nullptr, key.bytes.data(), nonce.bytes.data()), "EVP_DecryptInit_ex key"
  );

  int produced = 0;
  if (!aad.empty()) {
    detail::check_evp(
        EVP_DecryptUpdate(ctx_.get(), nullptr, &produced, aad.data(), static_cast<int>(aad.size())),
        "EVP_DecryptUpdate aad"
    );
  }
  produced = 0;
  detail::check_evp(
      EVP_DecryptUpdate(
          ctx_.get(), plaintext.data(), &produced, ciphertext.data(), static_cast<int>(ciphertext.size())
      ),
      "EVP_DecryptUpdate"
  );
  ensure_equal(static_cast<std::size_t>(produced), ciphertext.size(), "gcm decrypt short output");

  detail::check_evp(
      EVP_CIPHER_CTX_ctrl(
          ctx_.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_size), const_cast<std::uint8_t*>(tag.bytes.data())
      ),
      "EVP_CIPHER_CTX_ctrl set tag"
  );

  int final_bytes = 0;
  const int ok = EVP_DecryptFinal_ex(ctx_.get(), plaintext.data() + produced, &final_bytes);
  if (ok != 1 || final_bytes != 0) {
    return false;
  }
  return true;
}

gcm_cipher::key_type gcm_cipher::generate_key(prng& rng) {
  key_type key;
  rng.random_bytes(key.bytes.data(), key.bytes.size());
  return key;
}

gcm_cipher::nonce_type gcm_cipher::generate_nonce(prng& rng) {
  nonce_type nonce;
  rng.random_bytes(nonce.bytes.data(), nonce.bytes.size());
  return nonce;
}

}
