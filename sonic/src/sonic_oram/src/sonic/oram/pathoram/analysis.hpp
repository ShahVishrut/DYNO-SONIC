#pragma once

#include <cmath>
#include <cstdint>

#include "sonic/util/log.hpp"

namespace sn::oram::pathoram {

namespace log = sn::util::log;

inline std::uint64_t pathoram_tree_height(std::uint64_t block_count) {
  log::ensure(block_count > 0, "pathoram::analysis: block_count must be positive");
  if (block_count <= 1) {
    return 0;
  }

  // based on the experiments section of the pathoram paper
  // we choose L = log2(N) - 1
  return static_cast<std::uint64_t>(std::ceil(std::log2(block_count))) - 1;
}

} // namespace sn::oram::pathoram
