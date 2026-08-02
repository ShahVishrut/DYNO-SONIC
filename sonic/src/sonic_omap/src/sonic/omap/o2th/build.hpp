#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "sonic/omap/o2th/helpers.hpp"
#include "sonic/sortshuffle/par/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::omap::o2th {

// populate the first level
template <typename Key, std::size_t BlockSize>
inline void populate_level_one(
    state<Key, BlockSize>& st,
    sn::util::span<
        typename table_types<Key, BlockSize>::template maybe_dummy<typename table_types<Key, BlockSize>::op_request>>
        in_data
) {
  sn_prof_zone("o2th.populate_l1");
  // this will assign all real blocks (keys) to a uniformly random bucket
  // additionally, filler blocks will be assigned to fully fill all buckets
  // so we are guaranteed to fill every bucket, preferably with real blocks
  const std::size_t worker_count = st.worker_state_count();
  const std::size_t total = in_data.size();
#if defined(ORAM_DEBUG)
  st.log.dbgf("o2th.populate_level_one: total=%zu, worker_count=%zu", total, worker_count);
#endif

  st.workers.parallel_work([&st, in_data, total, worker_count](std::size_t worker_ix, std::size_t) noexcept {
    auto [begin, end] = sn::threads::partition_evenly(worker_ix, total, worker_count);
    if (begin >= end) {
      return;
    }
    auto& worker = st.worker_state_for_index(worker_ix);
    for (std::size_t ix = begin; ix < end; ++ix) {
      const auto& input = in_data[ix];
      auto& block = st.level1_blocks[ix];
      block.reset();

      // whether the in item is dummy
      const sn::obliv::choice is_real(!input.is_dummy);
      const sn::obliv::choice is_write(input.value.is_write);
      const sn::obliv::choice is_read(!input.value.is_write);

      // conditionally set key and data (default to dummy)
      block.key = sn::obliv::ct_select<typename table_types<Key, BlockSize>::key_type>(
          input.value.key, table_types<Key, BlockSize>::invalid_key_value(), is_real.unwrap()
      );
      sn::obliv::ct_set_words<BlockSize>(block.data.data(), input.value.data.data(), is_real.unwrap());
      block.extra_data = input.value.extra_data;

      // conditionally set flags
      block.set_flags(is_real, is_real && (!is_write), is_real && is_write);

      // assign position tags
      block.tag_l1 = derive_bucket_index<1, Key, BlockSize>(worker, block.key, st.bucket_count);
      block.tag_l2 = derive_bucket_index<2, Key, BlockSize>(worker, block.key, st.bucket_count);
#if defined(ORAM_DEBUG)
      sn::util::log::ensure(block.tag_l1 < st.bucket_count, "o2th_rwkv: tag_l1 out of range");
      sn::util::log::ensure(block.tag_l2 < st.bucket_count, "o2th_rwkv: tag_l2 out of range");
      if (st.debug_should_log(sn::util::log::level::pedantic)) {
        st.log.pedf(
            "  populate_l1[%zu]: key=%lld, bucket_l1=%u, bucket_l2=%u, flags=0x%02x", ix,
            static_cast<long long>(block.key), static_cast<unsigned>(block.tag_l1), static_cast<unsigned>(block.tag_l2),
            block.flags
        );
      }
#endif
    }
  });
}

// populate filler blocks in both levels
template <typename Key, std::size_t BlockSize> inline void populate_fillers(state<Key, BlockSize>& st) {
  sn_prof_zone("o2th.populate_fillers");
  const std::size_t filler_begin = st.bucket_block_count;
  std::size_t filler_counter = 0;
#if defined(ORAM_DEBUG)
  st.log.dbgf("o2th.populate_fillers: filler_slots=%zu", st.level1_blocks.size() - filler_begin);
#endif
  for (std::size_t ix = filler_begin; ix < st.level1_blocks.size(); ++ix) {
    auto& block_l1 = st.level1_blocks[ix];
    auto& block_l2 = st.level2_blocks[ix];
    block_l1.reset();
    block_l2.reset();

    // clear key
    block_l1.flags = table_types<Key, BlockSize>::flag_mask(table_types<Key, BlockSize>::block_flag::filler);
    block_l2.flags = table_types<Key, BlockSize>::flag_mask(table_types<Key, BlockSize>::block_flag::filler);

    // assign bin index based on counter
    const typename table_types<Key, BlockSize>::bucket_index bin =
        static_cast<typename table_types<Key, BlockSize>::bucket_index>(filler_counter / st.cfg.bucket_size);
    block_l1.tag_l1 = bin;
    block_l2.tag_l2 = bin;
    ++filler_counter;
#if defined(ORAM_DEBUG)
    if (st.debug_should_log(sn::util::log::level::pedantic)) {
      st.log.pedf("  filler[%zu]: tag=%u", ix, static_cast<unsigned>(bin));
    }
#endif
  }
}

