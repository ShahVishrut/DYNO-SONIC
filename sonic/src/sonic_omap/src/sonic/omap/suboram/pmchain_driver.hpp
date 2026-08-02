#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/ops/word_ops.hpp"
#include "sonic/omap/o2th/client.hpp"
#include "sonic/omap/pmchain/client.hpp"
#include "sonic/oram/adapter/direct_block.hpp"
#include "sonic/oram/adapter/split_block.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::suboram {

namespace pmchain {

constexpr std::size_t kPosmapEntryBytes = 8;
constexpr std::size_t kHashtableBucketSize = 64;

struct config {
  std::size_t block_count = 0;
  std::size_t batch_size = 0;
  std::size_t physical_block_count = 0;
  std::size_t physical_batch_size = 0;
  std::size_t split_factor = 1;
  std::size_t posmap_bucket_size = kHashtableBucketSize;
  std::uint32_t bucket_real_size = 0;
  std::uint32_t bucket_dummy_size = 0;
  std::uint32_t eviction_rate = 0;
  std::uint32_t routing_depth = 0;
  std::uint32_t evict_batch = 1;
  std::uint32_t access_concurrency = 1;
  std::uint32_t oram_parallelism = 0;
  std::uint64_t disjoint_epoch_window = 0;
  std::uint64_t logical_disjoint_epoch_window = 0;
  std::size_t eviction_threads = 1;
  bool drop_epoch = false;
  bool tiered = false;
  std::uint64_t hot_memory_budget_bytes = 0;
  std::uint64_t cache_memory_budget_bytes = 0;
  std::uint64_t backend_cache_budget_bytes = 0;
  std::uint32_t cache_pack_factor = 1;
  std::string cache_path{};
};

template <
    typename Key, std::size_t LogicalBytes,
    typename Traits = sn::oram::zingoram::traits<LogicalBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch>>
class driver {
public:
  using key_type = Key;
  static constexpr std::size_t logical_block_bytes = LogicalBytes;
  static constexpr std::size_t payload_bytes = logical_block_bytes;
  using oram_traits = Traits;
  static constexpr std::size_t physical_block_bytes = oram_traits::block_bytes;
  static_assert(payload_bytes % 8 == 0, "payload size must be multiple of 8 bytes");
  static_assert(physical_block_bytes > 0, "pmchain_driver: physical block bytes must be positive");
  static_assert(
      logical_block_bytes % physical_block_bytes == 0, "pmchain_driver: logical block bytes must align with physical"
  );
  static_assert(std::is_integral_v<key_type>, "pmchain_driver: key_type must be integral");

  using posmap_type = sn::omap::o2th::o2th_rwkv<key_type, kPosmapEntryBytes>;
  using backing_oram_type = sn::oram::zingoram::client<oram_traits>;
  static constexpr std::size_t split_factor = logical_block_bytes / physical_block_bytes;
  using oram_client_type = std::conditional_t<
      split_factor == 1, sn::oram::adapter::direct_block<backing_oram_type>,
      sn::oram::adapter::split_block<backing_oram_type, split_factor>>;
  using pmchain_type = sn::omap::pmchain::client<posmap_type, oram_client_type>;
  using operation = typename pmchain_type::operation;

  driver(const config& cfg, sn::threads::thread_team eviction_team, sn::threads::thread_team access_team) :
      cfg_(resolve_config(cfg)),
      posmap_(make_posmap_config(cfg_), sn::threads::thread_team(access_team.pool(), access_team.logical_threads())),
      backing_oram_(make_oram_options(cfg_), std::move(eviction_team)),
      oram_(backing_oram_, make_oram_client_options(cfg_)),
      chain_(make_chain_config(cfg_), posmap_, oram_, std::move(access_team)),
      ops_(cfg_.batch_size) {
    validate_resolved_config(cfg_);
    chain_.initialize();
  }

  [[nodiscard]] std::size_t batch_size() const noexcept { return ops_.size(); }
  [[nodiscard]] std::size_t block_count() const noexcept { return cfg_.block_count; }

  // chain request buffer
  [[nodiscard]] sn::util::span<std::uint8_t> payload_buffer(std::size_t slot) { return chain_.request_buffer(slot); }

  // chain retrieve buffer
  [[nodiscard]] sn::util::span<typename pmchain_type::template maybe_dummy<typename pmchain_type::req_type>>
  retrieved_requests() {
    return chain_.retrieved_requests();
  }

