#include "sonic/crypto/hkdf.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/impl/openssl/detail/utils.hpp"
#include "sonic/util/memcpy.hpp"

namespace sn::crypto {

namespace {

constexpr std::size_t kMaxExpandBlocks = 255;

struct scoped_hmac_ctx {
  HMAC_CTX* ctx;

  scoped_hmac_ctx() {
    ctx = HMAC_CTX_new();
    if (!ctx) {
      throw error("HMAC_CTX_new failed");
    }
  }

  ~scoped_hmac_ctx() {
    if (ctx) {
      HMAC_CTX_free(ctx);
    }
  }

  operator HMAC_CTX*() { return ctx; }
};

void verify_output_length(std::size_t requested) {
  if (requested > kMaxExpandBlocks * hkdf_sha256::hash_len) {
    throw error("hkdf expand too long");
  }
}

void verify_prk_length(sn::util::span<const std::uint8_t> prk) {
  if (prk.size() != hkdf_sha256::hash_len) {
    throw error("hkdf prk wrong length");
  }
}

void verify_hash_output(sn::util::span<std::uint8_t> out) {
  if (out.size() != hkdf_sha256::hash_len) {
    throw error("hkdf output wrong length");
  }
}

}

void hkdf_sha256::extract(
    sn::util::span<const std::uint8_t> salt, sn::util::span<const std::uint8_t> ikm,
    sn::util::span<std::uint8_t> prk_out
) {
  verify_hash_output(prk_out);

  unsigned int len = 0;
  const void* salt_ptr = salt.empty() ? nullptr : salt.data();

  if (!HMAC(
          EVP_sha256(), salt_ptr, static_cast<int>(salt.size()), ikm.data(), static_cast<int>(ikm.size()),
          prk_out.data(), &len
      )) {
    detail::throw_openssl_error("HMAC extract failed");
  }

  if (len != hkdf_sha256::hash_len) {
    throw error("HMAC produced wrong length");
  }
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

  scoped_hmac_ctx ctx;
  std::array<std::uint8_t, hkdf_sha256::hash_len> block{};
  std::size_t produced = 0;
  std::uint8_t counter = 1;

  if (HMAC_Init_ex(ctx, prk.data(), static_cast<int>(prk.size()), EVP_sha256(), nullptr) != 1) {
    detail::throw_openssl_error("HMAC_Init_ex failed");
  }

  while (produced < okm_out.size()) {
    if (HMAC_Init_ex(ctx, nullptr, 0, nullptr, nullptr) != 1) {
      detail::throw_openssl_error("HMAC_Init_ex reset failed");
    }

    if (counter > 1) {
      if (HMAC_Update(ctx, block.data(), block.size()) != 1) {
        detail::throw_openssl_error("HMAC_Update (block) failed");
      }
    }

    if (!info.empty()) {
      if (HMAC_Update(ctx, info.data(), info.size()) != 1) {
        detail::throw_openssl_error("HMAC_Update (info) failed");
      }
    }

    if (HMAC_Update(ctx, &counter, 1) != 1) {
      detail::throw_openssl_error("HMAC_Update (counter) failed");
    }

    unsigned int len = 0;
    if (HMAC_Final(ctx, block.data(), &len) != 1) {
      detail::throw_openssl_error("HMAC_Final failed");
    }

    const std::size_t to_copy = std::min(block.size(), okm_out.size() - produced);
    sn::mem::copy_bytes(okm_out.data() + produced, block.data(), to_copy);

    produced += to_copy;
    ++counter;
  }

  detail::secure_zero(block.data(), block.size());
}

}
