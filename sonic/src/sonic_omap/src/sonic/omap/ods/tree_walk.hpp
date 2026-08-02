#pragma once

#include <cstdint>
#include <utility>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/omap/ods/leaf_sampler.hpp"
#include "sonic/omap/ods/oram_record_access.hpp"
#include "sonic/omap/ods/ptr.hpp"

namespace sn::omap::ods {

// fixed-depth traversal helper for oram-backed ods
// - perform exactly H oram accesses
// - rerandomize root pointer
// - call StepFn at each step to update node and plan next pending pointer
// StepFn: void(std::uint32_t depth, Node& n, sn::obliv::choice active, const ods::step_leaves& leaves,
// ods::pending_ptr& next_p)

template <class OramClient, typename Node, class Scratch, class StepFn>
inline void walk_fixed_depth_from_root(
    OramClient& oram, sn::omap::ods::ptr& root, Scratch& scratch, std::uint32_t h, StepFn&& step_fn
) {
  const std::uint64_t leaf_count = oram.shape().leaf_count;

  // rerandomize root pointer
  const std::uint64_t root_new_leaf = sn::omap::ods::sample_leaf(scratch.rng, leaf_count);
  sn::omap::ods::pending_ptr p{root.addr, root.leaf, root_new_leaf};

  // one access per level, starting from root, chasing pending pointer down the tree
  for (std::uint32_t depth = 0; depth < h; ++depth) {
    const auto leaves = sn::omap::ods::sample_step_leaves(scratch.rng, leaf_count);

    const sn::obliv::choice active = sn::omap::ods::is_real(p);
    sn::omap::ods::pending_ptr next_p{};

    // access record at pointer, and invoke step_fn to update node and plan next pointer
    sn::omap::ods::access_record<OramClient, Node>(
        oram, p, active, leaves.dummy_cur, leaves.dummy_new, scratch.oram, scratch.io,
        [&](Node& n) noexcept { std::forward<StepFn>(step_fn)(depth, n, active, leaves, next_p); }
    );

    // after the root-level access, save updated root pointer
    if (depth == 0) {
      sn::omap::ods::update_ptr_leaf_after_access(root, p.new_leaf, active);
    }

    // chase planned pending pointer
    p = next_p;
  }
}

} // namespace sn::omap::ods
