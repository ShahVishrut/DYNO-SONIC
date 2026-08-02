#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/omap/lbrouter/types.hpp"
#include "sonic/omap/suboram/o2th_driver.hpp"
#include "sonic/omap/suboram/pmchain_driver.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::suboram {

template <std::size_t PayloadBytes> class suboram_backend {
public:
  using bin_slot_t = routed_slot<key_type, PayloadBytes>;

  virtual ~suboram_backend() = default;
  [[nodiscard]] virtual std::size_t bin_capacity() const noexcept = 0;
  virtual void process_bin(sn::util::span<bin_slot_t> bin) = 0;
  virtual void perform_maintenance() = 0;
  virtual void shutdown() {}
};

template <std::size_t PayloadBytes>
using driver_slot_type = typename sn::omap::lbrouter::router_types<key_type, PayloadBytes>::routed_slot;

template <std::size_t PayloadBytes>
inline void wire_to_driver_slot(const routed_slot<key_type, PayloadBytes>& src, driver_slot_type<PayloadBytes>& dst) {
  dst.source_index = src.source_index;
  dst.is_dummy = src.is_dummy();
  dst.item.suboram_index = src.suboram_index;
  dst.item.is_write = src.is_write();
  dst.item.key = src.key;
  sn::obliv::copy_n(src.payload.begin(), PayloadBytes, dst.item.payload.begin());
}

template <std::size_t PayloadBytes>
inline void driver_to_wire_slot(const driver_slot_type<PayloadBytes>& src, routed_slot<key_type, PayloadBytes>& dst) {
  dst.source_index = src.source_index;
  dst.suboram_index = src.item.suboram_index;
  dst.key = src.item.key;
  dst.flags = 0;
  dst.set_dummy(src.is_dummy);
  dst.set_write(src.item.is_write);
  sn::obliv::copy_n(src.item.payload.begin(), PayloadBytes, dst.payload.begin());
}

template <std::size_t PayloadBytes> class o2th_suboram_backend final : public suboram_backend<PayloadBytes> {
public:
  using bin_slot_t = typename suboram_backend<PayloadBytes>::bin_slot_t;
  using driver_type = sn::omap::suboram::o2th::driver<key_type, PayloadBytes>;

  o2th_suboram_backend(
      std::size_t bin_capacity, std::size_t padded_batch, std::size_t block_count, sn::threads::thread_team workers
  ) :
      capacity_(bin_capacity),
      padded_capacity_(padded_batch),
      driver_(
          sn::omap::suboram::o2th::config{
              .block_count = block_count,
              .batch_size = padded_batch,
              .bucket_size = k_o2th_bucket_size,
          },
          std::move(workers)
      ),
      staged_(padded_batch) {
    sn::util::log::ensuref(capacity_ > 0, "scooby-omap o2th backend requires positive capacity");
    sn::util::log::ensuref(driver_.batch_size() >= capacity_, "scooby-omap o2th batch smaller than bin capacity");
  }

  [[nodiscard]] std::size_t bin_capacity() const noexcept override { return capacity_; }

  void process_bin(sn::util::span<bin_slot_t> bin) override {
    sn::util::log::ensuref(bin.size() == capacity_, "scooby-omap o2th bin size mismatch");
    for (std::size_t ix = 0; ix < padded_capacity_; ++ix) {
      auto& slot = staged_[ix];
      if (ix < bin.size()) {
        wire_to_driver_slot(bin[ix], slot);
      } else {
        slot.source_index = static_cast<std::uint32_t>(ix);
        slot.is_dummy = true;
        slot.item.suboram_index = 0;
        slot.item.is_write = false;
        slot.item.key = invalid_key;
        slot.item.payload.fill(0);
      }
    }
    driver_.process_bin(sn::util::span<driver_slot_type<PayloadBytes>>(staged_.data(), staged_.size()));
    for (std::size_t ix = 0; ix < bin.size(); ++ix) {
      driver_to_wire_slot(staged_[ix], bin[ix]);
    }
  }

  void perform_maintenance() override { driver_.maintenance(); }
  void shutdown() override {}

private:
  std::size_t capacity_{0};
  std::size_t padded_capacity_{0};
  driver_type driver_;
  std::vector<driver_slot_type<PayloadBytes>> staged_{};
};

template <
    std::size_t PayloadBytes,
    typename Traits = sn::oram::zingoram::traits<PayloadBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch>>
class pmchain_suboram_backend final : public suboram_backend<PayloadBytes> {
public:
  using bin_slot_t = typename suboram_backend<PayloadBytes>::bin_slot_t;
  using driver_type = sn::omap::suboram::pmchain::driver<key_type, PayloadBytes, Traits>;

  pmchain_suboram_backend(
      std::size_t bin_capacity, const sn::omap::suboram::pmchain::config& cfg, sn::threads::thread_team eviction_team,
      sn::threads::thread_team access_team
  ) :
      capacity_(bin_capacity), driver_(cfg, std::move(eviction_team), std::move(access_team)), staged_(cfg.batch_size) {
    sn::util::log::ensuref(capacity_ > 0, "scooby-omap pmchain backend requires positive capacity");
    sn::util::log::ensuref(driver_.batch_size() >= capacity_, "scooby-omap pmchain batch smaller than bin capacity");
  }

  [[nodiscard]] std::size_t bin_capacity() const noexcept override { return capacity_; }

  void process_bin(sn::util::span<bin_slot_t> bin) override {
    sn::util::log::ensuref(bin.size() == capacity_, "scooby-omap pmchain bin size mismatch");
    const std::size_t padded = staged_.size();
    for (std::size_t ix = 0; ix < padded; ++ix) {
      auto& slot = staged_[ix];
      if (ix < bin.size()) {
        wire_to_driver_slot(bin[ix], slot);
      } else {
        slot.source_index = static_cast<std::uint32_t>(ix);
        slot.is_dummy = true;
        slot.item.suboram_index = 0;
        slot.item.is_write = false;
        slot.item.key = invalid_key;
        slot.item.payload.fill(0);
      }
    }
    driver_.process_bin(sn::util::span<driver_slot_type<PayloadBytes>>(staged_.data(), staged_.size()));
    for (std::size_t ix = 0; ix < bin.size(); ++ix) {
      driver_to_wire_slot(staged_[ix], bin[ix]);
    }
  }

  void perform_maintenance() override { driver_.maintenance(); }
  void shutdown() override { driver_.shutdown(); }

private:
  std::size_t capacity_{0};
  driver_type driver_;
  std::vector<driver_slot_type<PayloadBytes>> staged_{};
};
}
