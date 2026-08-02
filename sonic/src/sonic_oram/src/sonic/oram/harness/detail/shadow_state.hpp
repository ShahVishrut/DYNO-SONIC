#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sn::oram::harness::detail {

// shadow state for checking ram correctness
template <std::size_t BlockBytes> class shadow_state {
public:
  using block_data = std::array<std::uint8_t, BlockBytes>;

  shadow_state() = default;
  explicit shadow_state(std::size_t block_count) : blocks_(block_count) { clear(); }

  void clear() noexcept {
    for (auto& block : blocks_) {
      block.fill(0);
    }
  }

  [[nodiscard]] block_data& block(std::uint64_t address) noexcept { return blocks_[static_cast<std::size_t>(address)]; }

  [[nodiscard]] const block_data& block(std::uint64_t address) const noexcept {
    return blocks_[static_cast<std::size_t>(address)];
  }

private:
  std::vector<block_data> blocks_{};
};

} // namespace sn::oram::harness::detail
