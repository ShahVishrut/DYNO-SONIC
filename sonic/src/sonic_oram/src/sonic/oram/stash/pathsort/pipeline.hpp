#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/tree/path_buffer.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::oram::stash::pathsort::pipeline {

// geometry descriptor
struct shape {
  std::uint64_t height = 0;
  std::uint64_t bucket_size = 0;
  std::uint64_t path_block_count = 0;

  static shape make(std::uint64_t height, std::uint64_t bucket_size) noexcept {
    shape result{};
    result.height = height;
    result.bucket_size = bucket_size;
    result.path_block_count = (height + 1ULL) * bucket_size;
    return result;
  }
};

// scratch buffers
struct scratch {
  explicit scratch(std::size_t storage_size) :
      random_leaf_ixs(storage_size), keep_marks(storage_size), prefix(storage_size + 1) {}

  // buffer of random leaf indices for each block
  [[nodiscard]] sn::util::span<std::uint64_t> random_leafs() {
    return sn::util::span<std::uint64_t>(random_leaf_ixs.data(), random_leaf_ixs.size());
  }

  // buffer of keep flags used for compaction
  [[nodiscard]] sn::util::span<std::uint8_t> keep_mark_span() {
    return sn::util::span<std::uint8_t>(keep_marks.data(), keep_marks.size());
  }

  // prefix buffer for compaction
  [[nodiscard]] sn::util::span<std::size_t> prefix_span() {
    return sn::util::span<std::size_t>(prefix.data(), prefix.size());
  }

  std::vector<std::uint64_t> random_leaf_ixs;
  std::vector<std::uint8_t> keep_marks;
  std::vector<std::size_t> prefix;
};

// context for a path eviction
template <typename Block> struct context {
  sn::util::span<Block> storage;
  sn::util::span<Block> live;
  sn::util::span<Block> fillers;
  const sn::oram::tree::topology& topology;
  const shape& geom;
  std::uint64_t deferred_capacity = 0;
  sn::oram::uid_generator& uid_gen;
  sn::crypto::buffered_prng<>& prng;
  sn::util::log::logger& log;
};

// population after postprocessing
struct population {
  std::uint64_t evicted_total = 0; // blocks assigned to the eviction path (includes dummies)
  std::uint64_t evicted_real = 0;  // real blocks among the evicted set
  std::uint64_t retained_real = 0; // real blocks retained in the stash
};

// the result of the pipeline is an organization of storage into evicted/retained spans
// we don't care about the rest of the storage and it's left in an unspecified state
template <typename Block> struct result {
  population pop;
  sn::util::span<Block> evicted_span;
  sn::util::span<Block> retained_span;
};

// accessor helpers for the per-block target depth stored in extra
template <typename Block> inline void set_target_depth(Block& block, std::int64_t depth) noexcept {
  block.extra = depth;
}
template <typename Block> inline std::int64_t target_depth(const Block& block) noexcept { return block.extra; }

} // namespace sn::oram::stash::pathsort::pipeline

