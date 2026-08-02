#pragma once

#include <vector>

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/oram/pathoram/state.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::oram::pathoram {

struct eviction_request {
  sn::util::span<const std::uint64_t> leaves;
};

// pathoram eviction schedule: evict each path that was accessed
class schedule {
public:
  explicit schedule(std::uint32_t evict_batch = 1) { configure(evict_batch); }

  void configure(std::uint32_t evict_batch) {
    sn::util::log::ensure(evict_batch > 0, "pathoram::schedule: evict_batch must be positive");
    buffer_.resize(evict_batch);
    batch_size_ = evict_batch;
    fill_count_ = 0;
    draining_ = false;
  }

  void record_access(std::uint64_t leaf_ix) {
    sn_prof_zone("pathoram.schedule.record");
    sn::util::log::ensure(!draining_, "pathoram::schedule: record_access while drain pending");
    sn::util::log::ensure(fill_count_ < batch_size_, "pathoram::schedule: record_access overflow");
    buffer_[fill_count_] = leaf_ix;
    if (++fill_count_ == batch_size_) {
      // full, wait for drain
      draining_ = true;
    }
  }

  bool needs_eviction() const noexcept { return draining_; }

  sn::util::span<const std::uint64_t> take_evict_leaves() {
    sn_prof_zone("pathoram.schedule.take");
    sn::util::log::ensure(draining_, "pathoram::schedule: take_evict_leaves with no pending batch");
    const std::uint32_t ready = fill_count_;
    sn::util::log::ensure(ready == batch_size_, "pathoram::schedule: partial batch drain");
    draining_ = false;
    fill_count_ = 0;
    return sn::util::span<const std::uint64_t>(buffer_.data(), ready);
  }

private:
  std::vector<std::uint64_t> buffer_;
  std::uint32_t batch_size_ = 1;
  std::uint32_t fill_count_ = 0;
  bool draining_ = false;
};

template <typename Traits>
void evict(state<Traits>& st, const eviction_request& req, typename state<Traits>::eviction_ctx& ctx) {
  sn_prof_zone("pathoram.evict");
#if defined(ORAM_DEBUG)
  st.log().trcf("evict: leaf_ixs=%s", sn::util::format::format_vec(req.leaves));
#endif

  sn::util::log::ensure(req.leaves.size() == 1, "pathoram::evict: batched eviction not implemented");

  auto& stash = st.stash();
  auto& storage = st.storage();
  auto& topology = st.topology();

  // evict leaf path to path buffer
  const std::uint64_t leaf_ix = req.leaves[0];
  stash.evict_to_path(leaf_ix, ctx.buffer);

  const auto path_view = ctx.buffer.view();
  const auto node_ids = path_view.node_ids();
  const std::uint64_t height = path_view.height();

  // write back from path buffer to tree
  for (std::uint64_t depth = 0; depth <= height; ++depth) {
    const std::uint64_t node_id = node_ids[depth];
    auto bucket_span = path_view.bucket_span(depth);
    auto& bucket = storage[node_id];

    sn::obliv::copy_n(bucket_span.data(), bucket_span.size(), bucket.slots.begin());
  }
}

} // namespace sn::oram::pathoram
