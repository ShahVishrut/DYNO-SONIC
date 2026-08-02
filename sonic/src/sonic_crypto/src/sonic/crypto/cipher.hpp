#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

class ctr_cipher {
public:
  static constexpr std::size_t key_size = 32;
  static constexpr std::size_t nonce_size = 16;

  struct key_type {
    std::array<std::uint8_t, key_size> bytes{};
  };

  struct nonce_type {
    std::array<std::uint8_t, nonce_size> bytes{};
  };

  ctr_cipher() = default;
  ctr_cipher(const ctr_cipher&) = delete;
  ctr_cipher& operator=(const ctr_cipher&) = delete;
  ctr_cipher(ctr_cipher&&) = default;
  ctr_cipher& operator=(ctr_cipher&&) = default;

  void encrypt(
      const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> plaintext,
      sn::util::span<std::uint8_t> ciphertext
  ) const;

  void decrypt(
      const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> ciphertext,
      sn::util::span<std::uint8_t> plaintext
  ) const;

  static key_type generate_key(prng& rng);
  static nonce_type generate_nonce(prng& rng);

private:
  using ctr_state_type =
#if defined(SONIC_CRYPTO_BACKEND_BEARSSL)
      detail::bearssl_ctr_cipher_state
#else
      detail::evp_cipher_ctx
#endif
      ;

  mutable ctr_state_type ctx_{};
};

class gcm_cipher {
public:
  static constexpr std::size_t key_size = 32;
  static constexpr std::size_t nonce_size = 12;
  static constexpr std::size_t tag_size = 16;

  struct key_type {
    std::array<std::uint8_t, key_size> bytes{};
  };

  struct nonce_type {
    std::array<std::uint8_t, nonce_size> bytes{};
  };

  struct tag_type {
    std::array<std::uint8_t, tag_size> bytes{};
  };

  gcm_cipher() = default;
  gcm_cipher(const gcm_cipher&) = delete;
  gcm_cipher& operator=(const gcm_cipher&) = delete;
  gcm_cipher(gcm_cipher&&) = default;
  gcm_cipher& operator=(gcm_cipher&&) = default;

  void encrypt(
      const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> aad,
      sn::util::span<const std::uint8_t> plaintext, sn::util::span<std::uint8_t> ciphertext, tag_type& tag
  ) const;

  bool decrypt(
      const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> aad,
      sn::util::span<const std::uint8_t> ciphertext, sn::util::span<std::uint8_t> plaintext, const tag_type& tag
  ) const;

  void encrypt(
      const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> plaintext,
      sn::util::span<std::uint8_t> ciphertext, tag_type& tag
  ) const {
    encrypt(key, nonce, sn::util::span<const std::uint8_t>{}, plaintext, ciphertext, tag);
  }

  bool decrypt(
      const key_type& key, const nonce_type& nonce, sn::util::span<const std::uint8_t> ciphertext,
      sn::util::span<std::uint8_t> plaintext, const tag_type& tag
  ) const {
    return decrypt(key, nonce, sn::util::span<const std::uint8_t>{}, ciphertext, plaintext, tag);
  }

  static key_type generate_key(prng& rng);
  static nonce_type generate_nonce(prng& rng);

private:
  using gcm_state_type =
#if defined(SONIC_CRYPTO_BACKEND_BEARSSL)
      detail::bearssl_gcm_cipher_state
#else
      detail::evp_cipher_ctx
#endif
      ;

  mutable gcm_state_type ctx_{};
};

}
