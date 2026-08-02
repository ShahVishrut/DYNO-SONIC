#pragma once

#include <cstdint>

namespace sn::oram::pathoram {

struct options {
  // N: number of addressable blocks
  std::uint64_t block_count = 0;
  std::uint32_t evict_batch = 1;
};

} // namespace sn::oram::pathoram
