#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sn::util {

namespace detail {

[[nodiscard]] inline __attribute__((always_inline)) std::uint64_t rotl64(std::uint64_t value, unsigned shift) noexcept {
  const unsigned amount = shift & 63u;
  if (amount == 0) {
    return value;
  }
  return (value << amount) | (value >> (64u - amount));
}

static constexpr double k_inv_u53 = 0x1.0p-53;
static constexpr float k_inv_u24 = 0x1.0p-24f;

template <typename Generator>
[[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_below(
    Generator& gen, std::uint64_t bound
) noexcept {
  if (bound == 0) {
    return gen.random_u64();
  }
  const std::uint64_t threshold = static_cast<std::uint64_t>((static_cast<__uint128_t>(-bound)) % bound);
  for (;;) {
    const std::uint64_t r = gen.random_u64();
    const __uint128_t product = static_cast<__uint128_t>(r) * static_cast<__uint128_t>(bound);
    if (static_cast<std::uint64_t>(product) >= threshold) {
      return static_cast<std::uint64_t>(product >> 64);
    }
  }
}

template <typename Generator>
inline __attribute__((always_inline)) void fill_bytes(Generator& gen, std::uint8_t* dst, std::size_t len) noexcept {
  std::size_t offset = 0;
  while ((len - offset) >= sizeof(std::uint64_t)) {
    const std::uint64_t value = gen.random_u64();
    std::memcpy(dst + offset, &value, sizeof(value));
    offset += sizeof(value);
  }
  if (offset == len) {
    return;
  }
  const std::uint64_t value = gen.random_u64();
  for (std::size_t i = 0; offset < len; ++i, ++offset) {
    const unsigned shift = static_cast<unsigned>(56u - 8u * i);
    dst[offset] = static_cast<std::uint8_t>(value >> shift);
  }
}

}

class splitmix64 {
public:
  explicit splitmix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

private:
  std::uint64_t state_;
};

class xorshift64star {
public:
  constexpr xorshift64star(std::uint64_t seed = k_default_seed) noexcept : state_(seed ? seed : k_default_seed) {}

  void reseed(std::uint64_t seed) noexcept { state_ = seed ? seed : k_default_seed; }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_u64() noexcept {
    std::uint64_t x = state_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state_ = x;
    return x * 2685821657736338717ULL;
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint32_t random_u32() noexcept {
    return static_cast<std::uint32_t>(random_u64() >> 32);
  }

  [[nodiscard]] inline __attribute__((always_inline)) double random_f64() noexcept {
    return static_cast<double>(random_u64() >> 11) * detail::k_inv_u53;
  }

  [[nodiscard]] inline __attribute__((always_inline)) float random_f32() noexcept {
    return static_cast<float>(random_u64() >> 40) * detail::k_inv_u24;
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_below(std::uint64_t bound) noexcept {
    return detail::random_below(*this, bound);
  }

  inline __attribute__((always_inline)) void discard(std::uint64_t count) noexcept {
    while (count != 0) {
      static_cast<void>(random_u64());
      --count;
    }
  }

  void random_bytes(std::uint8_t* dst, std::size_t len) noexcept { detail::fill_bytes(*this, dst, len); }

private:
  static constexpr std::uint64_t k_default_seed = 0x9E3779B97F4A7C15ULL;

  std::uint64_t state_;
};

class xoroshiro128ss {
public:
  constexpr xoroshiro128ss(std::uint64_t seed0, std::uint64_t seed1) noexcept : state0_(seed0), state1_(seed1) {
    if ((state0_ | state1_) == 0) {
      state0_ = k_default_seed_lo;
      state1_ = k_default_seed_hi;
    }
  }

  explicit xoroshiro128ss(std::uint64_t seed) noexcept { reseed(seed); }

  void reseed(std::uint64_t seed) noexcept {
    splitmix64 seeder(seed);
    state0_ = seeder.next();
    state1_ = seeder.next();
    if ((state0_ | state1_) == 0) {
      state0_ = k_default_seed_lo;
      state1_ = k_default_seed_hi;
    }
  }

  void reseed(std::uint64_t seed0, std::uint64_t seed1) noexcept {
    state0_ = seed0;
    state1_ = seed1;
    if ((state0_ | state1_) == 0) {
      state0_ = k_default_seed_lo;
      state1_ = k_default_seed_hi;
    }
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_u64() noexcept {
    const std::uint64_t result = detail::rotl64(state0_ * 5ULL, 7) * 9ULL;
    std::uint64_t s1 = state1_ ^ state0_;
    state0_ = detail::rotl64(state0_, 24) ^ s1 ^ (s1 << 16);
    state1_ = detail::rotl64(s1, 37);
    return result;
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint32_t random_u32() noexcept {
    return static_cast<std::uint32_t>(random_u64() >> 32);
  }

  [[nodiscard]] inline __attribute__((always_inline)) double random_f64() noexcept {
    return static_cast<double>(random_u64() >> 11) * detail::k_inv_u53;
  }

  [[nodiscard]] inline __attribute__((always_inline)) float random_f32() noexcept {
    return static_cast<float>(random_u64() >> 40) * detail::k_inv_u24;
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_below(std::uint64_t bound) noexcept {
    return detail::random_below(*this, bound);
  }

  inline __attribute__((always_inline)) void discard(std::uint64_t count) noexcept {
    while (count != 0) {
      static_cast<void>(random_u64());
      --count;
    }
  }

  void random_bytes(std::uint8_t* dst, std::size_t len) noexcept { detail::fill_bytes(*this, dst, len); }

private:
  static constexpr std::uint64_t k_default_seed_lo = 0x9E3779B97F4A7C15ULL;
  static constexpr std::uint64_t k_default_seed_hi = 0xD2B74407B1CE6E93ULL;

  std::uint64_t state0_;
  std::uint64_t state1_;
};

class xoshiro256ss {
public:
  constexpr xoshiro256ss(std::uint64_t s0, std::uint64_t s1, std::uint64_t s2, std::uint64_t s3) noexcept :
      state_{s0, s1, s2, s3} {
    if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
      state_[0] = 0x9E3779B97F4A7C15ULL;
      state_[1] = 0xD2B74407B1CE6E93ULL;
      state_[2] = 0x6A09E667F3BCC909ULL;
      state_[3] = 0xBB67AE8584CAA73BULL;
    }
  }

  explicit xoshiro256ss(std::uint64_t seed) noexcept { reseed(seed); }

  void reseed(std::uint64_t seed) noexcept {
    splitmix64 seeder(seed);
    for (std::uint64_t& word : state_) {
      word = seeder.next();
    }
    if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
      state_[0] = 0x9E3779B97F4A7C15ULL;
      state_[1] = 0xD2B74407B1CE6E93ULL;
      state_[2] = 0x6A09E667F3BCC909ULL;
      state_[3] = 0xBB67AE8584CAA73BULL;
    }
  }

  void reseed(std::uint64_t s0, std::uint64_t s1, std::uint64_t s2, std::uint64_t s3) noexcept {
    state_[0] = s0;
    state_[1] = s1;
    state_[2] = s2;
    state_[3] = s3;
    if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
      state_[0] = 0x9E3779B97F4A7C15ULL;
      state_[1] = 0xD2B74407B1CE6E93ULL;
      state_[2] = 0x6A09E667F3BCC909ULL;
      state_[3] = 0xBB67AE8584CAA73BULL;
    }
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_u64() noexcept {
    const std::uint64_t result = detail::rotl64(state_[1] * 5ULL, 7) * 9ULL;
    const std::uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = detail::rotl64(state_[3], 45);
    return result;
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint32_t random_u32() noexcept {
    return static_cast<std::uint32_t>(random_u64() >> 32);
  }

  [[nodiscard]] inline __attribute__((always_inline)) double random_f64() noexcept {
    return static_cast<double>(random_u64() >> 11) * detail::k_inv_u53;
  }

  [[nodiscard]] inline __attribute__((always_inline)) float random_f32() noexcept {
    return static_cast<float>(random_u64() >> 40) * detail::k_inv_u24;
  }

  [[nodiscard]] inline __attribute__((always_inline)) std::uint64_t random_below(std::uint64_t bound) noexcept {
    return detail::random_below(*this, bound);
  }

  inline __attribute__((always_inline)) void discard(std::uint64_t count) noexcept {
    while (count != 0) {
      static_cast<void>(random_u64());
      --count;
    }
  }

  void random_bytes(std::uint8_t* dst, std::size_t len) noexcept { detail::fill_bytes(*this, dst, len); }

private:
  std::uint64_t state_[4];
};

using xorshift_rng = xoroshiro128ss;

}
