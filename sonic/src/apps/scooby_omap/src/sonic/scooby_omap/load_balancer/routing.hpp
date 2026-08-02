#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "sonic/crypto/prf.hpp"
#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/ops/word_ops.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes> class routing_selector {
public:
  using request_slot_t = request_slot<key_type, PayloadBytes>;
  using routed_slot_t = routed_slot<key_type, PayloadBytes>;

  routing_selector(std::size_t suborams, sn::threads::thread_team worker) :
      worker_(std::move(worker)), suboram_count_(suborams) {
    sn::util::log::ensuref(suboram_count_ > 0, "routing_selector: suboram_count must be positive");
    const auto key = make_prf_key();
    const std::size_t logical_workers = worker_.logical_threads();
    prfs_.resize(std::max<std::size_t>(logical_workers, std::size_t{1}));
    prngs_.resize(std::max<std::size_t>(logical_workers, std::size_t{1}));
    for (auto& prf : prfs_) {
      prf.set_key(key);
    }
  }

  void assign(sn::util::span<const request_slot_t> requests, sn::util::span<routed_slot_t> routed) {
    sn::util::log::ensuref(requests.size() == routed.size(), "routing_selector size mismatch");
    const std::size_t total = requests.size();
    if (total == 0) {
      return;
    }
    const std::size_t workers = worker_.logical_threads();
    auto route_chunk = [&](std::size_t begin, std::size_t end, std::size_t worker_index) noexcept {
      auto& prf = worker_prf(worker_index);
      auto& prng = worker_prng(worker_index);
      for (std::size_t ix = begin; ix < end; ++ix) {
        const auto& src = requests[ix];
        auto& dst = routed[ix];
        copy_slot(src, dst);
        const std::uint32_t prf_bin = assign_with_prf(prf, src.key);
        const std::uint32_t rand_bin = assign_with_prng(prng);
        const sn::obliv::choice real_slot(!src.is_dummy());

        dst.suboram_index = sn::obliv::ct_select<std::uint32_t>(prf_bin, rand_bin, real_slot.unwrap());
      }
    };

    sn::util::log::ensuref(total >= workers, "routing_selector: slots=%zu workers=%zu", total, workers);
    worker_.parallel_work([&, total, workers](std::size_t logical, std::size_t worker_index) noexcept {
      auto [chunk_begin, chunk_end] = sn::threads::partition_evenly(logical, total, workers);
      route_chunk(chunk_begin, chunk_end, worker_index);
    });
  }

private:
  sn::threads::thread_team worker_;
  std::size_t suboram_count_{1};
  std::vector<sn::crypto::prf> prfs_{};
  std::vector<sn::crypto::buffered_prng<>> prngs_{};

  static sn::crypto::prf::key_type make_prf_key() {
    sn::crypto::prng prng;
    return sn::crypto::prf::generate_key(prng);
  }

  sn::crypto::prf& worker_prf(std::size_t worker_index) noexcept {
    const auto index = worker_index < prfs_.size() ? worker_index : prfs_.size() - 1;
    return prfs_[index];
  }

  sn::crypto::buffered_prng<>& worker_prng(std::size_t worker_index) noexcept {
    const auto index = worker_index < prngs_.size() ? worker_index : prngs_.size() - 1;
    return prngs_[index];
  }

  std::uint32_t assign_with_prf(sn::crypto::prf& prf, key_type key) noexcept {
    std::array<std::uint8_t, sizeof(key_type)> input{};
    sn::obliv::memcpy(input.data(), &key, sizeof(key_type));
    std::array<std::uint8_t, sn::crypto::prf::block_size> output{};
    prf.derive(
        sn::util::span<const std::uint8_t>(input.data(), input.size()),
        sn::util::span<std::uint8_t>(output.data(), output.size())
    );
    std::uint32_t value = 0;
    sn::obliv::memcpy(&value, output.data(), sizeof(value));
    return static_cast<std::uint32_t>(value % suboram_count_);
  }

  std::uint32_t assign_with_prng(sn::crypto::buffered_prng<>& prng) noexcept {
    return static_cast<std::uint32_t>(prng.random_u64(0, static_cast<std::uint64_t>(suboram_count_)));
  }
};

}
