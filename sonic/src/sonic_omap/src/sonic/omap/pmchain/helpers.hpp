#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sonic/omap/pmchain/state.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::omap::pmchain {

template <typename O2THClient, typename OramClient>
inline void log_configuration(
    const state<O2THClient, OramClient>& st,
    const typename state<O2THClient, OramClient>::o2th_client_type::config& pos_cfg, std::uint64_t window
) {
  st.log.inff(
      "pmchain::client: block_count=%zu block_bytes=%zu batch_size=%zu posmap_blocks=%zu posmap_bucket=%zu window=%llu "
      "workers=%zu",
      st.cfg.block_count, st.cfg.oram_block_bytes, st.cfg.batch_size, pos_cfg.block_count, pos_cfg.bucket_size,
      static_cast<unsigned long long>(window), st.access_team.logical_threads()
  );
}

template <typename O2THClient, typename OramClient>
inline void initialize_worker_states(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.initialize_worker_states");
  const std::size_t worker_count = st.access_team.logical_threads();
  sn::util::log::ensure(worker_count > 0, "pmchain::client: access worker count must be positive");
#if defined(ORAM_DEBUG)
  st.log.trcf("pmchain.initialize_worker_states: workers=%zu", worker_count);
#endif
  st.worker_states.clear();
  st.worker_states.reserve(worker_count);
  for (std::size_t ix = 0; ix < worker_count; ++ix) {
    st.worker_states.emplace_back();
  }
  for (auto& worker : st.worker_states) {
    st.oram.configure_access_scratch(worker.scratch);
  }
}

template <typename O2THClient, typename OramClient> inline void initialize_dataset(state<O2THClient, OramClient>& st) {
  sn_prof_zone("pmchain.initialize_dataset");
  // posmap dataset stores one entry per logical block id
  using data_query = typename state<O2THClient, OramClient>::data_query;
  for (std::size_t ix = 0; ix < st.posmap_data.size(); ++ix) {
    auto& dq = st.posmap_data[ix];
    dq.key = static_cast<typename state<O2THClient, OramClient>::key_type>(ix);
    // set initial (addr,ctr) tuple
    auto* words = reinterpret_cast<std::uint32_t*>(dq.data.data());
    words[0] = static_cast<std::uint32_t>(ix);
    words[1] = 0U;
    // all dq ops from the posmap dataset are mutate-reads
    dq.flags = static_cast<std::uint8_t>(data_query::flag::query_read);
  }
}

template <typename O2THClient, typename OramClient>
[[nodiscard]] inline typename state<O2THClient, OramClient>::types::addrctr_tuple decode_addrctr_tuple(
    const typename state<O2THClient, OramClient>::req_type& request
) noexcept {
  const auto* words = reinterpret_cast<const std::uint32_t*>(request.data.data());
  typename state<O2THClient, OramClient>::types::addrctr_tuple tup{};
  tup.address = words[0];
  tup.counter = words[1];
  return tup;
}

} // namespace sn::omap::pmchain
