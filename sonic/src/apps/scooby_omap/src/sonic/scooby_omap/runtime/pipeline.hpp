#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/runtime/host_buffer_pool.hpp"
#include "sonic/scooby_omap/runtime/metrics.hpp"

namespace sn::scooby::omap {

struct buffer_tag {
  const char* role{"role"};
  const char* channel{"channel"};
  std::uint32_t ordinal{0};
};

struct pipeline_slot {
  host_buffer_pool::buffer_type* buffer{nullptr};
  sn::util::future<void> completion{};
  bool in_use{false};
  buffer_tag tag{};
};

template <typename Metrics> class send_pipeline {
public:
  void initialize(host_buffer_pool& pool, std::size_t depth, const buffer_tag& base_tag) {
    sn::util::log::ensuref(depth > 0, "send_pipeline depth must be positive");
    pool_ = &pool;
    slots_.clear();
    slots_.reserve(depth);
    for (std::size_t i = 0; i < depth; ++i) {
      buffer_tag tag = base_tag;
      tag.ordinal = static_cast<std::uint32_t>(i);
      slots_.push_back(pipeline_slot{&pool.acquire(), sn::util::future<void>{}, false, tag});
    }
    next_index_ = 0;
  }

  void attach_metrics(Metrics* counters) { telemetry_ = counters; }

  pipeline_slot& next_slot() {
    sn::util::log::ensuref(!slots_.empty(), "send_pipeline is not initialized");
    pipeline_slot& slot = slots_[next_index_];
    if (slot.in_use) {
      const auto wait_start = sn::sgxbridge::time::steady_clock::now();
      slot.completion.get();
      slot.in_use = false;
      if (telemetry_ != nullptr) {
        const auto elapsed = sn::sgxbridge::time::since(wait_start);
        telemetry_->pipeline_wait.add(static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(elapsed)));
        ++telemetry_->pipeline_wait_events;
      }
    }
    return slot;
  }

  void submit(pipeline_slot& slot, int dest_rank, std::size_t used_bytes) {
    sn::util::log::ensuref(slot.buffer != nullptr, "scooby send");
    sn::util::log::ensuref(
        used_bytes <= pool_->slot_bytes(),
        "scooby send", used_bytes,
        pool_->slot_bytes(), slot.tag.role, slot.tag.channel, slot.tag.ordinal
    );
    auto writable = map_writable(*slot.buffer);
    sn::util::log::ensuref(
        writable.size() >= used_bytes, "scooby send", writable.size(), used_bytes
    );
    slot.completion = sn::sgxbridge::dist::async_send_bytes(
        dest_rank, sn::util::span<const std::uint8_t>(writable.data(), used_bytes)
    );
    slot.in_use = true;
    next_index_ = (next_index_ + 1) % slots_.size();
  }

  void drain() {
    for (auto& slot : slots_) {
      if (slot.in_use) {
        slot.completion.get();
        slot.in_use = false;
      }
    }
  }

  host_buffer_pool& pool() const { return *pool_; }

private:
  host_buffer_pool* pool_{nullptr};
  std::vector<pipeline_slot> slots_{};
  std::size_t next_index_{0};
  Metrics* telemetry_{nullptr};
};

}
