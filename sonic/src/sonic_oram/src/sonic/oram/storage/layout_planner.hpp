#pragma once

#include <cstdint>
#include <utility>

namespace sn::oram::zingoram::storage {

struct hot_layout_plan {
  // number of complete levels (starting at level 0, root), placed in hot tier
  std::uint32_t hot_levels = 0;
  // depest hot level (0-indexed); -1 if no hot levels
  std::int64_t hot_level_max = -1;
  // number of hot nodes (excluding sentinel 0)
  std::uint64_t hot_node_count = 0;
  // largest node id that is hot (0 if no hot nodes)
  std::uint64_t hot_last_node_id = 0;
};

// compute how many whole levels can be kept hot within a byte budget; if budget_bytes is 0, keep all levels hot
inline hot_layout_plan plan_hot_layout(std::uint64_t height, std::uint64_t bucket_bytes, std::uint64_t budget_bytes) {
  hot_layout_plan plan{};

  const std::uint64_t total_levels = height + 1ULL;
  if (budget_bytes == 0) {
    plan.hot_levels = static_cast<std::uint32_t>(total_levels);
  } else {
    std::uint64_t used = 0;
    std::uint32_t level = 0;
    for (; level < total_levels; ++level) {
      const std::uint64_t level_nodes = 1ULL << level;
      const std::uint64_t level_bytes = level_nodes * bucket_bytes;
      if (used + level_bytes > budget_bytes) {
        break; // next level would exceed budget
      }
      used += level_bytes;
    }
    plan.hot_levels = level;
  }

  if (plan.hot_levels > 0) {
    plan.hot_level_max = static_cast<std::int64_t>(plan.hot_levels) - 1;
    plan.hot_node_count = (1ULL << plan.hot_levels) - 1ULL;
    plan.hot_last_node_id = plan.hot_node_count; // nodes 1..hot_last_node_id are hot
  }

  return plan;
}

} // namespace sn::oram::zingoram::storage
