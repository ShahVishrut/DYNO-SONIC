#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

class prf {
public:
  static constexpr std::size_t key_size = 32;
  static constexpr std::size_t block_size = 16;
  static constexpr std::size_t max_input_size = 11;

  struct key_type {
    std::array<std::uint8_t, key_size> bytes{};
  };

  prf();

  explicit prf(const key_type& key);

  ~prf();

  prf(const prf&) = delete;
  prf& operator=(const prf&) = delete;

  prf(prf&&) noexcept = default;
  prf& operator=(prf&&) noexcept = default;

  void set_key(const key_type& key);

  static key_type generate_key(prng& rng);

  void derive(sn::util::span<const std::uint8_t> input, sn::util::span<std::uint8_t> output);

private:
  using prf_state_type =
#if defined(SONIC_CRYPTO_BACKEND_BEARSSL)
      detail::bearssl_prf_state
#else
      detail::openssl_prf_state
#endif
      ;

  prf_state_type ctx_{};
  bool keyed_ = false;
};

}
