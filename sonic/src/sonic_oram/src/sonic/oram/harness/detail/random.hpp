#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/crypto/random.hpp"
#include "sonic/util/span.hpp"
#include "sonic/util/xorshift.hpp"

namespace sn::oram::harness::detail {

class seed_generator {
public:
  seed_generator() = default;

  [[nodiscard]] sn::crypto::prng::seed_material next_seed() { return sn::crypto::prng::make_seed(rd_); }

  [[nodiscard]] std::uint64_t next_u64() {
    auto rng = sn::crypto::buffered_prng<>(next_seed());
    std::uint64_t seed = 0;
    while (seed == 0) {
      seed = rng.random_u64();
    }
    return seed;
  }

  [[nodiscard]] sn::crypto::buffered_prng<> spawn_prng() { return sn::crypto::buffered_prng<>(next_seed()); }

private:
  sn::crypto::random_device rd_{};
};

[[nodiscard]] inline std::uint64_t resolve_run_seed(std::uint64_t requested_seed) {
  if (requested_seed != 0) {
    return requested_seed;
  }
  seed_generator gen;
  return gen.next_u64();
}

[[nodiscard]] inline sn::crypto::prng::seed_material make_deterministic_seed(std::uint64_t seed_value) {
  sn::crypto::prng::seed_material seed{};
  sn::util::xoroshiro128ss seeder(seed_value);
  seeder.random_bytes(seed.key.data(), seed.key.size());
  seeder.random_bytes(seed.iv.data(), seed.iv.size());
  return seed;
}

[[nodiscard]] inline sn::crypto::buffered_prng<> make_prng(std::uint64_t seed_value) {
  return sn::crypto::buffered_prng<>(make_deterministic_seed(seed_value));
}

[[nodiscard]] inline sn::crypto::buffered_prng<> make_prng(seed_generator& gen) { return gen.spawn_prng(); }

class seed_stream {
public:
  explicit seed_stream(std::uint64_t seed) noexcept : rng_(seed) {}

  [[nodiscard]] std::uint64_t next_seed() noexcept {
    std::uint64_t seed = 0;
    while (seed == 0) {
      seed = rng_.random_u64();
    }
    return seed;
  }

  [[nodiscard]] sn::crypto::buffered_prng<> spawn_prng() { return make_prng(next_seed()); }

private:
  sn::util::xoroshiro128ss rng_;
};

[[nodiscard]] inline sn::crypto::buffered_prng<> make_prng(seed_stream& seeds) { return seeds.spawn_prng(); }

inline void fill_random_buffer(sn::util::span<std::uint8_t> buffer, sn::crypto::buffered_prng<>& rng) {
  rng.random_bytes(buffer);
}

template <typename Buffer> inline void fill_random_buffer(Buffer& buffer, sn::crypto::buffered_prng<>& rng) {
  rng.random_bytes(buffer.data(), buffer.size());
}

[[nodiscard]] inline std::uint64_t sample_leaf(sn::crypto::buffered_prng<>& rng, std::uint64_t leaf_count) noexcept {
  if (leaf_count == 0) {
    return 0;
  }
  return rng.random_u64(0, leaf_count);
}

} // namespace sn::oram::harness::detail
