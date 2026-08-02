#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/omap/pmchain/helpers.hpp"
#include "sonic/sortshuffle/par/bitonic.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::omap::pmchain {

template <typename O2THClient, typename OramClient>
inline void perform_oram_access(
    state<O2THClient, OramClient>& st, std::size_t slot, typename state<O2THClient, OramClient>::worker_state& worker
) noexcept {
  // entry contains the (addr, ctr) tuple for this slot
  const auto& entry = st.retrieve_buffer[slot];
  // decode tuple from entry
  const auto tuple = decode_addrctr_tuple<O2THClient, OramClient>(entry.value);
  auto data_buffer = st.request_buffer(slot);

  // issue access to logical oram adapter
  using oram_client_type = typename state<O2THClient, OramClient>::oram_client_type;
  typename oram_client_type::logical_access_request logical_req{};
  logical_req.address = static_cast<std::int64_t>(tuple.address);
  logical_req.counter = tuple.counter;
  logical_req.is_write = entry.value.is_write;
  logical_req.is_dummy = entry.is_dummy;
  logical_req.in = data_buffer;
  logical_req.out = data_buffer;
  st.oram.access(logical_req, worker.scratch);
}

template <typename O2THClient, typename OramClient> void initialize(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.initialize");
  st.posmap.initialize();
  st.oram.initialize();

  const std::uint64_t window = st.oram.options().disjoint_epoch_window;
  sn::util::log::ensure(window >= st.cfg.batch_size, "pmchain::client: disjoint epoch window < batch size");

  const auto& posmap_cfg = st.posmap.config_ref();
  const std::size_t build_capacity = posmap_cfg.block_count;
  sn::util::log::ensure(
      build_capacity == st.cfg.batch_size, "pmchain::client: posmap block count must equal batch size"
  );
  log_configuration(st, posmap_cfg, window);

  // posmap o2th: reqs is build set, data is query set
  st.posmap_reqs.assign(
      build_capacity,
      typename state<O2THClient, OramClient>::template maybe_dummy<typename state<O2THClient, OramClient>::req_type>{}
  );
  st.posmap_data.assign(st.cfg.block_count, typename state<O2THClient, OramClient>::data_query{});

  // buffers for o2th access
  st.pos_buf_l1.assign(st.cfg.block_count, 0);
  st.pos_buf_l2.assign(st.cfg.block_count, 0);

  // buffers for o2th retrieve
  const std::size_t retrieve_capacity = build_capacity * 2;
  st.retrieve_buffer.assign(
      retrieve_capacity,
      typename state<O2THClient, OramClient>::template maybe_dummy<typename state<O2THClient, OramClient>::req_type>{}
  );
  st.compact_marks.assign(retrieve_capacity, 0);
  st.compact_prefix.assign(retrieve_capacity + 1, 0);
  st.oram_buffers.assign(st.cfg.batch_size * st.cfg.oram_block_bytes, 0);

  initialize_worker_states(st);
  initialize_dataset(st);
  st.pending_flush = false;
  st.retrieval_ready = false;

  const std::size_t available = st.access_team.logical_threads();
  const std::size_t req_oram_workers = state<O2THClient, OramClient>::resolve_oram_parallelism(st.cfg, available);
  sn::util::log::ensure(req_oram_workers > 0, "pmchain::initialize: oram_parallelism must be positive");
}

template <typename O2THClient, typename OramClient>
void populate_requests(
    state<O2THClient, OramClient>& st, sn::util::span<const typename state<O2THClient, OramClient>::operation> ops
) {
  sn_prof_zone("pmchain.populate_requests");
  sn::util::log::ensure(!st.pending_flush, "pmchain::client: flush_pending must precede next batch");
  const std::size_t request_count = st.posmap_reqs.size();
#if defined(ORAM_DEBUG)
  st.log.trcf("pmchain.populate_requests: count=%zu", request_count);
#endif

  sn::util::log::ensure(request_count == st.cfg.batch_size, "pmchain::client: request buffer mismatch");
  sn::util::log::ensure(ops.size() == st.cfg.batch_size, "pmchain::client: operation count mismatch");
  for (std::size_t ix = 0; ix < st.cfg.batch_size; ++ix) {
    auto& request = st.posmap_reqs[ix];
    const auto& op = ops[ix];
    request.is_dummy = op.is_dummy;
    request.value.key = op.key;
    request.value.is_write = op.is_write;
    request.value.extra_data = op.extra_data;
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(op.key < st.cfg.block_count, "pmchain::client: key out of range");
#endif
  }
  st.retrieval_ready = false;
}