namespace sn::oram::stash::pathsort::pipeline {

// key for depth grouping sort
struct depth_group_key {
  std::int64_t depth;
  std::uint8_t is_dummy;
};

// extract key for depth grouping sort
template <typename Block> struct depth_group_key_extractor {
  depth_group_key operator()(const Block& block) const noexcept {
    depth_group_key key{};
    key.depth = target_depth(block);
    key.is_dummy = static_cast<std::uint8_t>(block.is_dummy().unwrap());
    return key;
  }
};

// comparator for depth grouping sort
struct depth_group_key_compare {
  // we implement the following ordering:
  // - depth (decreasing)
  // - is_dummy (real blocks before dummy blocks)
  bool operator()(const depth_group_key& lhs, const depth_group_key& rhs) const noexcept {
    constexpr bool use_bitwise_compare = false;
    if constexpr (use_bitwise_compare) {
      using namespace sn::obliv;
      const std::uint64_t lhs_key = (lhs.depth << 1U) | static_cast<std::uint64_t>(lhs.is_dummy ^ 0x1U);
      const std::uint64_t rhs_key = (rhs.depth << 1U) | static_cast<std::uint64_t>(rhs.is_dummy ^ 0x1U);
      return ct_gt<std::uint64_t>(lhs_key, rhs_key);
    } else {
      using namespace sn::obliv;
      const bool depth_gt = ct_gt<std::uint64_t>(lhs.depth, rhs.depth);
      const bool depth_eq = ct_eq<std::uint64_t>(lhs.depth, rhs.depth);
      const bool real_first = ct_lt<std::uint8_t>(lhs.is_dummy, rhs.is_dummy);
      return depth_gt | (depth_eq & real_first);
    }
  }
};

// precompute random leaf indices for each block
template <typename Block> inline void precompute_random_leaves(context<Block>& ctx, scratch& sc) {
  sn_prof_zone("pathsort.pipeline.random_leaves");
  auto rnd_leaves_span = sc.random_leafs();
  const std::uint64_t leaf_count = ctx.topology.leaf_count();
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      rnd_leaves_span.size() == ctx.storage.size(),
      "pathsort::precompute_random_leaves: scratch random buffer size mismatch (expected storage-sized)"
  );
#endif

  // only populate for the live region, as the filler region doesn't need random leaves
  for (std::size_t ix = 0; ix < ctx.live.size(); ++ix) {
    rnd_leaves_span[ix] = ctx.prng.random_u64(0, leaf_count);
  }
}

// populate the eviction buffer with filler blocks, one bucket per tree level
template <typename Block> inline void populate_fillers(context<Block>& ctx) {
  sn_prof_zone("pathsort.pipeline.populate_fillers");
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      ctx.fillers.size() == ctx.geom.path_block_count, "pathsort::populate_fillers: fillerslots span size mismatch"
  );
  ctx.log.dbgf("pathsort::populate_fillers: slots=%d", ctx.fillers.size());
#endif

  const std::size_t bucket_size = ctx.geom.bucket_size;
  const std::size_t height = ctx.geom.height;

  std::uint64_t level = 0;          // current level
  std::uint64_t level_fullness = 0; // number of blocks filled at current level

  // populate with filler blocks
  for (std::size_t ix = 0; ix < ctx.fillers.size(); ++ix) {
    Block& slot = ctx.fillers[ix];
    slot.set_dummy(ctx.uid_gen);

#if defined(ORAM_DEBUG)
    sn::util::log::ensure(level <= height, "pathsort::populate_fillers: dummy level exceeds tree height");
#endif
    // store target depth in extra data
    set_target_depth(slot, level);

    // advance level when current level is full
    ++level_fullness;
    if (level_fullness == bucket_size) {
      ++level;
      level_fullness = 0;
    }

#if defined(ORAM_DEBUG)
    const auto buf_ix = section_entry_index(ctx, ctx.fillers, ix);
    ctx.log.pedf("  filler[%03zu] buf[%03zu]: block#%d depth=%d", ix, buf_ix, slot.uid, target_depth(slot));
#endif
  }
}

