#pragma once

#include <cstddef>
#include <cstdint>
#include "sonic/crypto/cipher.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::sgxbridge::secure {

struct null_traits {
  struct key_type {};

  static constexpr bool enabled = false;
  static constexpr std::size_t overhead = 0;

  static void encrypt(
      const key_type&, sn::util::span<const std::uint8_t>, sn::util::span<const std::uint8_t>,
      sn::util::span<std::uint8_t>, sn::crypto::prng&
  ) {}

  static bool decrypt(
      const key_type&, sn::util::span<const std::uint8_t>, sn::util::span<const std::uint8_t>,
      sn::util::span<std::uint8_t>
  ) {
    return true;
  }
};

struct aes_gcm_traits {
  using cipher_type = sn::crypto::gcm_cipher;
  using key_type = cipher_type::key_type;
  using nonce_type = cipher_type::nonce_type;
  using tag_type = cipher_type::tag_type;

  static constexpr bool enabled = true;
  static constexpr std::size_t overhead = cipher_type::nonce_size + cipher_type::tag_size;

  static void encrypt(
      const key_type& key, sn::util::span<const std::uint8_t> aad, sn::util::span<const std::uint8_t> plaintext,
      sn::util::span<std::uint8_t> ciphertext_with_iv_tag, sn::crypto::prng& rng
  ) {
    sn::util::log::ensuref(
        ciphertext_with_iv_tag.size() == plaintext.size() + overhead, "secure encrypt", plaintext.size(),
        ciphertext_with_iv_tag.size()
    );
    cipher_type cipher{};
    nonce_type nonce = cipher_type::generate_nonce(rng);
    tag_type tag{};
    auto nonce_view = sn::util::span<std::uint8_t>(ciphertext_with_iv_tag.data(), cipher_type::nonce_size);
    sn::obliv::memcpy(nonce_view.data(), nonce.bytes.data(), nonce.bytes.size());
    auto cipher_view =
        sn::util::span<std::uint8_t>(ciphertext_with_iv_tag.data() + cipher_type::nonce_size, plaintext.size());
    cipher.encrypt(key, nonce, aad, plaintext, cipher_view, tag);
    auto tag_view = sn::util::span<std::uint8_t>(
        ciphertext_with_iv_tag.data() + cipher_type::nonce_size + plaintext.size(), cipher_type::tag_size
    );
    sn::obliv::memcpy(tag_view.data(), tag.bytes.data(), tag.bytes.size());
  }

  static bool decrypt(
      const key_type& key, sn::util::span<const std::uint8_t> aad,
      sn::util::span<const std::uint8_t> ciphertext_with_iv_tag, sn::util::span<std::uint8_t> plaintext
  ) {
    if (ciphertext_with_iv_tag.size() < overhead) {
      return false;
    }
    const std::size_t cipher_bytes = ciphertext_with_iv_tag.size() - overhead;
    if (cipher_bytes != plaintext.size()) {
      return false;
    }
    cipher_type cipher{};
    nonce_type nonce{};
    sn::obliv::memcpy(nonce.bytes.data(), ciphertext_with_iv_tag.data(), nonce.bytes.size());
    tag_type tag{};
    const auto tag_offset = nonce.bytes.size() + cipher_bytes;
    sn::obliv::memcpy(tag.bytes.data(), ciphertext_with_iv_tag.data() + tag_offset, tag.bytes.size());
    auto cipher_view =
        sn::util::span<const std::uint8_t>(ciphertext_with_iv_tag.data() + nonce.bytes.size(), cipher_bytes);
    return cipher.decrypt(key, nonce, aad, cipher_view, plaintext, tag);
  }
};

}
