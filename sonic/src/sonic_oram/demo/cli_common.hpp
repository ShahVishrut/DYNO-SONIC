#pragma once

#include <algorithm>

#include "sonic/util/ext/args.hpp"
#include "sonic/util/log.hpp"

namespace sn::demo::cli {

inline sn::util::log::level verbosity_from_flags(int verbose_count, bool quiet) {
  using sn::util::log::level;
  if (quiet) {
    return level::warn;
  }
  switch (verbose_count) {
  case 0:
    return level::info;
  case 1:
    return level::verbose;
  case 2:
    return level::trace;
  case 3:
    return level::debug;
  case 4:
    return level::pedantic;
  default:
    sn::util::log::fail("verbosity_from_flags: too many -v flags");
    return level::debug; // unreachable
  }
}

inline void apply_global_verbosity(int verbose_count, bool quiet) {
  sn::util::log::global_logger().set_verbosity(verbosity_from_flags(verbose_count, quiet));
}

} // namespace sn::demo::cli
