#pragma once

#include <cstddef>

#include "sonic/oram/pathoram/bucket.hpp"
#include "sonic/oram/pathoram/options.hpp"
#include "sonic/oram/stash/pathsort/stash.hpp"
#include "sonic/oram/tree/block.hpp"

namespace sn::oram::pathoram {

template <std::size_t BlockBytes> struct traits {
  static constexpr std::size_t block_bytes = BlockBytes;
  // pathoram Z=4 is the minimum secure value (and best for performance)
  static constexpr std::size_t bucket_size = 4;

  using block_t = sn::oram::tree::block<BlockBytes>;
  using options_t = sn::oram::pathoram::options;
  using bucket_t = sn::oram::pathoram::bucket<block_t, bucket_size>;
  using stash_t = sn::oram::stash::pathsort::stash<block_t>;
};

} // namespace sn::oram::pathoram
