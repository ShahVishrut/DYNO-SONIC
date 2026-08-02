#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes> struct client_epoch_ctx {
  using request_slot_t = request_slot<key_type, PayloadBytes>;

  bool seen{false};
  std::uint32_t client_id{0};
  std::vector<request_slot_t> slots{};
};

template <std::size_t PayloadBytes> struct bin_epoch_ctx {
  using routed_slot_t = routed_slot<key_type, PayloadBytes>;

  bool seen{false};
  std::vector<routed_slot_t> slots{};
};

template <std::size_t PayloadBytes> struct epoch_ctx {
  epoch_ctx() = default;
  explicit epoch_ctx(const plan_config& plan) { initialize(plan); }

  void initialize(const plan_config& plan) {
    clients.assign(plan.client_count, client_epoch_ctx<PayloadBytes>{});
    for (auto& ctx : clients) {
      ctx.slots.assign(
          static_cast<std::size_t>(plan.layout.requests_per_client_batch),
          typename client_epoch_ctx<PayloadBytes>::request_slot_t{}
      );
    }
    bins.assign(static_cast<std::size_t>(plan.client_count) * plan.suboram_count, bin_epoch_ctx<PayloadBytes>{});
    for (auto& bin : bins) {
      bin.slots.assign(
          static_cast<std::size_t>(plan.layout.bin_capacity), typename bin_epoch_ctx<PayloadBytes>::routed_slot_t{}
      );
    }
  }

  void reset_clients() {
    for (auto& ctx : clients) {
      ctx.seen = false;
      ctx.client_id = 0;
    }
  }

  void reset_bins() {
    for (auto& bin : bins) {
      bin.seen = false;
    }
  }

  bin_epoch_ctx<PayloadBytes>& bin_ctx(std::uint32_t client_id, std::uint32_t suboram_id, const plan_config& plan) {
    const std::size_t ix = static_cast<std::size_t>(client_id) * plan.suboram_count + suboram_id;
    return bins[ix];
  }

  std::vector<client_epoch_ctx<PayloadBytes>> clients{};
  std::vector<bin_epoch_ctx<PayloadBytes>> bins{};
};

}