template <typename O2THClient, typename OramClient> void execute_o2th_chains(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.execute_o2th_chains");
  sn::util::log::ensure(!st.pending_flush, "pmchain::client: flush_pending must precede next batch");
  const std::size_t build_capacity = st.posmap_reqs.size();
#if defined(ORAM_DEBUG)
  st.log.trcf("pmchain.execute_o2th_chains: build=%zu", build_capacity);
#endif

  // build o2th on key requests
  st.posmap.build(
      sn::util::span<typename state<O2THClient, OramClient>::template maybe_dummy<
          typename state<O2THClient, OramClient>::req_type>>(st.posmap_reqs.data(), build_capacity)
  );

  // mutator for o2th posmap items
  auto mutator = [](std::uint8_t* dataset_bytes, std::uint8_t* op_bytes, sn::obliv::choice cond) {
    auto* dataset_words = reinterpret_cast<std::uint32_t*>(dataset_bytes);
    auto* op_words = reinterpret_cast<std::uint32_t*>(op_bytes);
    const std::uint32_t addr = dataset_words[0];
    const std::uint32_t ctr = dataset_words[1];
    const std::uint32_t next_ctr = ctr + 1U;

    sn::obliv::ct_set(&dataset_words[1], next_ctr, cond.unwrap());
    sn::obliv::ct_set(&op_words[0], addr, cond.unwrap());
    sn::obliv::ct_set(&op_words[1], ctr, cond.unwrap());
  };

  // access o2th with posmap data as query set
  st.posmap.access_batch(
      sn::util::span<typename state<O2THClient, OramClient>::data_query>(st.posmap_data.data(), st.posmap_data.size()),
      sn::util::span<typename state<O2THClient, OramClient>::bucket_index>(st.pos_buf_l1.data(), st.pos_buf_l1.size()),
      sn::util::span<typename state<O2THClient, OramClient>::bucket_index>(st.pos_buf_l2.data(), st.pos_buf_l2.size()),
      mutator
  );

  // retrieve populated key requests
  st.posmap.retrieve(
      sn::util::span<typename state<O2THClient, OramClient>::template maybe_dummy<
          typename state<O2THClient, OramClient>::req_type>>(st.retrieve_buffer.data(), st.retrieve_buffer.size()),
      sn::util::span<std::uint8_t>(st.compact_marks.data(), st.compact_marks.size()),
      sn::util::span<std::size_t>(st.compact_prefix.data(), st.compact_prefix.size())
  );

  st.retrieval_ready = true;
}

template <typename O2THClient, typename OramClient> void sort_o2th_chains(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.sort_o2th_chains");
  sn::util::log::ensure(st.retrieval_ready, "pmchain::client: o2th chains must run before sort");
  sn::util::log::ensure(!st.pending_flush, "pmchain::client: flush_pending must precede next batch");
  const std::size_t op_count = st.cfg.batch_size;
#if defined(ORAM_DEBUG)
  st.log.trcf("pmchain.sort_o2th_chains: count=%zu", op_count);
#endif
  sn::util::log::ensure(st.retrieve_buffer.size() >= op_count, "pmchain::client: retrieve buffer underrun");

  struct sort_key {
    std::uint64_t packed = 0;
  };

  auto extractor = [](const auto& entry) noexcept {
    sort_key key{};
    const std::uint64_t dummy_bit = entry.is_dummy ? 1ULL : 0ULL;
    const std::uint64_t index = static_cast<std::uint64_t>(entry.value.extra_data);
    key.packed = (dummy_bit << 32U) | index;
    return key;
  };

  auto comparator = [](const sort_key& lhs, const sort_key& rhs) noexcept {
    return sn::obliv::ct_lt(lhs.packed, rhs.packed);
  };

  // sort retrieve buffer by (is_dummy, extra_data); to ensure original order before build
  sn::sortshuffle::par::bitonic_sort<
      typename state<O2THClient, OramClient>::template maybe_dummy<typename state<O2THClient, OramClient>::req_type>>(
      st.retrieve_buffer.data(), op_count, st.access_team, extractor, comparator
  );
}

template <typename O2THClient, typename OramClient> void execute_oram_queries(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.execute_oram_queries");
  sn::util::log::ensure(st.retrieval_ready, "pmchain::client: o2th chains must run before oram queries");
  sn::util::log::ensure(!st.pending_flush, "pmchain::client: flush_pending must precede next batch");
  const std::size_t op_count = st.cfg.batch_size;
#if defined(ORAM_DEBUG)
  st.log.trcf("pmchain.execute_oram_queries: count=%zu", op_count);
#endif

  std::atomic<std::size_t> next_slot{0};

  st.oram_team.parallel_work([&st, &next_slot, op_count](std::size_t logical_worker, std::size_t) noexcept {
    auto& worker = st.worker_states[logical_worker];
    while (true) {
      const std::size_t slot = next_slot.fetch_add(1, std::memory_order_relaxed);
      if (slot >= op_count) {
        break;
      }
      perform_oram_access<O2THClient, OramClient>(st, slot, worker);
    }
  });

  st.pending_flush = true;
}

template <typename O2THClient, typename OramClient> void flush_pending(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.flush_pending");
  // no flush pending
  if (!st.pending_flush) {
    return;
  }

  if (st.cfg.drop_epoch) {
    if constexpr (requires(OramClient& oram) { oram.drop_epoch(); }) {
      st.oram.drop_epoch();
    } else {
      sn::util::log::fail("pmchain.flush_pending: drop_epoch requested but oram client does not support drop_epoch()");
    }
  } else {
    st.oram.flush_epoch();
  }
  st.pending_flush = false;
}

template <typename O2THClient, typename OramClient> void shutdown(state<O2THClient, OramClient>&) {}

template <typename O2THClient, typename OramClient>
[[nodiscard]] sn::util::span<
    typename state<O2THClient, OramClient>::template maybe_dummy<typename state<O2THClient, OramClient>::req_type>>
retrieved_requests(state<O2THClient, OramClient>& st) {
  sn::util::log::ensure(st.retrieval_ready, "pmchain::client: retrieve results not available");
  return sn::util::span<
      typename state<O2THClient, OramClient>::template maybe_dummy<typename state<O2THClient, OramClient>::req_type>>(
      st.retrieve_buffer.data(), st.cfg.batch_size
  );
}

template <typename O2THClient, typename OramClient>
[[nodiscard]] sn::util::span<const typename state<O2THClient, OramClient>::template maybe_dummy<
    typename state<O2THClient, OramClient>::req_type>>
retrieved_requests(const state<O2THClient, OramClient>& st) {
  sn::util::log::ensure(st.retrieval_ready, "pmchain::client: retrieve results not available");
  return sn::util::span<const typename state<O2THClient, OramClient>::template maybe_dummy<
      typename state<O2THClient, OramClient>::req_type>>(st.retrieve_buffer.data(), st.cfg.batch_size);
}

} // namespace sn::omap::pmchain
