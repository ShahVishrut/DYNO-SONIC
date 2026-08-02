#pragma once

#include <array>
#include <cstring>
#include <type_traits>

#include "sonic/crypto/cipher.hpp"
#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/crypto/prng.hpp"

namespace sn::crypto {

template <typename T> struct ctr_box {
  typename ctr_cipher::nonce_type nonce{};
  std::array<std::uint8_t, sizeof(T)> body{};
};

template <typename T> struct gcm_box {
  typename gcm_cipher::nonce_type nonce{};
  typename gcm_cipher::tag_type tag{};
  std::array<std::uint8_t, sizeof(T)> body{};
};

namespace detail {

template <typename T> inline void memcpy_out(std::uint8_t* dst, const T& value) {
  static_assert(std::is_trivially_copyable<T>::value, "struct encryption requires trivially copyable type");
  std::memcpy(dst, &value, sizeof(T));
}

template <typename T> inline void memcpy_in(T& dst, const std::uint8_t* src) {
  static_assert(std::is_trivially_copyable<T>::value, "struct encryption requires trivially copyable type");
  std::memcpy(&dst, src, sizeof(T));
}

}

template <typename T>
void encrypt_struct(
    const ctr_cipher& cipher, prng& rng, const typename ctr_cipher::key_type& key, const T& input, ctr_box<T>& out
) {
  typename ctr_cipher::nonce_type nonce = ctr_cipher::generate_nonce(rng);
  out.nonce = nonce;

  std::array<std::uint8_t, sizeof(T)> buffer{};
  detail::memcpy_out(buffer.data(), input);

  sn::util::span<const std::uint8_t> plaintext(buffer.data(), buffer.size());
  sn::util::span<std::uint8_t> ciphertext(out.body.data(), out.body.size());
  cipher.encrypt(key, out.nonce, plaintext, ciphertext);
  detail::secure_zero(buffer.data(), buffer.size());
}

template <typename T>
void decrypt_struct(
    const ctr_cipher& cipher, const typename ctr_cipher::key_type& key, const ctr_box<T>& box, T& output
) {
  sn::util::span<const std::uint8_t> ciphertext(box.body.data(), box.body.size());
  std::array<std::uint8_t, sizeof(T)> buffer{};
  sn::util::span<std::uint8_t> plaintext(buffer.data(), buffer.size());
  cipher.decrypt(key, box.nonce, ciphertext, plaintext);
  detail::memcpy_in(output, buffer.data());
  detail::secure_zero(buffer.data(), buffer.size());
}

template <typename T>
void encrypt_struct(
    const gcm_cipher& cipher, prng& rng, const typename gcm_cipher::key_type& key, const T& input, gcm_box<T>& out
) {
  typename gcm_cipher::nonce_type nonce = gcm_cipher::generate_nonce(rng);
  out.nonce = nonce;

  std::array<std::uint8_t, sizeof(T)> buffer{};
  detail::memcpy_out(buffer.data(), input);

  sn::util::span<const std::uint8_t> plaintext(buffer.data(), buffer.size());
  sn::util::span<std::uint8_t> ciphertext(out.body.data(), out.body.size());
  cipher.encrypt(key, out.nonce, plaintext, ciphertext, out.tag);
  detail::secure_zero(buffer.data(), buffer.size());
}

template <typename T>
bool decrypt_struct(
    const gcm_cipher& cipher, const typename gcm_cipher::key_type& key, const gcm_box<T>& box, T& output
) {
  sn::util::span<const std::uint8_t> ciphertext(box.body.data(), box.body.size());
  std::array<std::uint8_t, sizeof(T)> buffer{};
  sn::util::span<std::uint8_t> plaintext(buffer.data(), buffer.size());
  const bool ok = cipher.decrypt(key, box.nonce, ciphertext, plaintext, box.tag);
  if (!ok) {
    detail::secure_zero(buffer.data(), buffer.size());
    return false;
  }
  detail::memcpy_in(output, buffer.data());
  detail::secure_zero(buffer.data(), buffer.size());
  return true;
}

}