  template <typename RoutedSpan> void process_bin(RoutedSpan bin) {
    const std::size_t bin_size = bin.size();
    const std::size_t batch_size = cfg_.batch_size;
    sn::util::log::ensure(bin_size <= batch_size, "pmchain_driver: bin larger than batch");

    populate_ops_from_bin(bin, batch_size, bin_size);

    chain_.populate_requests(sn::util::span<const operation>(ops_.data(), ops_.size()));
    chain_.execute_o2th_chains();
    chain_.sort_o2th_chains();
    chain_.execute_oram_queries();

    const auto retrieved_chains = chain_.retrieved_requests();
    sn::util::log::ensure(retrieved_chains.size() == ops_.size(), "pmchain_driver: retrieved count mismatch");
    apply_results_to_bin(retrieved_chains, bin, bin_size);
  }

  void maintenance() {
    // eviction phase
    chain_.flush_pending();
  }

  void shutdown() { chain_.shutdown(); }

private:
  static typename oram_traits::options_t make_oram_options(const config& cfg) {
    typename oram_traits::options_t opts{};
    opts.block_count = cfg.physical_block_count == 0 ? cfg.block_count * split_factor : cfg.physical_block_count;
    opts.bucket_real_size = cfg.bucket_real_size;
    opts.bucket_dummy_size = cfg.bucket_dummy_size;
    opts.eviction_rate = cfg.eviction_rate;
    opts.routing_depth = cfg.routing_depth;
    opts.evict_batch = cfg.evict_batch;
    opts.access_concurrency = cfg.access_concurrency;
    opts.disjoint_epoch_window = cfg.disjoint_epoch_window;
    if (cfg.tiered) {
      opts.hot_memory_budget_bytes = cfg.hot_memory_budget_bytes;
      opts.cache_memory_budget_bytes = cfg.cache_memory_budget_bytes;
      opts.backend_cache_budget_bytes = cfg.backend_cache_budget_bytes;
      opts.cache_pack_factor = cfg.cache_pack_factor;
      opts.cache_path = cfg.cache_path;
    } else {
      opts.hot_memory_budget_bytes = 0;
      opts.cache_memory_budget_bytes = 0;
      opts.backend_cache_budget_bytes = 0;
      opts.cache_pack_factor = 1;
      opts.cache_path.clear();
    }
    return opts;
  }

  static sn::omap::pmchain::config make_chain_config(const config& cfg) {
    sn::omap::pmchain::config out{};
    out.block_count = cfg.block_count;
    out.oram_block_bytes = logical_block_bytes;
    out.batch_size = cfg.batch_size;
    out.oram_parallelism = cfg.oram_parallelism;
    out.drop_epoch = cfg.drop_epoch;
    return out;
  }

  static typename posmap_type::config make_posmap_config(const config& cfg) {
    return typename posmap_type::config{.block_count = cfg.batch_size, .bucket_size = cfg.posmap_bucket_size};
  }

  static typename oram_client_type::options_t make_oram_client_options(const config& cfg) {
    return typename oram_client_type::options_t{
        .block_count = cfg.block_count, .disjoint_epoch_window = cfg.logical_disjoint_epoch_window
    };
  }

  static config resolve_config(const config& in) {
    config out = in;
    if (out.posmap_bucket_size == 0) {
      out.posmap_bucket_size = kHashtableBucketSize;
    }
    out.split_factor = out.split_factor == 0 ? 1 : out.split_factor;
    sn::util::log::ensure(out.split_factor == split_factor, "pmchain_driver: split factor mismatch");
    if (out.physical_block_count == 0) {
      sn::util::log::ensure(
          out.block_count <= std::numeric_limits<std::size_t>::max() / out.split_factor,
          "pmchain_driver: physical block count overflow"
      );
      out.physical_block_count = out.block_count * out.split_factor;
    }
    if (out.physical_batch_size == 0) {
      sn::util::log::ensure(
          out.batch_size <= std::numeric_limits<std::size_t>::max() / out.split_factor,
          "pmchain_driver: physical batch size overflow"
      );
      out.physical_batch_size = out.batch_size * out.split_factor;
    }
    if (out.disjoint_epoch_window == 0) {
      out.disjoint_epoch_window = static_cast<std::uint64_t>(out.physical_batch_size);
    }
    if (out.logical_disjoint_epoch_window == 0) {
      if (out.split_factor == 1) {
        out.logical_disjoint_epoch_window = out.disjoint_epoch_window;
      } else {
        sn::util::log::ensure(
            out.disjoint_epoch_window % out.split_factor == 0,
            "pmchain_driver: physical disjoint window not divisible by split factor"
        );
        out.logical_disjoint_epoch_window = out.disjoint_epoch_window / out.split_factor;
      }
    }
    return out;
  }

