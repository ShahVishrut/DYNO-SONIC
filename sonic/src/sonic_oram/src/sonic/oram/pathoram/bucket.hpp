#pragma once

#include <array>
#include <cstddef>

#include "sonic/oram/tree/block.hpp"
#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/tree/block.hpp"

namespace sn::oram::pathoram {

template <typename Block, std::size_t BucketSize> struct bucket {
  static constexpr std::size_t slot_count = BucketSize;

  std::array<Block, BucketSize> slots{};

  void fill_dummy(sn::oram::uid_generator& uid_gen) {
    for (auto& slot : slots) {
      slot.set_dummy(uid_gen);
    }
  }

  Block& operator[](std::size_t index) noexcept { return slots[index]; }
  const Block& operator[](std::size_t index) const noexcept { return slots[index]; }
};

} // namespace sn::oram::pathoram
