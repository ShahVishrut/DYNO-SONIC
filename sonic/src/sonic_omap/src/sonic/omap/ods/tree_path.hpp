#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/omap/ods/ptr.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::ods {

template <typename Node> struct alignas(16) path_entry {
  // where this node will be written during writeback
  ods::ptr self{};

  // cached node contents
  Node node{};

  // direction taken at this node during a search walk
  // 0 = left, 1 = right
  std::uint8_t dir = 0; // 0=left, 1=right

  // whether this entry corresponds to a real node
  std::uint8_t valid = 0; // 0/1
  std::uint8_t _pad[14]{};
};

// fixed-size (H) path cache for a binary tree path
template <typename Node> class tree_path {
public:
  static_assert(std::is_trivially_copyable_v<Node>, "ods::tree_path requires trivially copyable Node");
  static_assert(std::is_trivially_copyable_v<path_entry<Node>>, "ods::tree_path: entry must be trivially copyable");

  void configure(std::size_t depth) { entries_.assign(depth, path_entry<Node>{}); }

  [[nodiscard]] std::size_t depth() const noexcept { return entries_.size(); }

  void clear() {
    for (auto& e : entries_) {
      e.valid = 0;
    }
  }

  [[nodiscard]] sn::util::span<path_entry<Node>> entries() noexcept {
    return sn::util::span<path_entry<Node>>(entries_.data(), entries_.size());
  }
  [[nodiscard]] sn::util::span<const path_entry<Node>> entries() const noexcept {
    return sn::util::span<const path_entry<Node>>(entries_.data(), entries_.size());
  }

private:
  std::vector<path_entry<Node>> entries_{};
};

// before writeback, fix up cached child pointer leaf fields, so pointers point to updated leaves
template <typename Node>
inline void fixup_child_leaves_for_writeback(
    sn::util::span<path_entry<Node>> path, sn::util::span<const std::uint64_t> new_leaf_per_level
) noexcept {
  static_assert(
      std::is_trivially_copyable_v<Node>, "fixup_child_leaves_for_writeback requires trivially copyable Node"
  );

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(path.size() == new_leaf_per_level.size(), "tree_path fixup: size mismatch");
#endif

  const std::size_t h = path.size();
  for (std::size_t i = 0; i < h; ++i) {
    // address of this node (negative if dummy)
    const std::int64_t addr_i = path[i].self.addr;
    const std::uint64_t new_leaf_i = new_leaf_per_level[i];
    const sn::obliv::choice addr_is_real = sn::obliv::choice(addr_i >= 0);

    for (std::size_t j = 0; j < h; ++j) {
      // always touch both children
      // this is an in-cache O(H^2) pass
      for (int c = 0; c < 2; ++c) {
        const ods::ptr child = path[j].node.child[c];
        // if the child pointer matches this node's address
        const sn::obliv::choice match =
            addr_is_real && sn::obliv::choice(sn::obliv::ct_eq<std::int64_t>(child.addr, addr_i));
        // update its leaf field to the new leaf
        path[j].node.child[c].leaf =
            sn::obliv::ct_select<std::uint64_t>(new_leaf_i, path[j].node.child[c].leaf, match.unwrap());
      }
    }
  }
}

} // namespace sn::omap::ods