// compute the deepest evictable depth for each block along evict path
template <typename Block>
inline void assign_evictable_depths(context<Block>& ctx, scratch& sc, std::uint64_t evict_leaf_node_id) {
  sn_prof_zone("pathsort.pipeline.assign_depths");
#if defined(ORAM_DEBUG)
  ctx.log.dbgf("pathsort::assign_evictable_depths: evict_leaf_node_id=%d", evict_leaf_node_id);
#endif

  const std::int64_t leaf_count = ctx.topology.leaf_count();
  auto random_leaves_span = sc.random_leafs();

  for (std::size_t ix = 0; ix < ctx.live.size(); ++ix) {
    Block& block = ctx.live[ix];
    const sn::obliv::choice block_is_real = block.is_real();

    const std::int64_t random_leaf_ix = random_leaves_span[ix];
    const std::int64_t real_leaf_ix = block.leaf_ix;

#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        random_leaf_ix >= 0 && random_leaf_ix < leaf_count,
        "pathsort::assign_evictable_depths: random leaf out of range"
    );
    // any real block must be assigned to a valid leaf
    if (block_is_real.unwrap()) {
      sn::util::log::ensure(
          block.leaf_ix >= 0 && block.leaf_ix < leaf_count, "pathsort::assign_evictable_depths: block leaf out of range"
      );
    }
#endif

    // get the leaf index to which this block is mapped; if dummy block then use random leaf
    const std::uint64_t mapped_leaf_ix =
        sn::obliv::ct_select<std::uint64_t>(real_leaf_ix, random_leaf_ix, block_is_real.unwrap());
    // get the node id of that leaf
    const std::uint64_t mapped_leaf_node_id = ctx.topology.leaf_index_to_node_id(mapped_leaf_ix);
    // determine the deepest depth along the eviction path where this block can reside
    const std::uint64_t depth = ctx.topology.ct_evictable_depth(mapped_leaf_node_id, evict_leaf_node_id);
    // store target depth in extra data
    set_target_depth(block, depth);

#if defined(ORAM_DEBUG)
    if (block_is_real.unwrap()) {
      const auto buf_ix = section_entry_index(ctx, ctx.live, ix);
      ctx.log.pedf(
          "  live[%03zu] buf[%03zu]: real block#%d (address=$%08x, leaf_ix=%d) assigned to depth=%d", ix, buf_ix,
          block.uid, block.address, block.leaf_ix, depth
      );
    } else {
      const auto buf_ix = section_entry_index(ctx, ctx.live, ix);
      ctx.log.pedf(
          "  live[%03zu] buf[%03zu]: dummy block#%d (rnd_leaf_ix=%d) assigned to depth=%d", ix, buf_ix, block.uid,
          random_leaf_ix, depth
      );
    }
#endif
  }
}

// sort all blocks to group by by target depth
template <typename Block> inline void target_depth_group_sort(context<Block>& ctx) {
  sn_prof_zone("pathsort.pipeline.group_sort");
#if defined(ORAM_DEBUG)
  ctx.log.dbgf("pathsort::target_depth_group_sort");
#endif

  // sort the entire storage: depth (dsc), is_dummy (asc)
  // this realizes a grouping similar to infty-oram along this path
  // this groups blocks to their desired target depth
  // this may case overfull bucket assignments, but we will correct that in postprocessing
  {
    depth_group_key_extractor<Block> key_extractor{};
    depth_group_key_compare key_compare{};
    sn::sortshuffle::ser::bitonic::bitonic_sort(ctx.storage.data(), ctx.storage.size(), key_extractor, key_compare);
  }

}

