#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/crypto/prf.hpp"
#include "sonic/crypto/random.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::adapter::detail {

// reseed prng from random device
inline void reseed_prng(sn::crypto::buffered_prng<>& prng) {
  sn::crypto::random_device rd;
  prng.reseed(sn::crypto::prng::make_seed(rd));
}

// derive leaf index using prf over input words
template <typename... Words>
inline std::uint64_t derive_leaf(sn::crypto::prf& prf, std::uint64_t leaf_count, Words... words) noexcept {
  constexpr std::size_t kInputBytes = (sizeof(Words) + ... + 0);
  static_assert(kInputBytes <= sn::crypto::prf::max_input_size, "derive_leaf: PRF input exceeds max_input_size");
  std::array<std::uint8_t, kInputBytes> input{};

  std::size_t offset = 0;
  auto write_word = [&input, &offset](auto word) {
    sn::obliv::memcpy(input.data() + offset, &word, sizeof(word));
    offset += sizeof(word);
  };
  (write_word(words), ...);

  std::array<std::uint8_t, sn::crypto::prf::block_size> output{};
  prf.derive(sn::util::span<const std::uint8_t>(input.data(), input.size()), output);

  std::uint64_t value = 0;
  sn::obliv::memcpy(&value, output.data(), sizeof(value));
  return leaf_count == 0 ? 0 : (value % leaf_count);
}

} // namespace sn::oram::adapter::detail