// copy overflow blocks from level 1 to level 2
// after placement on level 1, any blocks that didn't fit go to the overflow section
// overflow blocks participate in level 2's placement
template <typename Key, std::size_t BlockSize> inline void copy_overflow_to_level_two(state<Key, BlockSize>& st) {
  sn_prof_zone("o2th.copy_overflow");
  const std::size_t overflow_begin = st.bucket_block_count;
  const std::size_t overflow_count = st.level1_blocks.size() - overflow_begin;
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(overflow_count == st.bucket_block_count, "o2th_rwkv: overflow section mismatch during copy");
  st.log.dbgf("o2th.copy_overflow: transferring overflow_begin=%zu overflow_count=%zu", overflow_begin, overflow_count);
#endif
  // all filler blocks are populated after the main bucket block slots
  // so if we copy the excess blocks into the main slots, it will put all the real excess there
  // then sort/compaction will add appropriate numbers of filler
  // actually, we want to ensure that the entirety of the main range of level2 is overwritten
  sn::obliv::copy(
      st.level1_blocks.begin() + static_cast<std::ptrdiff_t>(overflow_begin), st.level1_blocks.end(),
      st.level2_blocks.begin()
  );
#if defined(ORAM_DEBUG)
  st.debug_dump_blocks(st.level2_blocks, "o2th.copy_overflow:level2_seed");
#endif
}

