#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

#if defined(SONIC_CRYPTO_BACKEND_BEARSSL)
#error "BearSSL doesn't support blake2b_hasher"
#endif

class blake2b_hasher {
public:
  static constexpr std::size_t max_digest_size = 64;
  static constexpr std::size_t default_digest_size = 32;

  explicit blake2b_hasher(std::size_t digest_size = default_digest_size);

  void update(sn::util::span<const std::uint8_t> data);

  void finalize(sn::util::span<std::uint8_t> out);

  static void hash(sn::util::span<const std::uint8_t> data, sn::util::span<std::uint8_t> out);

private:
  detail::evp_md_ctx ctx_{};
  std::size_t digest_size_ = default_digest_size;
  bool finalized_ = false;
};

}
