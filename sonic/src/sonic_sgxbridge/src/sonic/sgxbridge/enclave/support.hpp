#pragma once

#include <cstdint>
#include <exception>
#include <new>
#include <string_view>

#include "sonic/util/log.hpp"

namespace sn::sgxbridge::enclave {

inline sn::util::log::level verbosity_to_level(std::uint32_t verbosity) {
  const int base = static_cast<int>(sn::util::log::level::info);
  const int max_level = static_cast<int>(sn::util::log::level::annoying);
  int requested = base + static_cast<int>(verbosity);
  if (requested > max_level) {
    requested = max_level;
  }
  return static_cast<sn::util::log::level>(requested);
}

inline void apply_global_verbosity(std::uint32_t verbosity) {
  sn::util::log::global_logger().set_verbosity(verbosity_to_level(verbosity));
}

template <typename Result, typename ErrorFactory, typename ExecuteFn, typename CleanupFn>
inline void execute_with_exception_boundary(
    sn::util::log::logger& logger, Result& out_result, ErrorFactory&& make_error, ExecuteFn&& exec, CleanupFn&& cleanup
) {
  auto call_cleanup = [&]() { cleanup(); };
  try {
    out_result = exec();
  } catch (const std::bad_alloc&) {
    logger.err("enclave oom");
    out_result = make_error("enclave oom");
    call_cleanup();
    return;
  } catch (const std::exception&) {
    logger.err("enclave exception");
    out_result = make_error("enclave exception");
    call_cleanup();
    return;
  } catch (...) {
    logger.err("enclave exception");
    out_result = make_error("enclave exception");
    call_cleanup();
    return;
  }
  call_cleanup();
}

}
