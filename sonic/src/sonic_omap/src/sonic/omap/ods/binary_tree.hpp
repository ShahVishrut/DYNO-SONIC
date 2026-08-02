#pragma once

#include <cstdint>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/omap/ods/ptr.hpp"

namespace sn::omap::ods {

// select child pointer based on `go_right` (0 = left, 1 = right)
template <typename Node>
[[nodiscard]] inline sn::omap::ods::ptr select_child_ptr(const Node& n, sn::obliv::choice go_right) noexcept {
  const sn::omap::ods::ptr left = n.child[0];
  const sn::omap::ods::ptr right = n.child[1];
  return sn::omap::ods::ct_select_ptr(right, left, go_right);
}

// conditionally update child leaf values
template <typename Node>
inline void update_child_leaves_cond(
    Node& n, std::uint64_t left_new_leaf, sn::obliv::choice set_left, std::uint64_t right_new_leaf,
    sn::obliv::choice set_right
) noexcept {
  n.child[0].leaf = sn::obliv::ct_select<std::uint64_t>(left_new_leaf, n.child[0].leaf, set_left.unwrap());
  n.child[1].leaf = sn::obliv::ct_select<std::uint64_t>(right_new_leaf, n.child[1].leaf, set_right.unwrap());
}

// plan a single-child descent step for binary search tree traversal
// - selects next child pointer
// - mask traversal if child is dummy
// - conditionally update child leaf values
// - return next pending pointer, or dummy
template <typename Node>
[[nodiscard]] inline sn::omap::ods::pending_ptr plan_traverse_one_child(
    Node& n, sn::obliv::choice go_right, sn::obliv::choice want_traverse, std::uint64_t left_new_leaf,
    std::uint64_t right_new_leaf
) noexcept {
  // select child pointer based on go_right
  const sn::omap::ods::ptr child_sel = select_child_ptr(n, go_right);
  const sn::obliv::choice child_real = sn::omap::ods::is_real(child_sel);

  // traverse if we want and child is real
  const sn::obliv::choice traverse = want_traverse && child_real;

  // conditionally update child leaf values
  const sn::obliv::choice set_left = traverse && !go_right;
  const sn::obliv::choice set_right = traverse && go_right;
  update_child_leaves_cond(n, left_new_leaf, set_left, right_new_leaf, set_right);

  // select new leaf for child pending pointer
  const std::uint64_t child_new_leaf =
      sn::obliv::ct_select<std::uint64_t>(right_new_leaf, left_new_leaf, go_right.unwrap());
  // plan next pending pointer
  const sn::omap::ods::pending_ptr child_pending{child_sel.addr, child_sel.leaf, child_new_leaf};
  // return planned pending pointer, or dummy if not traversing
  return sn::omap::ods::ct_select_pending(child_pending, sn::omap::ods::pending_ptr{}, traverse);
}

} // namespace sn::omap::ods
