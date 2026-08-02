#include "sonic/crypto/cipher.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sonic/crypto/impl/bearssl/detail/backend.hpp"
#include "sonic/crypto/impl/bearssl/detail/utils.hpp"
#include "sonic/util/memcpy.hpp"

namespace sn::crypto {

namespace {

void ensure_sizes_equal(std::size_t lhs, std::size_t rhs, const char* label) {
  if (lhs != rhs) {
    throw error(label);
  }
}

}

void ctr_cipher::encrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> plaintext,
    sn::util::span<std::uint8_t> ciphertext
) const {
  ensure_sizes_equal(plaintext.size(), ciphertext.size(), "ctr encrypt size mismatch");

  constexpr std::size_t counter_offset = nonce_size - sizeof(std::uint32_t);
  const auto* iv = nonce.bytes.data();
  const std::uint32_t counter = detail::load_be32(iv + counter_offset);

  if (!plaintext.empty()) {
    sn::mem::copy_bytes(ciphertext.data(), plaintext.data(), plaintext.size());
  }

  detail::bearssl_ctr_init(&ctx_.keys, key.bytes.data(), key.bytes.size());
  (void) detail::bearssl_ctr_run(&ctx_.keys, iv, counter, ciphertext.data(), ciphertext.size());
}

void ctr_cipher::decrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> ciphertext,
    sn::util::span<std::uint8_t> plaintext
) const {
  ensure_sizes_equal(ciphertext.size(), plaintext.size(), "ctr decrypt size mismatch");

  constexpr std::size_t counter_offset = nonce_size - sizeof(std::uint32_t);
  const auto* iv = nonce.bytes.data();
  const std::uint32_t counter = detail::load_be32(iv + counter_offset);

  if (!ciphertext.empty()) {
    sn::mem::copy_bytes(plaintext.data(), ciphertext.data(), ciphertext.size());
  }

  detail::bearssl_ctr_init(&ctx_.keys, key.bytes.data(), key.bytes.size());
  (void) detail::bearssl_ctr_run(&ctx_.keys, iv, counter, plaintext.data(), plaintext.size());
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
  ensure_sizes_equal(plaintext.size(), ciphertext.size(), "gcm encrypt size mismatch");

  if (!plaintext.empty()) {
    sn::mem::copy_bytes(ciphertext.data(), plaintext.data(), plaintext.size());
  }

  detail::bearssl_ctr_init(&ctx_.ctr_keys, key.bytes.data(), key.bytes.size());
  const auto ctr_vtable = detail::bearssl_ctr_vtable(ctx_.ctr_keys);
  br_gcm_init(&ctx_.gcm, ctr_vtable, detail::bearssl_select_ghash());
  br_gcm_reset(&ctx_.gcm, nonce.bytes.data(), nonce.bytes.size());
  if (!aad.empty()) {
    br_gcm_aad_inject(&ctx_.gcm, aad.data(), aad.size());
  }
  br_gcm_flip(&ctx_.gcm);
  if (!ciphertext.empty()) {
    br_gcm_run(&ctx_.gcm, 1, ciphertext.data(), ciphertext.size());
  }
  br_gcm_get_tag(&ctx_.gcm, tag.bytes.data());
}

bool gcm_cipher::decrypt(
    const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> aad,
    sn::util::span<const std::uint8_t> ciphertext, sn::util::span<std::uint8_t> plaintext, const tag_type& tag
) const {
  ensure_sizes_equal(ciphertext.size(), plaintext.size(), "gcm decrypt size mismatch");

  if (!ciphertext.empty()) {
    sn::mem::copy_bytes(plaintext.data(), ciphertext.data(), ciphertext.size());
  }

  detail::bearssl_ctr_init(&ctx_.ctr_keys, key.bytes.data(), key.bytes.size());
  const auto ctr_vtable = detail::bearssl_ctr_vtable(ctx_.ctr_keys);
  br_gcm_init(&ctx_.gcm, ctr_vtable, detail::bearssl_select_ghash());
  br_gcm_reset(&ctx_.gcm, nonce.bytes.data(), nonce.bytes.size());
  if (!aad.empty()) {
    br_gcm_aad_inject(&ctx_.gcm, aad.data(), aad.size());
  }
  br_gcm_flip(&ctx_.gcm);
  if (!plaintext.empty()) {
    br_gcm_run(&ctx_.gcm, 0, plaintext.data(), plaintext.size());
  }
  return br_gcm_check_tag(&ctx_.gcm, tag.bytes.data()) == 1;
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
