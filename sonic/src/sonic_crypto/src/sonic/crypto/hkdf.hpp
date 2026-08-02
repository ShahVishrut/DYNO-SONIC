#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

class hkdf_sha256 {
public:
  static constexpr std::size_t hash_len = 32;

  static void extract(
      sn::util::span<const std::uint8_t> salt, sn::util::span<const std::uint8_t> ikm,
      sn::util::span<std::uint8_t> prk_out
  );

  static void expand(
      sn::util::span<const std::uint8_t> prk, sn::util::span<const std::uint8_t> info,
      sn::util::span<std::uint8_t> okm_out
  );
};

}
