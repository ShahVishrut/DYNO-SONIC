#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

#include "sonic/sgxbridge/common/threadpool_support.hpp"
#if defined(SONIC_ORAM_TIERED_STORAGE)
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/oram/zingoram/options.hpp"
#include "sonic/storage/io/cached_backend.hpp"
#if defined(SN_SGX_ENCLAVE)
#include "sonic/sgxbridge/storage/encrypted_file_backend.hpp"
#else
#include "sonic/storage/io/posix_file_backend.hpp"
#endif
#endif
#include "sonic/threads/parallelism.hpp"
#include "sonic/util/demo/block_size_dispatch.hpp"

#include "sonic/omap/pmchain/threading.hpp"
#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/suboram/backend.hpp"

namespace sn::scooby::omap::suboram {

#if defined(SONIC_ORAM_TIERED_STORAGE)
template <typename Block> using tiered_store_default = sn::oram::zingoram::storage::tiered_store<Block>;

struct cache_backend_factory {
  template <typename Block, typename CachePlan>
  static std::unique_ptr<sn::storage::io::backend> make(
      const sn::oram::zingoram::options& opts, const CachePlan& plan
  ) {
    sn::util::log::ensure(!opts.cache_path.empty(), "zingoram tiered: cache_path must be provided");
    sn::util::log::ensure(plan.page_bytes > 0, "zingoram tiered: cache page_bytes must be positive");

#if defined(SN_SGX_ENCLAVE)
    sn::sgxbridge::storage::encrypted_file_backend::config cfg{};
    cfg.data_path = opts.cache_path;
    cfg.meta_path = opts.cache_path + ".meta";
    cfg.create = true;
    cfg.truncate = true;
    auto backend = std::make_unique<sn::sgxbridge::storage::encrypted_file_backend>(std::move(cfg));
#else
    sn::storage::io::posix_file_config cfg{};
    cfg.path = opts.cache_path;
    cfg.create = true;
    cfg.truncate = true;
    cfg.drop_cache_after_flush = false;
    cfg.allow_prefetch = true;
    auto backend = std::make_unique<sn::storage::io::posix_file_backend>(std::move(cfg));
    if (plan.expected_pages > 0) {
      backend->resize(plan.expected_pages, plan.page_bytes);
    }
#endif

    if (opts.backend_cache_budget_bytes == 0) {
      return backend;
    }

    sn::storage::io::cached_backend::config cbcfg{};
    cbcfg.page_bytes = plan.page_bytes;
    cbcfg.frame_bytes_budget = opts.backend_cache_budget_bytes;
    cbcfg.enable_prefetch = false;
    sn::util::log::ensure(
        cbcfg.frame_bytes_budget >= cbcfg.page_bytes, "zingoram tiered: backend cache budget must fit at least one page"
    );
    return std::make_unique<sn::storage::io::cached_backend>(cbcfg, std::move(backend));
  }
};
#endif

template <std::size_t PayloadBytes> struct state;

template <std::size_t PayloadBytes> inline bool initialize_o2th_backend(state<PayloadBytes>& s) {
  const std::uint32_t logical = std::max<std::uint32_t>(s.plan.o2th_parallelism, 1u);
  const std::uint32_t background = static_cast<std::uint32_t>(sn::threads::background_threads_for_parallelism(logical));
  auto req = sn::sgxbridge::tp::make_request(
      background, static_cast<std::uint32_t>(s.plan.o2th_batch_size), sn::sgxbridge::tp::queue_policy::block_when_full,
      "scooby-omap.o2th"
  );
  if (!sn::sgxbridge::tp::acquire_session(s.o2th_pool, s.ctx->threadpools, req, s.ctx->logger)) {
    return false;
  }
  if (s.o2th_pool.pool() == nullptr) {
    s.ctx->logger.err("scooby-omap o2th backend missing worker pool");
    return false;
  }
  s.backend = std::make_unique<o2th_suboram_backend<PayloadBytes>>(
      s.plan.layout.bin_capacity, s.plan.o2th_batch_size, s.plan.suboram_block_count,
      sn::threads::thread_team(*s.o2th_pool.pool(), logical)
  );
  s.ctx->logger.inf(
      pfm::format(
          "scooby-omap suboram using o2th backend padded_batch=%zu parallelism=%u", s.plan.o2th_batch_size,
          s.plan.o2th_parallelism
      )
  );
  return true;
}

template <std::size_t PayloadBytes> inline bool initialize_pmchain_backend(state<PayloadBytes>& s) {
  const auto thread_plan = sn::omap::pmchain::resolve_threading(
      s.plan.pmchain_access_parallelism, s.plan.pmchain_oram_parallelism, s.plan.pmchain_eviction_parallelism
  );
  auto req = sn::sgxbridge::tp::make_request(
      static_cast<std::uint32_t>(thread_plan.domain.background), static_cast<std::uint32_t>(s.plan.pmchain_batch_size),
      sn::sgxbridge::tp::queue_policy::block_when_full, "scooby-omap.pmchain"
  );
  if (!sn::sgxbridge::tp::acquire_session(s.pmchain_pool, s.ctx->threadpools, req, s.ctx->logger)) {
    return false;
  }
  if (s.pmchain_pool.pool() == nullptr) {
    s.ctx->logger.err("scooby-omap pmchain backend missing worker pool");
    return false;
  }
  auto* pool = s.pmchain_pool.pool();
  sn::omap::suboram::pmchain::config cfg{};
  cfg.block_count = s.plan.suboram_block_count;
  cfg.batch_size = s.plan.pmchain_batch_size;
  cfg.posmap_bucket_size = k_pmchain_bucket_size;
  cfg.bucket_real_size = s.plan.pmchain_bucket_real;
  cfg.bucket_dummy_size = s.plan.pmchain_bucket_dummy;
  cfg.eviction_rate = s.plan.pmchain_eviction_rate;
  cfg.routing_depth = s.plan.pmchain_routing_depth;
  cfg.evict_batch = s.plan.pmchain_evict_batch;
  cfg.access_concurrency = std::max<std::uint32_t>(s.plan.pmchain_access_parallelism, 1u);
  cfg.oram_parallelism = s.plan.pmchain_oram_parallelism;
  cfg.disjoint_epoch_window = s.plan.pmchain_physical_disjoint_window;
  cfg.logical_disjoint_epoch_window = s.plan.pmchain_disjoint_window;
  cfg.eviction_threads = s.plan.pmchain_eviction_parallelism;
  cfg.drop_epoch = s.plan.pmchain_drop_epoch;
  cfg.physical_block_count = s.plan.pmchain_physical_block_count;
  cfg.physical_batch_size = s.plan.pmchain_physical_batch_size;
  cfg.split_factor = s.plan.pmchain_split_factor;
  cfg.tiered = s.plan.pmchain_tiered;
  cfg.hot_memory_budget_bytes = s.plan.pmchain_hot_budget_bytes;
  cfg.cache_memory_budget_bytes = s.plan.pmchain_cache_budget_bytes;
  cfg.backend_cache_budget_bytes = s.plan.pmchain_backend_cache_budget_bytes;
  cfg.cache_pack_factor = s.plan.pmchain_cache_pack_factor;
  cfg.cache_path = s.plan.pmchain_cache_path;

  const std::size_t physical_block_bytes = s.plan.pmchain_physical_block_bytes;
  const auto validate_split = [&](std::size_t split) -> bool {
    if (split != s.plan.pmchain_split_factor) {
      s.ctx->logger.err(
          pfm::format(
              "scooby-omap pmchain split mismatch (requested=%zu derived=%zu)", s.plan.pmchain_split_factor, split
          )
      );
      return false;
    }
    if (split != 1 && !sn::util::demo::is_supported_param(pmchain_supported_split_factors{}, split)) {
      const auto supported = sn::util::demo::format_supported_params(pmchain_supported_split_factors::values);
      s.ctx->logger.err(
          pfm::format("scooby-omap pmchain unsupported split factor=%zu (supported: 1, %s)", split, supported)
      );
      return false;
    }
    return true;
  };

  const auto validate_tiered = [&]() -> bool {
    if (!cfg.tiered) {
      return true;
    }
#if !defined(SONIC_ORAM_TIERED_STORAGE)
    s.ctx->logger.err("scooby backend");
    return false;
#else
    if (cfg.cache_memory_budget_bytes == 0) {
      s.ctx->logger.err("scooby-omap pmchain tiered storage requires non-zero cache budget");
      return false;
    }
    if (cfg.cache_pack_factor == 0) {
      s.ctx->logger.err("scooby-omap pmchain tiered storage requires positive cache pack factor");
      return false;
    }
    return true;
#endif
  };

  const auto make_backend = [&](auto size_tag) -> bool {
    constexpr std::size_t PhysicalBytes = decltype(size_tag)::value;
    if constexpr ((PayloadBytes % PhysicalBytes) != 0) {
      return false;
    } else {
      const std::size_t split = PayloadBytes / PhysicalBytes;
      if (!validate_split(split) || !validate_tiered()) {
        return false;
      }

      using slab_traits = sn::oram::zingoram::traits<
          PhysicalBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::slab_store,
          sn::oram::zingoram::detail::null_cache_backend_factory>;
#if defined(SONIC_ORAM_TIERED_STORAGE)
      using tiered_traits = sn::oram::zingoram::traits<
          PhysicalBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch, tiered_store_default, cache_backend_factory>;
#endif

#if defined(SONIC_ORAM_TIERED_STORAGE)
      if (cfg.tiered) {
        s.backend = std::make_unique<pmchain_suboram_backend<PayloadBytes, tiered_traits>>(
            s.plan.layout.bin_capacity, cfg, sn::threads::thread_team(*pool, thread_plan.eviction.logical),
            sn::threads::thread_team(*pool, thread_plan.access.logical)
        );
        return true;
      }
#endif

      s.backend = std::make_unique<pmchain_suboram_backend<PayloadBytes, slab_traits>>(
          s.plan.layout.bin_capacity, cfg, sn::threads::thread_team(*pool, thread_plan.eviction.logical),
          sn::threads::thread_team(*pool, thread_plan.access.logical)
      );
      return true;
    }
  };

  bool backend_made = false;
  const bool dispatched =
      sn::util::demo::dispatch_block_size<pmchain_supported_block_sizes>(physical_block_bytes, [&](auto size_tag) {
        backend_made = make_backend(size_tag);
      });
  if (!dispatched) {
    const auto supported_sizes = sn::util::demo::format_supported_sizes(pmchain_supported_block_sizes::values);
    s.ctx->logger.err(
        pfm::format("scooby pmchain block size=%zu supported=%s", physical_block_bytes, supported_sizes)
    );
    return false;
  }
  if (!backend_made || !s.backend) {
    return false;
  }

  s.ctx->logger.inf(
      pfm::format(
          "scooby pmchain batch=%zu/%zu block=%zu split=%zu threads=%u/%u",
          s.plan.pmchain_batch_size, s.plan.pmchain_physical_batch_size, s.plan.pmchain_physical_block_bytes,
          s.plan.pmchain_split_factor, s.plan.pmchain_access_parallelism, s.plan.pmchain_eviction_parallelism
      )
  );
  return true;
}

template <std::size_t PayloadBytes> inline bool initialize_backend(state<PayloadBytes>& s) {
  switch (s.plan.backend) {
  case scooby_omap_backend::o2th:
    return initialize_o2th_backend(s);
  case scooby_omap_backend::pmchain:
    return initialize_pmchain_backend(s);
  }
  return false;
}

}