  static void validate_resolved_config(const config& cfg) {
    sn::util::log::ensure(cfg.posmap_bucket_size == 64, "pmchain_driver: bucket_size must be 64");
    sn::util::log::ensure(cfg.block_count > 0, "pmchain_driver: block_count must be positive");
    sn::util::log::ensure(cfg.batch_size > 0, "pmchain_driver: batch_size must be positive");
    sn::util::log::ensure(cfg.physical_block_count > 0, "pmchain_driver: physical block_count must be positive");
    sn::util::log::ensure(cfg.physical_batch_size > 0, "pmchain_driver: physical batch_size must be positive");
    sn::util::log::ensure(
        cfg.batch_size <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
        "pmchain_driver: batch_size exceeds extra_data capacity"
    );
    sn::util::log::ensure(
        cfg.block_count <= static_cast<std::size_t>(std::numeric_limits<key_type>::max()),
        "pmchain_driver: block_count exceeds key space"
    );
    sn::util::log::ensure(
        cfg.disjoint_epoch_window >= static_cast<std::uint64_t>(cfg.physical_batch_size),
        "pmchain_driver: physical disjoint window smaller than batch"
    );
    sn::util::log::ensure(
        cfg.logical_disjoint_epoch_window >= static_cast<std::uint64_t>(cfg.batch_size),
        "pmchain_driver: logical disjoint window smaller than batch"
    );
  }

  template <typename RoutedSpan>
  void populate_ops_from_bin(RoutedSpan bin, std::size_t batch_size, std::size_t bin_size) {
    for (std::size_t ix = 0; ix < batch_size; ++ix) {
      auto& op = ops_[ix];
      auto payload = payload_buffer(ix);

      op.key = 0;
      op.is_write = false;
      op.is_dummy = true;
      op.extra_data = static_cast<std::uint32_t>(ix);

      if (ix >= bin_size) {
        continue;
      }

      const auto& routed = bin[ix];
      sn::obliv::choice real_slot(!routed.is_dummy);
      sn::obliv::choice write_slot(routed.item.is_write);
      sn::obliv::choice real_write_slot = real_slot && write_slot;
      const key_type routed_key = static_cast<key_type>(routed.item.key);

#if defined(ORAM_DEBUG)
      if (real_slot.unwrap()) {
        sn::util::log::ensure(
            static_cast<std::size_t>(routed.item.key) < cfg_.block_count, "pmchain_driver: request key out of range"
        );
      }
#endif

      op.is_dummy = routed.is_dummy;
      op.is_write = routed.item.is_write;
      op.key = sn::obliv::ct_select<key_type>(routed_key, op.key, real_slot.unwrap());
      sn::obliv::ct_set_words<payload_bytes>(payload.data(), routed.item.payload.data(), real_write_slot.unwrap());
    }
  }

  template <typename RetrievedSpan, typename RoutedSpan>
  void apply_results_to_bin(RetrievedSpan retrieved, RoutedSpan bin, std::size_t bin_size) {
    for (std::size_t ix = 0; ix < bin_size; ++ix) {
      auto& entry = retrieved[ix];
      auto payload = payload_buffer(ix);
      auto& routed = bin[ix];

      sn::obliv::choice real_slot(!entry.is_dummy);

      routed.is_dummy = entry.is_dummy;
      routed.item.is_write = entry.value.is_write;
      routed.item.key =
          sn::obliv::ct_select<key_type>(entry.value.key, static_cast<key_type>(routed.item.key), real_slot.unwrap());
      sn::obliv::ct_set_words<payload_bytes>(routed.item.payload.data(), payload.data(), real_slot.unwrap());
    }
  }

  config cfg_{};
  posmap_type posmap_;
  backing_oram_type backing_oram_;
  oram_client_type oram_;
  pmchain_type chain_;
  std::vector<operation> ops_;
};

} // namespace pmchain

} // namespace sn::omap::suboram
