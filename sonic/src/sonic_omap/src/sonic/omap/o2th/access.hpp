#pragma once

#include <type_traits>
#include <utility>

#include "sonic/omap/o2th/build.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/obliv/ops/readwrite.hpp"

namespace sn::omap::o2th {

// access a bucket, looking for an item with matching key and perform read/write
template <typename Key, std::size_t BlockSize, typename Mutator>
inline void scan_bucket(
    state<Key, BlockSize>& st, typename table_types<Key, BlockSize>::op_block* blocks,
    typename table_types<Key, BlockSize>::bucket_index bucket_pos, typename table_types<Key, BlockSize>::key_type key,
    std::uint8_t* item_data, sn::obliv::choice& found, Mutator&& mutator
) noexcept {
  auto bucket = st.bucket_view(blocks, bucket_pos);
#if defined(ORAM_DEBUG)
  if (st.debug_should_log(sn::util::log::level::debug)) {
    st.log.dbgf("o2th.scan_bucket: bucket=%u", static_cast<unsigned>(bucket_pos));
  }
#endif
  for (auto& block : bucket) {
    const sn::obliv::choice key_match(sn::obliv::ct_eq(block.key, key));
    const sn::obliv::choice real_match = key_match && block.is_real();
#if defined(ORAM_DEBUG)
    if (st.debug_should_log(sn::util::log::level::pedantic)) {
      const auto flags_str = state<Key, BlockSize>::debug_flags_to_string(block.flags);
      st.log.pedf(
          "  scan: key=%lld block_key=%lld flags=0x%02x (%s)", static_cast<long long>(key),
          static_cast<long long>(block.key), block.flags, flags_str
      );
    }
#endif

    if constexpr (std::is_same_v<std::decay_t<Mutator>, typename table_types<Key, BlockSize>::mutator_none>) {
      // standard read/write path
      // now we found a match between a build set block and a query set block
      // the data_query (item_data) is the queried data item from the dataset
      // the op_block (block) is a request from the query set
      // semantics:
      //   write request: write from op_block to data_query (op_block.data -> item_data)
      //   read request: read from data_query to op_block (item_data -> op_block.data)
      // if op block is write, then we need to write its data to the data query
      // if op block is read, then we need to copy the data query to the block
      const bool write_op = (real_match && block.is_op_write()).unwrap();
      const bool read_op = (real_match && block.is_op_read()).unwrap();
      // specialize kernels; avoid vector ovehead
      if constexpr (BlockSize == 8) {
        sn::obliv::ct_rw_words_aligned<8>(item_data, block.data.data(), read_op, write_op);
      } else if constexpr (BlockSize == 16) {
        sn::obliv::ct_rw_words_aligned<16>(item_data, block.data.data(), read_op, write_op);
      } else {
        sn::obliv::ct_set_words<BlockSize>(item_data, block.data.data(), write_op);
        sn::obliv::ct_set_words<BlockSize>(block.data.data(), item_data, read_op);
      }
#if defined(ORAM_DEBUG)
      if (st.debug_should_log(sn::util::log::level::debug) && key_match.unwrap()) {
        st.log.dbgf(
            "    scan: match (real=%s, read=%s, write=%s)", block.is_real().unwrap() ? "true" : "false",
            block.is_op_read().unwrap() ? "true" : "false", block.is_op_write().unwrap() ? "true" : "false"
        );
      }
#endif
    } else {
      // if was templated with mutator func, run mutator
      mutator(item_data, block.data.data(), real_match);
    }
    found = found || real_match;
  }
}

// match a single key (serial query)
template <typename Key, std::size_t BlockSize, typename Mutator = typename table_types<Key, BlockSize>::mutator_none>
inline bool access_one(
    state<Key, BlockSize>& st, typename table_types<Key, BlockSize>::key_type key, std::uint8_t* item_data,
    Mutator&& mutator = Mutator{}
) {
  sn_prof_zone("o2th.access_one");

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(sn::obliv::is_word_aligned(item_data), "o2th_rwkv: access_one item_data alignment");
  st.log.trcf("o2th.access_one: key=%llu", static_cast<unsigned long long>(key));
#endif

  auto& worker = st.serial_worker_state();
  auto&& active_mutator = std::forward<Mutator>(mutator);
  // prf the key and look up which bucket
  const typename table_types<Key, BlockSize>::bucket_index bucket_pos_l1 =
      derive_bucket_index<1, Key, BlockSize>(worker, key, st.bucket_count);
  const typename table_types<Key, BlockSize>::bucket_index bucket_pos_l2_target =
      derive_bucket_index<2, Key, BlockSize>(worker, key, st.bucket_count);
#if defined(ORAM_DEBUG)
  st.log.dbgf(
      "o2th.access_one: bucket_l1=%u, bucket_l2=%u", static_cast<unsigned>(bucket_pos_l1),
      static_cast<unsigned>(bucket_pos_l2_target)
  );
#endif

  // find the bucket in the first level
  sn::obliv::choice found = sn::obliv::choice::false_value();
  scan_bucket<Key, BlockSize>(st, st.level1_blocks.data(), bucket_pos_l1, key, item_data, found, active_mutator);

  // pick a random bucket in level 2
  const typename table_types<Key, BlockSize>::bucket_index random_bucket =
      static_cast<typename table_types<Key, BlockSize>::bucket_index>(
          worker.rng.random_u64(0, static_cast<std::uint64_t>(st.bucket_count))
      );

  // if found, then select a random pos instead in l2
  const typename table_types<Key, BlockSize>::bucket_index bucket_pos_l2 =
      sn::obliv::ct_select<typename table_types<Key, BlockSize>::bucket_index>(
          random_bucket, bucket_pos_l2_target, found.unwrap()
      );

  // perform access in the second level
  scan_bucket<Key, BlockSize>(st, st.level2_blocks.data(), bucket_pos_l2, key, item_data, found, active_mutator);
#if defined(ORAM_DEBUG)
  st.log.dbgf("o2th.access_one: result=%s", found.unwrap() ? "hit" : "miss");
#endif
  return found.unwrap();
}

// match multiple keys (parallel query)
// @param data query items
// @param pos_buf_l1 buffer for level 1 bucket positions
// @param pos_buf_l2 buffer for level 2 bucket positions
// @param mutator optional callable invoked for each scanned bucket slot
template <typename Key, std::size_t BlockSize, typename Mutator = typename table_types<Key, BlockSize>::mutator_none>
inline void access_batch(
    state<Key, BlockSize>& st, sn::util::span<typename table_types<Key, BlockSize>::data_query> queries,
    sn::util::span<typename table_types<Key, BlockSize>::bucket_index> pos_buf_l1,
    sn::util::span<typename table_types<Key, BlockSize>::bucket_index> pos_buf_l2, Mutator&& mutator = Mutator{}
) {
  sn_prof_zone("o2th.access_batch");

  const std::size_t query_count = queries.size();
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(pos_buf_l1.size() == query_count, "o2th_rwkv: pos_buf_l1 size mismatch");
  sn::util::log::ensure(pos_buf_l2.size() == query_count, "o2th_rwkv: pos_buf_l2 size mismatch");
  st.log.trcf("o2th.access_batch: query_count=%zu", query_count);
#endif

  if (query_count == 0) {
    return;
  }

  const std::size_t worker_count = st.worker_state_count();
  using mutator_type = std::decay_t<Mutator>;
  mutator_type mutator_obj(std::forward<Mutator>(mutator));
  auto* mutator_ptr = &mutator_obj;

  // stage 1: evaluate prfs for all keys
  // to derive bucket probe positions
  st.workers.parallel_work([&st, &queries, pos_buf_l1, pos_buf_l2, worker_count,
                            query_count](std::size_t worker_ix, std::size_t) noexcept {
    auto [begin, end] = sn::threads::partition_evenly(worker_ix, query_count, worker_count);
    if (begin >= end) {
      return;
    }
    auto& worker = st.worker_state_for_index(worker_ix);
    for (std::size_t ix = begin; ix < end; ++ix) {
      const auto& query = queries[ix];
      // compute bucket probe positions in each level
      pos_buf_l1[ix] = derive_bucket_index<1, Key, BlockSize>(worker, query.key, st.bucket_count);
      pos_buf_l2[ix] = derive_bucket_index<2, Key, BlockSize>(worker, query.key, st.bucket_count);
    }
  });

  // parallel partitioning of buckets
  // in order to allow lock-free parallel access, we partition as follows
  // we access all level 1 buckets in parallel, then all level 2 buckets in parallel
  // within each level, each thread only performs accesses to queries in a range of buckets
  // this way, writes can be performed without any synchronization
  // we also try to optimize for cache by reading consecutive pos_buf entries first

  // stage 2a: access all level 1 buckets in parallel
  st.workers.parallel_work([&st, &queries, pos_buf_l1, pos_buf_l2, worker_count, query_count,
                            mutator_ptr](std::size_t worker_ix, std::size_t) noexcept {
    // assigned bucket range
    auto [bucket_begin, bucket_end] = sn::threads::partition_evenly(worker_ix, st.bucket_count, worker_count);
    if (bucket_begin >= bucket_end) {
      return;
    }
    auto& worker = st.worker_state_for_index(worker_ix);
    auto& mutator_ref = *mutator_ptr;
    for (std::size_t ix = 0; ix < query_count; ++ix) {
      // process query if in our bucket range
      const auto bucket_pos_l1 = pos_buf_l1[ix];
      if (bucket_pos_l1 < bucket_begin || bucket_pos_l1 >= bucket_end) {
        continue;
      }

      auto& query = queries[ix];
      sn::obliv::choice found = sn::obliv::choice::false_value();
      scan_bucket<Key, BlockSize>(
          st, st.level1_blocks.data(), bucket_pos_l1, query.key, query.data.data(), found, mutator_ref
      );

      // conditionally set pos2 to random if found
      const auto random_bucket = static_cast<typename table_types<Key, BlockSize>::bucket_index>(
          worker.rng.random_u64(0, static_cast<std::uint64_t>(st.bucket_count))
      );
      pos_buf_l2[ix] = sn::obliv::ct_select<typename table_types<Key, BlockSize>::bucket_index>(
          random_bucket, pos_buf_l2[ix], found.unwrap()
      );

      // set ok flag if found
      query.set_ok_cond(found);
    }
  });

  // stage 2b: access all level 2 buckets in parallel
  st.workers.parallel_work([&st, &queries, pos_buf_l2, worker_count, query_count,
                            mutator_ptr](std::size_t worker_ix, std::size_t) noexcept {
    // assigned bucket range
    auto [bucket_begin, bucket_end] = sn::threads::partition_evenly(worker_ix, st.bucket_count, worker_count);
    if (bucket_begin >= bucket_end) {
      return;
    }
    auto& mutator_ref = *mutator_ptr;
    for (std::size_t ix = 0; ix < query_count; ++ix) {
      // process query if in our bucket range
      const auto bucket_pos_l2 = pos_buf_l2[ix];
      if (bucket_pos_l2 < bucket_begin || bucket_pos_l2 >= bucket_end) {
        continue;
      }

      auto& query = queries[ix];
      sn::obliv::choice found = sn::obliv::choice::false_value();
      scan_bucket<Key, BlockSize>(
          st, st.level2_blocks.data(), bucket_pos_l2, query.key, query.data.data(), found, mutator_ref
      );

      // set ok flag if found
      query.set_ok_cond(found);
    }
  });

#if defined(ORAM_DEBUG)
  std::size_t ok_count = 0;
  for (const auto& query : queries) {
    if (query.result_ok()) {
      ++ok_count;
    }
  }
  st.log.dbgf("o2th.access_batch: ok=%zu/%zu", ok_count, query_count);
#endif
}

} // namespace sn::omap::o2th
