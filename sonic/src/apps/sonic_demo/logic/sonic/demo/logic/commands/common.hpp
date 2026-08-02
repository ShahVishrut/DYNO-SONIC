#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

#include "sonic/demo/types/intents.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/util/picoformat.hpp"

namespace sn::demo::logic::commands::detail {

inline types::command_result make_error(types::result_status status, std::string_view message) {
  return types::make_result(status, message);
}

inline const char* describe_action(types::oram_action action) noexcept {
  switch (action) {
  case types::oram_action::validate:
    return "validate";
  case types::oram_action::experiment:
    return "experiment";
  case types::oram_action::benchmark:
    return "benchmark";
  }
  return "unknown";
}

inline std::size_t clamp_accesses(std::uint64_t requested) noexcept {
  if (requested == 0) {
    return 0;
  }
  constexpr std::uint64_t k_max = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
  if (requested > k_max) {
    return static_cast<std::size_t>(k_max);
  }
  return static_cast<std::size_t>(requested);
}

inline sn::sgxbridge::tp::request make_threadpool_request(std::size_t workers, const char* label) {
  const std::uint32_t worker_u32 = workers > 0 ? static_cast<std::uint32_t>(workers) : 0u;
  std::uint32_t queue_hint = 0;
  if (workers > 0) {
    const std::uint64_t hint = static_cast<std::uint64_t>(worker_u32) * 4ull;
    queue_hint = hint > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                                  : static_cast<std::uint32_t>(hint);
  }
  return sn::sgxbridge::tp::make_request(
      worker_u32, queue_hint, sn::sgxbridge::tp::queue_policy::block_when_full, label
  );
}

inline types::command_result session_error(std::string_view label, const sn::sgxbridge::tp::result& res) {
  auto text = pfm::format(
      "%s thread pool request failed: status=%s detail=%u", label.data(), sn::sgxbridge::tp::describe(res.code),
      static_cast<unsigned>(res.detail)
  );
  return make_error(types::result_status::internal_error, text);
}

}
