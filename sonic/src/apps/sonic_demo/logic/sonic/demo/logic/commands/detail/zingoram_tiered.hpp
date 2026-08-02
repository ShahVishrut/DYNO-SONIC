#pragma once

#include <memory>
#include <string>

#include "sonic/oram/zingoram/options.hpp"
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/sgxbridge/fs/filesystem.hpp"
#include "sonic/storage/cache/stats.hpp"
#include "sonic/storage/io/cached_backend.hpp"
#if defined(SN_SGX_ENCLAVE)
#include "sonic/sgxbridge/storage/encrypted_file_backend.hpp"
#else
#include "sonic/storage/io/posix_file_backend.hpp"
#endif
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"

namespace sn::demo::logic::commands::detail {

template <typename Block> using tiered_store_default = sn::oram::zingoram::storage::tiered_store<Block>;

#if defined(SN_SGX_ENCLAVE)
struct sgx_cache_backend_factory {
  template <typename Block, typename CachePlan>
  static std::unique_ptr<sn::storage::io::backend> make(
      const sn::oram::zingoram::options& opts, const CachePlan& plan
  ) {
    sn::util::log::ensure(!opts.cache_path.empty(), "zingoram tiered: cache_path must be provided");
    sn::util::log::ensure(plan.page_bytes > 0, "zingoram tiered: cache page_bytes must be positive");

    sn::sgxbridge::storage::encrypted_file_backend::config cfg{};
    cfg.data_path = opts.cache_path;
    cfg.meta_path = opts.cache_path + ".meta";
    cfg.create = true;
    cfg.truncate = true;

    auto backend = std::make_unique<sn::sgxbridge::storage::encrypted_file_backend>(std::move(cfg));
    if (plan.expected_pages > 0) {
      backend->resize(plan.expected_pages, plan.page_bytes);
    }
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
#else
struct posix_cache_backend_factory {
  template <typename Block, typename CachePlan>
  static std::unique_ptr<sn::storage::io::backend> make(
      const sn::oram::zingoram::options& opts, const CachePlan& plan
  ) {
    sn::util::log::ensure(!opts.cache_path.empty(), "zingoram tiered: cache_path must be provided");
    sn::util::log::ensure(plan.page_bytes > 0, "zingoram tiered: cache page_bytes must be positive");

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

inline void log_cache_stats(
    const sn::storage::cache::stats_snapshot& s, const char* label, sn::util::log::logger& logger,
    std::uint64_t app_ops = 0
) {
  logger.inff("%s cache stats: %s", label, sn::storage::cache::format_stats(s, app_ops));
}

template <typename State> inline void log_cache_stats(State& state, const char* label, sn::util::log::logger& logger) {
  if (auto s = state.cache_stats_snapshot()) {
    const std::uint64_t app_ops = state.metrics_snapshot().total_path_ops();
    log_cache_stats(*s, label, logger, app_ops);
  }
}

template <typename State> inline void log_cache_stats(State& state, const char* label) {
  auto logger = sn::util::log::create("demo");
  log_cache_stats(state, label, logger);
}

inline void cleanup_cache_files(const std::string& path) {
  if (path.empty()) {
    return;
  }
  try {
    (void) sn::sgxbridge::fs::remove_file(path);
    (void) sn::sgxbridge::fs::remove_file(path + ".meta");
  } catch (...) {
  }
}

}
