#pragma once

#include <cstdint>

#include "sonic/util/cputimer.hpp"

#if !defined(SONIC_NO_OS)
#include <chrono>
#endif

namespace sn::util::clock {

struct monotonic {
#if defined(SONIC_NO_OS)
  using time_point = sn::util::cpu_timer::counter_type;

  static time_point now() noexcept { return sn::util::cpu_timer::now(); }

  static double seconds_between(time_point start, time_point end) noexcept {
    const auto delta = end - start;
    const double ns = sn::util::cpu_timer::cycles_to_ns(delta);
    return ns / 1.0e9;
  }
#else
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;

  static time_point now() noexcept { return clock::now(); }

  static double seconds_between(time_point start, time_point end) noexcept {
    return std::chrono::duration<double>(end - start).count();
  }
#endif
};

}
