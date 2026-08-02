#include "sonic/crypto/hkdf.hpp"

#include <bearssl_hash.h>
#include <bearssl_hmac.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/impl/bearssl/detail/utils.hpp"
#include "sonic/util/memcpy.hpp"

namespace sn::crypto {

namespace {

constexpr std::size_t kMaxExpandBlocks = 255;

const std::array<std::uint8_t, hkdf_sha256::hash_len> kZeroSalt{};

void verify_hash_output(sn::util::span<std::uint8_t> out) {
  if (out.size() != hkdf_sha256::hash_len) {
    throw error("hkdf output wrong length");
  }
}

void verify_prk_length(sn::util::span<const std::uint8_t> prk) {
  if (prk.size() != hkdf_sha256::hash_len) {
    throw error("hkdf prk wrong length");
  }
}

void verify_output_length(std::size_t requested) {
  if (requested > kMaxExpandBlocks * hkdf_sha256::hash_len) {
    throw error("hkdf expand too long");
  }
}

void hmac_sha256(
    sn::util::span<const std::uint8_t> key, std::initializer_list<sn::util::span<const std::uint8_t>> parts,
    std::uint8_t* out
) {
  br_hmac_key_context key_ctx;
  br_hmac_key_init(&key_ctx, &br_sha256_vtable, key.data(), key.size());

  br_hmac_context hmac_ctx;
  br_hmac_init(&hmac_ctx, &key_ctx, hkdf_sha256::hash_len);
  for (const auto& part : parts) {
    if (!part.empty()) {
      br_hmac_update(&hmac_ctx, part.data(), part.size());
    }
  }

  br_hmac_out(&hmac_ctx, out);
  detail::secure_zero(&hmac_ctx, sizeof(hmac_ctx));
  detail::secure_zero(&key_ctx, sizeof(key_ctx));
}

}

void hkdf_sha256::extract(
    sn::util::span<const std::uint8_t> salt, sn::util::span<const std::uint8_t> ikm,
    sn::util::span<std::uint8_t> prk_out
) {
  verify_hash_output(prk_out);

  sn::util::span<const std::uint8_t> actual_salt = salt;
  if (actual_salt.empty()) {
    actual_salt = sn::util::span<const std::uint8_t>(kZeroSalt);
  }

  hmac_sha256(actual_salt, {ikm}, prk_out.data());
}

void hkdf_sha256::expand(
    sn::util::span<const std::uint8_t> prk, sn::util::span<const std::uint8_t> info,
    sn::util::span<std::uint8_t> okm_out
) {
  verify_prk_length(prk);
  verify_output_length(okm_out.size());
  if (okm_out.empty()) {
    return;
  }

  std::array<std::uint8_t, hkdf_sha256::hash_len> block{};
  std::size_t produced = 0;
  std::uint8_t counter = 1;

  while (produced < okm_out.size()) {
    if (produced == 0) {
      hmac_sha256(
          prk,
          {
              info,
              sn::util::span<const std::uint8_t>(&counter, 1),
          },
          block.data()
      );
    } else {
      hmac_sha256(
          prk,
          {
              sn::util::span<const std::uint8_t>(block.data(), block.size()),
              info,
              sn::util::span<const std::uint8_t>(&counter, 1),
          },
          block.data()
      );
    }

    const std::size_t to_copy = std::min(block.size(), okm_out.size() - produced);
    sn::mem::copy_bytes(okm_out.data() + produced, block.data(), to_copy);
    produced += to_copy;
    ++counter;
  }

  detail::secure_zero(block.data(), block.size());
}

}