// postprocess to push up blocks from overfull buckets
template <typename Block> inline population postprocess_levels(context<Block>& ctx, scratch& sc) {
  sn_prof_zone("pathsort.pipeline.postprocess");
  using std::int64_t;
  using std::uint64_t;
  using namespace sn::obliv;

#if defined(ORAM_DEBUG)
  ctx.log.dbgf("pathsort::postprocess_levels: bucket_size=%d", ctx.geom.bucket_size);
#endif

  // reset scratch buffers
  auto marks = sc.keep_mark_span();
  auto prefix = sc.prefix_span();
  sn::obliv::fill(marks.begin(), marks.end(), static_cast<std::uint8_t>(0));
  sn::obliv::fill(prefix.begin(), prefix.end(), static_cast<std::size_t>(0));

  // each path has (H+1) buckets of size Z
  const uint64_t bucket_size = ctx.geom.bucket_size;
  const int64_t max_depth = static_cast<int64_t>(ctx.geom.height);
  // dropped blocks are sent to sentinel/unassigned depth -1
  const int64_t sentinel_depth = -1;

  // waterline starts at leaf level and climbs upward as buckets fill
  int64_t waterline_depth = max_depth;
  uint64_t waterline_fill = 0;

  // population counts
  uint64_t evicted_total = 0;
  uint64_t evicted_real = 0;
  uint64_t retained_real = 0;

  // we will scan the entire storage, which was grouped by target depth
  // this will start at the leaf level and go towards the root
  // for each block, we will decide whether to evict, retain, or drop it
  // real blocks are always either evicted or retained
  // dummy blocks are evicted if there's leftover space in the bucket, otherwise dropped
  // real blocks that can't be evicted to the path are retained (unevicted, kept in the stash)

  for (std::size_t ix = 0; ix < ctx.storage.size(); ++ix) {
    Block& block = ctx.storage[ix];

    // metadata: target depth, is_real
    const int64_t current_depth = target_depth(block);
    const choice is_real = block.is_real();
    const choice is_dummy = !is_real;

    // waterline state
    const choice path_full = choice(ct_lt<int64_t>(waterline_depth, static_cast<int64_t>(0)));
    // level has space if waterline_fill < bucket_size
    const choice level_has_space = !path_full && choice(ct_lt<uint64_t>(waterline_fill, bucket_size));
    // block assigned below waterline if current_depth > waterline_depth
    const choice assigned_below_waterline = choice(ct_gt<int64_t>(current_depth, waterline_depth));

    // real blocks take priority; dummy/filler is only kept when the bucket has remaining slots

    // keep as evicted real
    const choice keep_evicted_real = is_real && !path_full;
    // keep as evicted dummy (leftover space)
    const choice keep_evicted_dummy = is_dummy && level_has_space && !assigned_below_waterline;
    // keep as evicted (either real or dummy)
    const choice keep_evicted = keep_evicted_real || keep_evicted_dummy;
    // if real and not evicted, then retained
    const choice keep_retained_real = is_real && !keep_evicted;
    // if we keep in any form, either evicted or retained
    const choice keep_block = keep_evicted || keep_retained_real;
    // if dummy and not evicted, drop (garbage)
    const choice drop_block = !is_real && !keep_evicted;

    // if we keep the block in any form, mark for compaction
    marks[ix] = static_cast<std::uint8_t>(keep_block.unwrap());

    // if evicted, assign to waterline, otherwise sentinel (-1) for dropped blocks
    int64_t assigned_depth = ct_select<int64_t>(waterline_depth, sentinel_depth, keep_evicted.unwrap());
    set_target_depth(block, assigned_depth);

    // if we evicted the block, increment waterline fullness
    const uint64_t kept_fill = ct_select<uint64_t>(waterline_fill + 1ull, waterline_fill, keep_evicted.unwrap());
    // if the bucket is now full, raise waterline and reset fullness
    const bool bucket_full_now = keep_evicted.unwrap() && ct_eq<uint64_t>(kept_fill, bucket_size);
    waterline_fill = ct_select<uint64_t>(static_cast<uint64_t>(0), kept_fill, bucket_full_now);
    waterline_depth = ct_select<int64_t>(waterline_depth - 1, waterline_depth, bucket_full_now);

    // update population
    evicted_total += static_cast<std::uint64_t>(keep_evicted.unwrap());
    evicted_real += static_cast<std::uint64_t>(keep_evicted_real.unwrap());
    retained_real += static_cast<std::uint64_t>(keep_retained_real.unwrap());
  }

  population population{};
  population.evicted_total = evicted_total;
  population.evicted_real = evicted_real;
  population.retained_real = retained_real;

#if defined(ORAM_DEBUG)
  ctx.log.dbgf(
      "pathsort::postprocess_levels: evicted_total=%d evicted_real=%d retained_real=%d", evicted_total, evicted_real,
      retained_real
  );
#endif

  return population;
}

