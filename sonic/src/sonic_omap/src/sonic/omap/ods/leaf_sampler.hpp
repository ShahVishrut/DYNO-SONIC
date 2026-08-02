#pragma once

#include <cstdint>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/util/log.hpp"

namespace sn::omap::ods {

// sample a pseudorandom leaf index in [0, leaf_count)
[[nodiscard]] inline std::uint64_t sample_leaf(sn::crypto::buffered_prng<>& rng, std::uint64_t leaf_count) noexcept {
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(leaf_count > 0, "ods::sample_leaf: leaf_count must be positive");
#endif
  return rng.random_u64(0, leaf_count);
}

struct step_leaves {
  std::uint64_t dummy_cur = 0;
  std::uint64_t dummy_new = 0;
  std::uint64_t left_new = 0;
  std::uint64_t right_new = 0;
};

// for tree traversals, sample pseudorandom leaves for dummy normalization and children
[[nodiscard]] inline step_leaves sample_step_leaves(
    sn::crypto::buffered_prng<>& rng, std::uint64_t leaf_count
) noexcept {
  step_leaves out{};
  out.dummy_cur = sample_leaf(rng, leaf_count);
  out.dummy_new = sample_leaf(rng, leaf_count);
  out.left_new = sample_leaf(rng, leaf_count);
  out.right_new = sample_leaf(rng, leaf_count);
  return out;
}

} // namespace sn::omap::ods
