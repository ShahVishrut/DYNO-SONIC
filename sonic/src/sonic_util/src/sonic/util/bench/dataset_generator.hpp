#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/util/xorshift.hpp"

namespace sn::util::bench {

inline std::uint64_t mix_seed(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  value ^= (value >> 31U);
  return value;
}

inline std::uint64_t mix_seed(std::uint64_t base, std::uint64_t salt) noexcept { return mix_seed(base ^ salt); }

inline sn::util::xorshift_rng make_rng(std::uint64_t seed0, std::uint64_t seed1) noexcept {
  return sn::util::xorshift_rng(mix_seed(seed0), mix_seed(seed1));
}

inline sn::util::xorshift_rng make_rng(std::uint64_t base_seed, std::uint64_t salt0, std::uint64_t salt1) noexcept {
  return make_rng(base_seed ^ salt0, base_seed ^ salt1);
}

inline void fill_random_bytes(sn::util::xorshift_rng& rng, void* dst, std::size_t len) noexcept {
  if (len == 0) {
    return;
  }
  rng.random_bytes(static_cast<std::uint8_t*>(dst), len);
}

template <typename T> inline void fill_random_span(sn::util::xorshift_rng& rng, T* data, std::size_t count) {
  if (count == 0) {
    return;
  }
  fill_random_bytes(rng, data, count * sizeof(T));
}

template <typename T> inline void fill_random_vector(sn::util::xorshift_rng& rng, std::vector<T>& values) {
  fill_random_span(rng, values.data(), values.size());
}

template <typename T> inline std::vector<T> make_random_vector(std::size_t count, sn::util::xorshift_rng& rng) {
  std::vector<T> values(count);
  fill_random_vector(rng, values);
  return values;
}

template <typename T> inline void reset_from_baseline(const std::vector<T>& baseline, std::vector<T>& working) {
  working = baseline;
}

inline void reset_bytes(const std::vector<std::uint8_t>& baseline, std::vector<std::uint8_t>& working) {
  working = baseline;
}

}
