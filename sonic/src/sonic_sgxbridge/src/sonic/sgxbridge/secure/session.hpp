#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/crypto/prng.hpp"
#include "sonic/sgxbridge/secure/crypto_traits.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::sgxbridge::secure {

template <typename Traits> class session {
public:
  using key_type = typename Traits::key_type;

  session() = default;

  void configure(const key_type& key, std::size_t max_payload_size) {
    key_ = key;
    max_payload_bytes_ = max_payload_size;
    if constexpr (Traits::enabled) {
      scratch_.resize(max_payload_size);
      sn::obliv::fill(scratch_.begin(), scratch_.end(), std::uint8_t{0});
    } else {
      (void) key;
      (void) max_payload_size;
    }
  }

  [[nodiscard]] const key_type& key() const noexcept { return key_; }

  [[nodiscard]] sn::util::span<std::uint8_t> acquire_scratch(std::size_t size) {
    if constexpr (!Traits::enabled) {
      (void) size;
      return sn::util::span<std::uint8_t>();
    } else {
      sn::util::log::ensuref(size <= scratch_.size(), "secure scratch", size, scratch_.size());
      return sn::util::span<std::uint8_t>(scratch_.data(), size);
    }
  }

  [[nodiscard]] sn::crypto::prng& rng() noexcept { return rng_; }

  [[nodiscard]] std::size_t max_payload_bytes() const noexcept { return max_payload_bytes_; }

private:
  key_type key_{};
  std::vector<std::uint8_t> scratch_{};
  sn::crypto::prng rng_{};
  std::size_t max_payload_bytes_{0};
};

}