// compact kept blocks into evicted and retained sections
// evicted blocks are at the front, followed by retained blocks, followed by garbage
template <typename Block> inline void compact_kept_blocks(context<Block>& ctx, scratch& sc, const population& pop) {
  sn_prof_zone("pathsort.pipeline.compact_kept");
  using std::size_t;
  using std::uint64_t;

  auto marks = sc.keep_mark_span();
  auto prefix = sc.prefix_span();

#if defined(ORAM_DEBUG)
  const uint64_t kept_total = pop.evicted_total + pop.retained_real;
  ctx.log.dbgf(
      "pathsort::compact_kept_blocks: buffer_size=%d kept_total=%d evicted_total=%d retained_real=%d",
      ctx.storage.size(), kept_total, pop.evicted_total, pop.retained_real
  );
#endif

  // step 1: compact all kept blocks to the front, preserving their relative order
  // here we compact the full storage, as kept blocks could be anywhere
  // we use the marks for all kept blocks made during postprocessing
  {
    sn::sortshuffle::ser::orshuffle::orcompact(ctx.storage.data(), ctx.storage.size(), marks.data(), prefix.data());
  }

  // step 2: compact the kept blocks again so all evicted blocks are at the front, followed by retained blocks
  // we must preserve the relative order of evicted blocks, but order is unspecified for retained blocks
  // for obliviousness, we compact based on the upper bound of the number of kept blocks
  uint64_t kept_count_ub = ctx.geom.path_block_count + ctx.deferred_capacity;
#if defined(ORAM_DEBUG)
  // ensure the upper bound holds
  sn::util::log::ensure(
      ctx.log, kept_count_ub >= pop.evicted_total + pop.retained_real,
      "pathsort::compact_kept_blocks: kept count upper bound violated"
  );
#endif

  // mark only evicted blocks
  {
    for (size_t ix = 0; ix < kept_count_ub; ++ix) {
      const Block& block = ctx.storage[ix];
      // determine if block is retained (not evicted)
      const bool is_retained = sn::obliv::ct_eq<int64_t>(target_depth(block), static_cast<int64_t>(-1));
      marks[ix] = static_cast<std::uint8_t>(!is_retained);
    }
  }
  // compact evicted blocks to the front of the kept block section
  {
    sn::sortshuffle::ser::orshuffle::orcompact(ctx.storage.data(), kept_count_ub, marks.data(), prefix.data());
  }

}

// pipeline entry point; returns spans for evicted path and retained stash
template <typename Block> inline result<Block> run(context<Block>& ctx, scratch& sc, std::uint64_t leaf_ix) {
  sn_prof_zone("pathsort.pipeline.run");
#if defined(ORAM_DEBUG)
  ctx.log.trcf("pathsort::run: leaf_ix=%d", leaf_ix);
#endif

  const std::uint64_t evict_leaf_node_id = ctx.topology.leaf_index_to_node_id(leaf_ix);

  // precompute random leaf indices for each block
  precompute_random_leaves(ctx, sc);

  // tag blocks in live region with evictable depths
  assign_evictable_depths(ctx, sc, evict_leaf_node_id);
  // populate filler blocks
  populate_fillers(ctx);

  // sort blocks by target depth
  target_depth_group_sort(ctx);

  // postprocess buckets to enforce capacity
  const population pop = postprocess_levels(ctx, sc);

  // always check stash overflow for correctness
  sn::util::log::ensure(
      ctx.log, pop.retained_real <= ctx.deferred_capacity, "pathsort::run: retained blocks exceed deferred capacity"
  );

  // compact blocks into final layout
  // into layout: [ evicted | retained | garbage ]
  compact_kept_blocks(ctx, sc, pop);

  // return result
  result<Block> res{};
  res.pop = pop;
  // span of evicted blocks (path of blocks)
  res.evicted_span = sn::util::span<Block>(ctx.storage.data(), ctx.geom.path_block_count);
  // span of retained blocks (deferred capacity as an upper bound)
  res.retained_span = sn::util::span<Block>(
      ctx.storage.data() + ctx.geom.path_block_count, static_cast<std::size_t>(ctx.deferred_capacity)
  );

  return res;
}
} // namespace sn::oram::stash::pathsort::pipeline
