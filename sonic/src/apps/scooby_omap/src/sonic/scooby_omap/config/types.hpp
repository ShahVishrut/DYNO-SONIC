#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#if !defined(SN_SGX_ENCLAVE)
#include <chrono>
#endif

#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/scooby_node/types/intents.hpp"
#include "sonic/threads/sync.hpp"

namespace sn::scooby::omap {

namespace sync = sn::threads;

using key_type = std::uint32_t;
inline constexpr key_type invalid_key = std::numeric_limits<key_type>::max();

using sn::scooby::types::scooby_omap_backend;
using sn::scooby::types::scooby_omap_intent;
using sn::scooby::types::scooby_omap_role;

inline constexpr std::size_t k_o2th_bucket_size = 64;
inline constexpr std::size_t k_pmchain_bucket_size = 64;

inline std::size_t round_up_to_multiple(std::size_t value, std::size_t multiple) {
  if (multiple == 0 || value == 0) {
    return std::max(value, multiple);
  }
  const std::size_t remainder = value % multiple;
  return remainder == 0 ? value : value + (multiple - remainder);
}

inline sn::sgxbridge::dist::recv_timeout recv_timeout_ms(std::uint64_t millis) noexcept {
#if defined(SN_SGX_ENCLAVE)
  return static_cast<sn::sgxbridge::dist::recv_timeout>(millis);
#else
  return std::chrono::milliseconds(static_cast<std::uint64_t>(millis));
#endif
}

inline const char* describe_role(scooby_omap_role role) {
  switch (role) {
  case scooby_omap_role::client:
    return "client";
  case scooby_omap_role::load_balancer:
    return "load_balancer";
  case scooby_omap_role::suboram:
    return "suboram";
  }
  return "unknown";
}

inline const char* describe_backend(scooby_omap_backend backend) {
  switch (backend) {
  case scooby_omap_backend::o2th:
    return "o2th";
  case scooby_omap_backend::pmchain:
    return "pmchain";
  }
  return "unknown";
}

}