// placement algorithm: assign blocks to buckets and handle overflow
// steps:
//   1. sort blocks by bin number (primary key) and realness (secondary key, real before dummy)
//   2. scan to mark blocks that overflow their bucket as excess
//   3. compact to separate non-excess blocks (placed in buckets) from excess blocks (overflow)
//   4. if allow_overflow, compact the overflow section to put real excess before dummy excess
template <typename Key, std::size_t BlockSize>
inline void placement(
    state<Key, BlockSize>& st, std::vector<typename table_types<Key, BlockSize>::op_block>& blocks, bool allow_overflow
) {
  sn_prof_zone("o2th.placement");
#if defined(ORAM_DEBUG)
  const char* level_name = allow_overflow ? "level1" : "level2";
  st.log.dbgf(
      "o2th.placement[%s]: begin (block_count=%zu, allow_overflow=%s)", level_name, blocks.size(),
      allow_overflow ? "true" : "false"
  );
  // ensure compaction buffer capacity
  sn::util::log::ensure(
      st.compact_marks.size() >= blocks.size(), "o2th_rwkv: compact_marks capacity underrun during placement"
  );
  sn::util::log::ensure(
      st.compact_prefix.size() >= blocks.size() + 1, "o2th_rwkv: compact_prefix capacity underrun during placement"
  );
#endif

  // sort by bin number, prioritizing real blocks over filler
  auto key_extractor = [allow_overflow](const typename table_types<Key, BlockSize>::op_block& block) noexcept {
    const std::uint64_t bin =
        allow_overflow ? static_cast<std::uint64_t>(block.tag_l1) : static_cast<std::uint64_t>(block.tag_l2);
    const std::uint64_t priority = static_cast<uint64_t>(!block.is_real().unwrap());
    return (bin << 1U) | priority;
  };

  auto comparator = [](std::uint64_t lhs, std::uint64_t rhs) noexcept { return sn::obliv::ct_lt(lhs, rhs); };

  sn::sortshuffle::par::bitonic_sort<typename table_types<Key, BlockSize>::op_block>(
      blocks.data(), blocks.size(), st.workers, key_extractor, comparator
  );
#if defined(ORAM_DEBUG)
  st.debug_dump_blocks(blocks, pfm::format("o2th.placement[%s]:after_sort", level_name));
#endif

  // keep overflow marking in a single constant-time pass
  // scan: for each element that isn't in the first Z for its bin, mark as excess
  auto mark_primary_bins = [&]() noexcept {
    std::size_t curr_bin_fullness = 0;
    typename table_types<Key, BlockSize>::bucket_index curr_bin_id = 0;
    for (std::size_t ix = 0; ix < blocks.size(); ++ix) {
      auto& block = blocks[ix];
      const typename table_types<Key, BlockSize>::bucket_index bin = allow_overflow ? block.tag_l1 : block.tag_l2;
      const sn::obliv::choice next_segment(sn::obliv::ct_gt(bin, curr_bin_id));
      curr_bin_fullness = sn::obliv::ct_select<std::size_t>(0, curr_bin_fullness, next_segment.unwrap());
      curr_bin_id = sn::obliv::ct_select<typename table_types<Key, BlockSize>::bucket_index>(
          bin, curr_bin_id, next_segment.unwrap()
      );
      curr_bin_fullness += 1;

      const sn::obliv::choice overfull(sn::obliv::ct_gt(curr_bin_fullness, st.cfg.bucket_size));
      block.flags = table_types<Key, BlockSize>::set_flag_cond(
          block.flags, table_types<Key, BlockSize>::block_flag::excess, overfull
      );
      st.compact_marks[ix] = static_cast<std::uint8_t>(!overfull.unwrap());
    }
  };

  mark_primary_bins();
#if defined(ORAM_DEBUG)
  st.debug_dump_blocks(blocks, pfm::format("o2th.placement[%s]:after_mark", level_name));
#endif

  // compact: order-preserving compaction to put all non-excess elements before excess elements
  sn::sortshuffle::ser::orshuffle::orcompact(
      blocks.data(), blocks.size(), st.compact_marks.data(), st.compact_prefix.data()
  );
#if defined(ORAM_DEBUG)
  st.debug_dump_blocks(blocks, pfm::format("o2th.placement[%s]:after_compact_primary", level_name));
#endif

  // overflow is only allowed in level 1's placement
  if (allow_overflow) {
    const std::size_t excess_start = st.bucket_block_count;
#if defined(ORAM_DEBUG)
    std::size_t real_overflow = 0;
#endif
    for (std::size_t ix = excess_start; ix < blocks.size(); ++ix) {
      auto& block = blocks[ix];
      const sn::obliv::choice is_real = block.is_real();
      st.compact_marks[ix] = static_cast<std::uint8_t>(is_real.unwrap());
      block.flags = table_types<Key, BlockSize>::clear_flag_cond(
          block.flags, table_types<Key, BlockSize>::block_flag::excess, is_real
      );
#if defined(ORAM_DEBUG)
      real_overflow += static_cast<std::size_t>(is_real.unwrap());
#endif
    }

    // compact: compaction in the overflow section to put all real excess before dummy excess
    // verify: check bounds, ensure no real blocks beyond bucket capacity + overflow reserve
    sn::sortshuffle::ser::orshuffle::orcompact(
        blocks.data() + static_cast<std::ptrdiff_t>(excess_start), blocks.size() - excess_start,
        st.compact_marks.data() + excess_start, st.compact_prefix.data() + excess_start
    );
#if defined(ORAM_DEBUG)
    st.log.dbgf("o2th.placement[%s]: real overflow=%zu", level_name, real_overflow);
    st.debug_dump_blocks(blocks, pfm::format("o2th.placement[%s]:after_overflow_compact", level_name));
#endif
  }
#if defined(ORAM_DEBUG)
  else {
    const std::size_t excess_start = st.bucket_block_count;
    for (std::size_t ix = excess_start; ix < blocks.size(); ++ix) {
      sn::util::log::ensure(!blocks[ix].is_real().unwrap(), "o2th_rwkv: real block leaked into overflow");
    }
    st.debug_dump_blocks(blocks, pfm::format("o2th.placement[%s]:final", level_name));
  }
#endif
}

