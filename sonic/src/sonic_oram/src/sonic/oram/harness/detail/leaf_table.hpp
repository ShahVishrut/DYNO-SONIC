#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/oram/harness/detail/random.hpp"

namespace sn::oram::harness::detail {

// track addr -> leaf position map
class leaf_table {
public:
  leaf_table() = default;
  explicit leaf_table(std::size_t block_count) : leaves_(block_count, 0) {}

  [[nodiscard]] std::uint64_t current(std::uint64_t address) const noexcept {
    return leaves_[static_cast<std::size_t>(address)];
  }

  void commit(std::uint64_t address, std::uint64_t leaf) noexcept { leaves_[static_cast<std::size_t>(address)] = leaf; }

  [[nodiscard]] std::size_t size() const noexcept { return leaves_.size(); }

private:
  std::vector<std::uint64_t> leaves_{};
};

// initialize random position map
[[nodiscard]] inline leaf_table make_random_leaf_table(
    std::size_t block_count, std::uint64_t leaf_count, detail::seed_stream& seeds
) {
  leaf_table table(block_count);
  auto rng = detail::make_prng(seeds);
  for (std::size_t i = 0; i < block_count; ++i) {
    table.commit(static_cast<std::uint64_t>(i), detail::sample_leaf(rng, leaf_count));
  }
  return table;
}

} // namespace sn::oram::harness::detail