template <typename Key, std::size_t BlockSize>
inline void build(
    state<Key, BlockSize>& st,
    sn::util::span<
        typename table_types<Key, BlockSize>::template maybe_dummy<typename table_types<Key, BlockSize>::op_request>>
        in_data
) {
  sn_prof_zone("o2th.build");
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(in_data.size() == st.cfg.block_count, "o2th_rwkv: build input size must match block_count");
  st.log.trcf("o2th.build: constructing hashtable for n=%zu requests", in_data.size());
#endif

  // rekey all prfs for this build epoch
  reseed_epoch<Key, BlockSize>(st);

  // populate level 1 blocks from in data, then add filler
  populate_level_one<Key, BlockSize>(st, in_data);
  populate_fillers<Key, BlockSize>(st);

  // run placement on first level, allowing overflow
#if defined(ORAM_DEBUG)
  st.log.dbgf("o2th.build: running placement for level 1 (allow overflow)");
#endif
  placement<Key, BlockSize>(st, st.level1_blocks, true);

  // copy the excess blocks to the second level
  copy_overflow_to_level_two<Key, BlockSize>(st);

  // run placement on second level, with no overflow allowed
#if defined(ORAM_DEBUG)
  st.log.dbgf("o2th.build: running placement for level 2 (no overflow)");
#endif
  placement<Key, BlockSize>(st, st.level2_blocks, false);

#if defined(ORAM_DEBUG)
  st.log.dbgf("o2th.build: completed placement across both levels");
  st.debug_dump_blocks(st.level1_blocks, "o2th.build:level1");
  st.debug_dump_blocks(st.level2_blocks, "o2th.build:level2");
#endif
}

template <typename Key, std::size_t BlockSize>
inline void retrieve(
    state<Key, BlockSize>& st,
    sn::util::span<
        typename table_types<Key, BlockSize>::template maybe_dummy<typename table_types<Key, BlockSize>::op_request>>
        out_data,
    sn::util::span<std::uint8_t> compact_marks, sn::util::span<std::size_t> compact_prefix
) {
  sn_prof_zone("o2th.retrieve");

  // expected size of out data buffer
  const std::size_t retrieve_count = st.bucket_block_count * 2;

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(out_data.size() == retrieve_count, "o2th_rwkv: retrieve output span must be 2 * block_count");
  sn::util::log::ensure(compact_marks.size() == retrieve_count, "o2th_rwkv: retrieve marks span size mismatch");
  sn::util::log::ensure(
      compact_prefix.size() == retrieve_count + 1, "o2th_rwkv: retrieve prefix span must be count + 1"
  );
  st.log.trcf("o2th.retrieve: exporting %zu blocks per level (total=%zu)", st.bucket_block_count, retrieve_count);
#endif

  // data is stored in both buffers; there are at most block_count items
  // blocks_l1 and blocks_l2 are already compacted to have all real data in the main section
  // however, since there are two of them, we will have to copy out and compact non-filler to be first
  // this will of course include some filler/junk data, which we will mark dummy

  // first, copy out all data from blocks_l1 and blocks_l2 to kvps in out_data
  for (std::size_t i = 0; i < st.bucket_block_count; ++i) {
    export_block<Key, BlockSize>(st.level1_blocks[i], out_data[i], compact_marks[i]);
  }
  for (std::size_t i = 0; i < st.bucket_block_count; ++i) {
    export_block<Key, BlockSize>(
        st.level2_blocks[i], out_data[st.bucket_block_count + i], compact_marks[st.bucket_block_count + i]
    );
  }

  // run compaction to bring all non-dummy data to the front
  sn::sortshuffle::ser::orshuffle::orcompact(
      out_data.data(), out_data.size(), compact_marks.data(), compact_prefix.data()
  );
#if defined(ORAM_DEBUG)
  std::size_t real_count = 0;
  for (const auto& item : out_data) {
    if (!item.is_dummy) {
      ++real_count;
    }
  }
  st.log.dbgf("o2th.retrieve: compacted real blocks=%zu of %zu", real_count, retrieve_count);
#endif
}

} // namespace sn::omap::o2th
