#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <sstream>

#include "cli_common.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/omap/harness/o2th/benchmark.hpp"
#include "sonic/omap/harness/o2th/experiment.hpp"
#include "sonic/omap/harness/o2th/validate.hpp"
#include "sonic/omap/harness/pmchain/experiment.hpp"
#include "sonic/omap/harness/pmchain/validate.hpp"
#include "sonic/oram/adapter/direct_block.hpp"
#include "sonic/oram/adapter/split_block.hpp"
#include "sonic/omap/o2th/client.hpp"
#include "sonic/omap/pmchain/client.hpp"
#include "sonic/omap/pmchain/threading.hpp"
#include "sonic/oram/zingoram/analysis.hpp"
#include "sonic/util/demo/block_size_dispatch.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/storage/cache/stats.hpp"
#include "sonic/util/cli/thread_options.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/util/ext/args.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/demo/zingoram_tiered.hpp"
#include "sonic/omap/pmchain/util/zingoram_setup.hpp"

namespace log_ns = sn::util::log;
namespace cli = sn::demo::cli;

namespace {

constexpr std::size_t kDefaultBlockBytes = 64;
constexpr std::size_t kPosmapEntryBytes = 8;

enum class action_kind { validate, experiment, benchmark };

action_kind parse_action(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (lowered == "validate") {
    return action_kind::validate;
  }
  if (lowered == "experiment") {
    return action_kind::experiment;
  }
  if (lowered == "benchmark") {
    return action_kind::benchmark;
  }
  throw args::Error("unknown action");
}

const char* action_label(action_kind action) noexcept {
  switch (action) {
  case action_kind::validate:
    return "validate";
  case action_kind::experiment:
    return "experiment";
  case action_kind::benchmark:
    return "benchmark";
  }
  return "unknown";
}

struct o2th_rwkv_cli_options {
  action_kind action = action_kind::validate;
  std::size_t dataset_size = 0;
  std::size_t request_count = 0;
  std::size_t block_size = kDefaultBlockBytes;
  std::size_t bucket_size = 0;
  std::size_t worker_parallelism = 0;
  std::size_t access_concurrency = 0;
  std::size_t batch_count = 1;
  double write_ratio = 0.5;
};

struct pmchain_cli_options {
  action_kind action = action_kind::validate;
  std::size_t block_count = 0;
  std::size_t batch_size = 0;
  std::size_t block_size = kDefaultBlockBytes;
  std::size_t split_factor = 1;
  std::size_t posmap_bucket_size = 0;
  std::uint32_t bucket_real_size = 0;
  std::uint32_t bucket_dummy_size = 0;
  std::uint32_t routing_depth = 0;
  std::uint32_t evict_batch = 1;
  bool drop_epoch = false;
  bool tiered = false;
  std::uint64_t hot_budget_bytes = 0;
  std::uint64_t cache_budget_bytes = 0;
  std::uint64_t backend_cache_budget_bytes = 0;
  std::uint32_t cache_pack_factor = 1;
  std::string cache_path = "/tmp/sonic_oram_demo.dat";
  std::size_t access_workers = 0;
  std::size_t oram_parallelism = 0;
  std::size_t eviction_threads = 0;
  std::uint64_t disjoint_epoch_window = 0;
  std::size_t batch_count = 1;
  double write_ratio = 0.5;
  double dummy_ratio = 0.0;
};

template <std::size_t BlockSize>
void run_o2th_rwkv_impl(const o2th_rwkv_cli_options& options, const sn::threads::thread_context& threads) {
  if (options.bucket_size < 64) {
    throw args::Error("bucket size must be >= 64");
  }
  if (options.dataset_size % options.bucket_size != 0) {
    throw args::Error("dataset size must be divisible by bucket size");
  }

  const auto worker_parallel = sn::threads::resolve_parallelism(options.worker_parallelism);
  sn::threads::pthread_thread_pool workers(threads, worker_parallel.background, "o2th");
  using table_type = sn::omap::o2th::o2th_rwkv<std::uint32_t, BlockSize>;
  table_type table(
      typename table_type::config{.block_count = options.dataset_size, .bucket_size = options.bucket_size},
      sn::threads::thread_team(workers.pool(), worker_parallel.logical)
  );
  table.initialize();

  auto logger = log_ns::create("omap");
  const std::size_t background_workers = workers.worker_count();
  const std::size_t logical_workers = worker_parallel.logical;
  logger.inf(
      pfm::format(
          "o2th config: action=%s block_bytes=%zu dataset=%zu queries=%zu bucket=%zu worker_parallel=%zu worker_bg=%zu "
          "batches=%zu write_ratio=%.3f phases=%s",
          action_label(options.action), BlockSize, options.dataset_size, options.request_count, options.bucket_size,
          worker_parallel.logical, background_workers, options.batch_count, options.write_ratio, "access_batch,retrieve"
      )
  );

  auto log_phase = [&](std::string_view label, const sn::omap::harness::o2th::phase_metrics& metrics,
                       std::string_view indent = "  ") {
    if (metrics.has_work()) {
      logger.inf(
          pfm::format(
              "%s%s: ops=%zu time=%.6fs (%.2f op/s)", indent, label, metrics.operations, metrics.seconds,
              metrics.throughput_ops_per_sec()
          )
      );
      return;
    }
    logger.inf(pfm::format("%s%s: ops=%zu time=%.6fs", indent, label, metrics.operations, metrics.seconds));
  };

  const std::size_t iterations = std::max<std::size_t>(options.batch_count, static_cast<std::size_t>(1));

  switch (options.action) {
  case action_kind::validate: {
    for (std::size_t batch = 0; batch < iterations; ++batch) {
      sn::omap::harness::o2th::validate_options validate_opts{};
      validate_opts.iterations = 1;

      if (iterations > 1) {
        logger.inf("running batch " + std::to_string(batch + 1) + " of " + std::to_string(iterations));
      }

      const auto result = sn::omap::harness::o2th::validate(table, validate_opts);
      logger.inf(
          pfm::format(
              "  validate: access_one=%zu dummy=%zu batch=%zu", result.access_one_accesses, result.dummy_probe_accesses,
              result.batch_accesses
          )
      );
    }
    break;
  }
  case action_kind::experiment: {
    sn::omap::harness::o2th::experiment_options exp_opts{};
    exp_opts.iterations = iterations;
    exp_opts.workload.real_request_count = options.dataset_size;
    exp_opts.workload.dataset_queries = options.request_count;
    exp_opts.workload.write_ratio = options.write_ratio;

    sn::omap::harness::o2th::experiment(table, exp_opts);
    break;
  }
  case action_kind::benchmark: {
    sn::omap::harness::o2th::benchmark_options bench_opts{};
    bench_opts.iterations = iterations;
    bench_opts.workload.real_request_count = options.dataset_size;
    bench_opts.workload.dataset_queries = options.request_count;
    bench_opts.workload.write_ratio = options.write_ratio;

    const auto bench = sn::omap::harness::o2th::benchmark(table, bench_opts);
    logger.inf("benchmark summary:");
    log_phase("build", bench.build);
    log_phase("access_batch", bench.access_batch);
    log_phase("retrieve", bench.retrieve);
    break;
  }
  }
}

using o2th_supported_block_sizes = sn::util::demo::block_size_list<8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096>;
using pmchain_supported_block_sizes = sn::util::demo::block_size_list<16, 32, 64, 128, 256, 512, 1024, 2048, 4096>;
using pmchain_supported_split_factors = sn::util::demo::static_param_list<std::size_t, 2>;

struct pmchain_layout_input {
  std::size_t split_factor = 1;
  std::size_t logical_block_bytes = 0;
  std::size_t logical_block_count = 0;
  std::size_t logical_batch_size = 0;
  std::size_t physical_block_bytes = 0;
  std::size_t physical_block_count = 0;
  std::size_t physical_batch_size = 0;
  std::uint64_t logical_disjoint_window_request = 0;
  std::uint64_t physical_disjoint_window_request = 0;
};

struct pmchain_layout {
  std::size_t split_factor = 1;
  std::size_t logical_block_bytes = 0;
  std::size_t logical_block_count = 0;
  std::size_t logical_batch_size = 0;
  std::size_t physical_block_bytes = 0;
  std::size_t physical_block_count = 0;
  std::size_t physical_batch_size = 0;
  std::uint64_t logical_disjoint_window = 0;
  std::uint64_t physical_disjoint_window = 0;
};

template <std::size_t PhysicalBlockBytes>
pmchain_layout_input make_pmchain_layout_input(const pmchain_cli_options& options, std::size_t split_factor) {
  sn::util::log::ensure(
      PhysicalBlockBytes <= std::numeric_limits<std::size_t>::max() / split_factor, "pmchain: block size overflow"
  );
  const std::size_t logical_block_bytes = PhysicalBlockBytes * split_factor;
  sn::util::log::ensure(
      options.block_count <= std::numeric_limits<std::size_t>::max() / split_factor, "pmchain: block count overflow"
  );
  const std::size_t physical_block_count =
      split_factor == 1 ? options.block_count : (options.block_count * split_factor);
  sn::util::log::ensure(
      options.batch_size <= std::numeric_limits<std::size_t>::max() / split_factor, "pmchain: batch size overflow"
  );
  const std::size_t physical_batch_size = split_factor == 1 ? options.batch_size : (options.batch_size * split_factor);

  const std::uint64_t logical_disjoint_window_input = options.disjoint_epoch_window;
  std::uint64_t physical_disjoint_window = logical_disjoint_window_input;
  if (split_factor > 1) {
    sn::util::log::ensure(
        logical_disjoint_window_input <= std::numeric_limits<std::uint64_t>::max() / split_factor,
        "pmchain: disjoint_epoch_window overflow"
    );
    physical_disjoint_window = logical_disjoint_window_input * split_factor;
  }

  pmchain_layout_input layout{};
  layout.split_factor = split_factor;
  layout.logical_block_bytes = logical_block_bytes;
  layout.logical_block_count = options.block_count;
  layout.logical_batch_size = options.batch_size;
  layout.physical_block_bytes = PhysicalBlockBytes;
  layout.physical_block_count = physical_block_count;
  layout.physical_batch_size = physical_batch_size;
  layout.logical_disjoint_window_request = logical_disjoint_window_input;
  layout.physical_disjoint_window_request = physical_disjoint_window;
  return layout;
}

pmchain_layout finalize_pmchain_layout(const pmchain_layout_input& input, std::uint64_t physical_disjoint_window) {
  if (input.split_factor > 1) {
    sn::util::log::ensure(
        physical_disjoint_window % input.split_factor == 0, "pmchain: physical disjoint window not divisible by split"
    );
  }
  const std::uint64_t logical_disjoint_window =
      input.split_factor == 1 ? physical_disjoint_window : (physical_disjoint_window / input.split_factor);
  sn::util::log::ensure(
      logical_disjoint_window >= input.logical_batch_size,
      "pmchain: logical disjoint_epoch_window smaller than batch size"
  );

  pmchain_layout layout{};
  layout.split_factor = input.split_factor;
  layout.logical_block_bytes = input.logical_block_bytes;
  layout.logical_block_count = input.logical_block_count;
  layout.logical_batch_size = input.logical_batch_size;
  layout.physical_block_bytes = input.physical_block_bytes;
  layout.physical_block_count = input.physical_block_count;
  layout.physical_batch_size = input.physical_batch_size;
  layout.logical_disjoint_window = logical_disjoint_window;
  layout.physical_disjoint_window = physical_disjoint_window;
  return layout;
}

void log_pmchain_config(
    const pmchain_layout& layout, const pmchain_cli_options& options, const sn::omap::pmchain::threading& threads,
    log_ns::logger& logger
) {
  logger.inf(
      pfm::format(
          "pmchain config: logical_block_bytes=%zu physical_block_bytes=%zu split_factor=%zu block_count=%zu "
          "batch=%zu posmap_bucket=%zu window_phys=%llu window_logical=%llu domain_parallel=%zu domain_bg=%zu "
          "access_parallel=%zu access_bg=%zu oram_parallel=%zu evict_parallel=%zu evict_bg=%zu drop_epoch=%u "
          "write_ratio=%.3f dummy_ratio=%.3f",
          layout.logical_block_bytes, layout.physical_block_bytes, layout.split_factor, layout.logical_block_count,
          layout.logical_batch_size, options.posmap_bucket_size,
          static_cast<unsigned long long>(layout.physical_disjoint_window),
          static_cast<unsigned long long>(layout.logical_disjoint_window), threads.domain.logical,
          threads.domain.background, threads.access.logical, threads.access.background, threads.oram.logical,
          threads.eviction.logical, threads.eviction.background, options.drop_epoch ? 1u : 0u, options.write_ratio,
          options.dummy_ratio
      )
  );
}

void dispatch_o2th_rwkv(const o2th_rwkv_cli_options& options, const sn::threads::thread_context& threads) {
  const bool dispatched =
      sn::util::demo::dispatch_block_size<o2th_supported_block_sizes>(options.block_size, [&](auto size_tag) {
        constexpr std::size_t BlockSize = decltype(size_tag)::value;
        run_o2th_rwkv_impl<BlockSize>(options, threads);
      });
  if (!dispatched) {
    throw args::Error(
        "unsupported block size; supported sizes are " +
        sn::util::demo::format_supported_sizes(o2th_supported_block_sizes::values) + " bytes"
    );
  }
}

template <typename Traits>
void run_pmchain_with_traits(const pmchain_cli_options& options, const sn::threads::thread_context& threads) {
  using traits_t = Traits;
  constexpr std::size_t PhysicalBlockBytes = traits_t::block_bytes;

  if (options.block_count == 0) {
    throw args::Error("block count must be > 0");
  }
  if (options.batch_size == 0) {
    throw args::Error("batch size must be > 0");
  }
  if (options.posmap_bucket_size == 0) {
    throw args::Error("posmap bucket size must be > 0");
  }
  if (options.bucket_real_size == 0 || options.bucket_dummy_size == 0) {
    throw args::Error("bucket sizes must be > 0");
  }
  if (options.routing_depth >= 63) {
    throw args::Error("routing depth must be < 63");
  }
  const std::size_t split_factor = options.split_factor == 0 ? 1 : options.split_factor;
  if (split_factor != 1 && !sn::util::demo::is_supported_param(pmchain_supported_split_factors{}, split_factor)) {
    throw args::Error(
        "pmchain: unsupported split factor; supported factors are 1, " +
        sn::util::demo::format_supported_params(pmchain_supported_split_factors::values)
    );
  }
  sn::util::log::ensure(
      options.batch_size % options.posmap_bucket_size == 0, "pmchain: batch size must align with bucket size"
  );

  using posmap_type = sn::omap::o2th::o2th_rwkv<std::uint32_t, kPosmapEntryBytes>;
  static_assert(posmap_type::block_size == kPosmapEntryBytes, "pmchain: posmap block size mismatch");
  const auto layout_input = make_pmchain_layout_input<PhysicalBlockBytes>(options, split_factor);
  const std::size_t build_capacity = layout_input.logical_batch_size;

  using oram_client_type = sn::oram::zingoram::client<traits_t>;
  const std::uint32_t evict_batch = options.evict_batch == 0 ? 1 : options.evict_batch;
  const auto requested_access = sn::threads::resolve_parallelism(std::max<std::size_t>(options.access_workers, 1));
  const std::size_t eviction_threads = options.eviction_threads != 0
                                           ? options.eviction_threads
                                           : sn::oram::zingoram::fit_eviction_threads(
                                                 options.routing_depth, requested_access.logical
                                             );
  const auto thread_plan = sn::omap::pmchain::resolve_threading(
      requested_access.logical, options.oram_parallelism, eviction_threads
  );
  const auto access_parallel = thread_plan.access;
  const auto oram_parallel = thread_plan.oram;
  const auto eviction_parallel = thread_plan.eviction;
  const std::uint32_t access_concurrency = static_cast<std::uint32_t>(access_parallel.logical);

  sn::omap::pmchain::util::zingoram_config_input zing_cfg{
      .block_count = layout_input.physical_block_count,
      .batch_size = layout_input.physical_batch_size,
      .bucket_real_size = options.bucket_real_size,
      .bucket_dummy_size = options.bucket_dummy_size,
      .routing_depth = options.routing_depth,
      .evict_batch = evict_batch,
      .access_concurrency = access_concurrency,
      .disjoint_epoch_window = layout_input.physical_disjoint_window_request,
  };
  auto zing_setup = sn::omap::pmchain::util::compute_zingoram_setup<traits_t>(zing_cfg, "pmchain");
  auto oram_opts = zing_setup.opts;
  const auto layout = finalize_pmchain_layout(layout_input, zing_setup.disjoint_window);

  constexpr bool is_tiered = std::is_base_of_v<
      sn::oram::zingoram::storage::tiered_store<typename traits_t::block_t>, typename traits_t::block_store_t>;
  if constexpr (is_tiered) {
    oram_opts.hot_memory_budget_bytes = options.hot_budget_bytes;
    oram_opts.cache_memory_budget_bytes = options.cache_budget_bytes;
    oram_opts.backend_cache_budget_bytes = options.backend_cache_budget_bytes;
    oram_opts.cache_pack_factor = options.cache_pack_factor;
    oram_opts.cache_path = options.cache_path;
  } else {
    oram_opts.hot_memory_budget_bytes = 0;
    oram_opts.cache_memory_budget_bytes = 0;
    oram_opts.backend_cache_budget_bytes = 0;
    oram_opts.cache_pack_factor = 1;
    oram_opts.cache_path.clear();
  }

  sn::threads::pthread_thread_pool pmchain_workers(threads, thread_plan.domain.background, "pmchain");

  posmap_type posmap(
      typename posmap_type::config{.block_count = build_capacity, .bucket_size = options.posmap_bucket_size},
      sn::threads::thread_team(pmchain_workers.pool(), access_parallel.logical)
  );

  sn::omap::pmchain::config chain_cfg{
      .block_count = layout.logical_block_count,
      .oram_block_bytes = layout.logical_block_bytes,
      .batch_size = layout.logical_batch_size,
      .oram_parallelism = oram_parallel.logical,
  };
  chain_cfg.drop_epoch = options.drop_epoch;

  auto logger = log_ns::create("pmchain");
  log_pmchain_config(layout, options, thread_plan, logger);

  auto run_chain = [&](auto& oram_client) {
    using oram_t = std::decay_t<decltype(oram_client)>;
    using pmchain_type = sn::omap::pmchain::client<posmap_type, oram_t>;
    chain_cfg.oram_parallelism = oram_parallel.logical;

    pmchain_type chain(
        chain_cfg, posmap, oram_client, sn::threads::thread_team(pmchain_workers.pool(), access_parallel.logical)
    );
    chain.initialize();

#if defined(SONIC_ORAM_TIERED_STORAGE)
    if constexpr (is_tiered) {
      auto& stats_state = [&]() -> auto& {
        if constexpr (requires { oram_client.backing_state_ref(); }) {
          return oram_client.backing_state_ref();
        } else {
          return oram_client.state_ref();
        }
      }();
      sn::util::demo::log_cache_stats(stats_state, "init", logger);
      stats_state.reset_cache_stats();
      if constexpr (requires { stats_state.reset_metrics(); }) {
        stats_state.reset_metrics();
      }
    }
#endif

    switch (options.action) {
    case action_kind::validate: {
      sn::omap::harness::pmchain::validate_options validate_opts{};
      validate_opts.batches = options.batch_count;
      validate_opts.write_ratio = options.write_ratio;
      validate_opts.dummy_ratio = options.dummy_ratio;
      sn::omap::harness::pmchain::validate(chain, validate_opts);
      break;
    }
    case action_kind::experiment: {
      sn::omap::harness::pmchain::experiment_options exp_opts{};
      exp_opts.batches = options.batch_count;
      exp_opts.write_ratio = options.write_ratio;
      sn::omap::harness::pmchain::experiment(chain, exp_opts);
      break;
    }
    case action_kind::benchmark:
      throw args::Error("benchmark mode unavailable for pmchain");
    }

#if defined(SONIC_ORAM_TIERED_STORAGE)
    if constexpr (is_tiered) {
      auto& stats_state = [&]() -> auto& {
        if constexpr (requires { oram_client.backing_state_ref(); }) {
          return oram_client.backing_state_ref();
        } else {
          return oram_client.state_ref();
        }
      }();
      sn::util::demo::log_cache_stats(stats_state, "access", logger);
      sn::util::demo::cleanup_cache_file(options.cache_path);
    }
#endif
  };

  if (layout.split_factor == 1) {
    oram_client_type backing_oram(
        oram_opts, sn::threads::thread_team(pmchain_workers.pool(), eviction_parallel.logical)
    );
    using direct_adapter = sn::oram::adapter::direct_block<oram_client_type>;
    typename direct_adapter::options_t logical_opts{
        .block_count = layout.logical_block_count, .disjoint_epoch_window = layout.logical_disjoint_window
    };
    direct_adapter oram_client(backing_oram, logical_opts);
    run_chain(oram_client);
  } else {
    const bool dispatched = sn::util::demo::dispatch_static_param<pmchain_supported_split_factors>(
        layout.split_factor, [&](auto split_tag) {
          constexpr std::size_t Split = decltype(split_tag)::value;
          using adapter_type = sn::oram::adapter::split_block<oram_client_type, Split>;
          typename adapter_type::options_t logical_opts{
              .block_count = layout.logical_block_count, .disjoint_epoch_window = layout.logical_disjoint_window
          };
          oram_client_type backing_oram(
              oram_opts, sn::threads::thread_team(pmchain_workers.pool(), eviction_parallel.logical)
          );
          adapter_type oram_client(backing_oram, logical_opts);
          run_chain(oram_client);
        }
    );
    if (!dispatched) {
      throw args::Error(
          "pmchain: unsupported split factor; supported factors are 1, " +
          sn::util::demo::format_supported_params(pmchain_supported_split_factors::values)
      );
    }
  }
}

void dispatch_pmchain(const pmchain_cli_options& options, const sn::threads::thread_context& threads) {
  const bool dispatched =
      sn::util::demo::dispatch_block_size<pmchain_supported_block_sizes>(options.block_size, [&](auto size_tag) {
        constexpr std::size_t BlockSize = decltype(size_tag)::value;
        using default_traits = sn::oram::zingoram::traits<BlockSize, sn::oram::zingoram::epoch_mode::disjoint_epoch>;
        if (!options.tiered) {
          run_pmchain_with_traits<default_traits>(options, threads);
          return;
        }
#if defined(SONIC_ORAM_TIERED_STORAGE)
        using tiered_traits = sn::oram::zingoram::traits<
            BlockSize, sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::tiered_store,
            sn::util::demo::posix_cache_backend_factory>;
        run_pmchain_with_traits<tiered_traits>(options, threads);
#else
        throw args::Error("tiered storage unavailable in this build");
#endif
      });
  if (!dispatched) {
    throw args::Error(
        "unsupported block size; supported sizes are " +
        sn::util::demo::format_supported_sizes(pmchain_supported_block_sizes::values) + " bytes"
    );
  }
}

// conservative avl height bound for capacity max_nodes

void run_o2th_rwkv(args::Subparser& parser, const sn::threads::thread_context& threads) {
  args::Positional<std::string> action_arg(parser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::size_t> dataset_size_flag(
      parser, "dataset-size", "dataset size", {'Y', "dataset-size"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> request_count_flag(parser, "request-count", "request count", {'M', "request-count"}, 0);
  args::ValueFlag<std::size_t> block_size_flag(
      parser, "block-bytes", "block size", {'B', "block-bytes"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> bucket_size_flag(parser, "bucket-size", "bucket size", {'Z', "bucket-size"}, 64);
  args::ValueFlag<std::size_t> worker_parallelism_flag(
      parser, "worker-parallelism", "workers", {'W', "worker-parallelism"}, 1
  );
  args::ValueFlag<std::size_t> access_concurrency_flag(
      parser, "access-concurrency", "access concurrency", {'C', "access-concurrency"}, 0
  );
  args::ValueFlag<std::size_t> batches_flag(parser, "batches", "batches", {'m', "batches"}, 1);
  args::ValueFlag<double> write_ratio_flag(parser, "write-ratio", "write ratio", {"write-ratio"}, 0.5);

  parser.Parse();

  o2th_rwkv_cli_options options{};
  options.action = parse_action(args::get(action_arg));
  options.dataset_size = args::get(dataset_size_flag);
  options.request_count = args::get(request_count_flag);
  if (options.request_count == 0) {
    options.request_count = options.dataset_size;
  }
  options.block_size = args::get(block_size_flag);
  options.bucket_size = args::get(bucket_size_flag);
  options.worker_parallelism = args::get(worker_parallelism_flag);
  options.access_concurrency = args::get(access_concurrency_flag);
  options.batch_count = args::get(batches_flag);
  if (options.batch_count == 0) {
    options.batch_count = 1;
  }
  options.write_ratio = args::get(write_ratio_flag);
  if (options.write_ratio < 0.0 || options.write_ratio > 1.0) {
    throw args::Error("write ratio out of range");
  }

  if (options.dataset_size == 0) {
    throw args::Error("dataset size must be > 0");
  }
  if (options.request_count == 0) {
    throw args::Error("request count must be > 0");
  }
  if (options.action == action_kind::validate && options.request_count != options.dataset_size) {
    throw args::Error("validate mode needs request-count to match dataset-size");
  }

  dispatch_o2th_rwkv(options, threads);
}

void run_pmchain(args::Subparser& parser, const sn::threads::thread_context& threads) {
  args::Positional<std::string> action_arg(parser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::size_t> block_count_flag(
      parser, "block-count", "block count", {'N', "block-count"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> batch_size_flag(
      parser, "batch-size", "batch size", {'m', "batch-size"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> block_bytes_flag(
      parser, "block-bytes", "block size", {'B', "block-bytes"}, kDefaultBlockBytes
  );
  args::ValueFlag<std::size_t> split_factor_flag(parser, "split-factor", "split factor", {"split-factor"}, 1);
  args::ValueFlag<std::size_t> posmap_bucket_flag(parser, "posmap-bucket", "posmap bucket", {"posmap-bucket"}, 64);
  args::ValueFlag<std::size_t> access_workers_flag(
      parser, "access-workers", "access workers", {'W', "access-workers"}, 1
  );
  args::ValueFlag<std::size_t> oram_parallel_flag(
      parser, "oram-parallelism", "oram workers; 0 for access", {"oram-parallelism"}, 0
  );
  args::ValueFlag<std::uint32_t> bucket_real_flag(
      parser, "bucket-real", "bucket real", {'Z', "bucket-real"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> bucket_dummy_flag(
      parser, "bucket-dummy", "bucket dummy", {'S', "bucket-dummy"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> routing_depth_flag(
      parser, "routing-depth", "routing depth", {'R', "routing-depth"}, 0
  );
  args::ValueFlag<std::uint32_t> evict_batch_flag(parser, "evict-batch", "evict batch", {"evict-batch"}, 1);
  args::ValueFlag<std::size_t> eviction_threads_flag(
      parser, "evict-threads", "evict threads; 0 for auto", {"evict-threads"}, 0
  );
  args::Flag tiered_flag(parser, "tiered-storage", "use tiered storage", {"tiered-storage"});
  args::ValueFlag<std::string> hot_budget_flag(parser, "hot-budget", "hot-tier budget", {"hot-budget"}, "0");
  args::ValueFlag<std::string> cache_budget_flag(parser, "cache-budget", "cache budget", {"cache-budget"}, "0");
  args::ValueFlag<std::string> backend_cache_budget_flag(
      parser, "backend-cache-budget", "backend cache budget", {"backend-cache-budget"}, "0"
  );
  args::ValueFlag<std::uint32_t> cache_pack_factor_flag(
      parser, "cache-pack-factor", "cache pack factor", {"cache-pack-factor"}, 1
  );
  args::ValueFlag<std::string> cache_path_flag(
      parser, "cache-path", "cache path", {"cache-path"}, "/tmp/sonic_oram_demo.dat"
  );
  args::ValueFlag<std::uint64_t> disjoint_epoch_flag(
      parser, "disjoint-epoch", "epoch window; 0 for auto", {"disjoint-epoch"}, 0
  );
  args::Flag drop_epoch_flag(parser, "drop-epoch", "drop pending epoch", {"drop-epoch"});
  args::ValueFlag<std::size_t> batches_flag(parser, "batches", "batches", {"batches"}, 1);
  args::ValueFlag<double> write_ratio_flag(parser, "write-ratio", "write ratio", {"write-ratio"}, 0.5);
  args::ValueFlag<double> dummy_ratio_flag(parser, "dummy-ratio", "dummy ratio", {"dummy-ratio"}, 0.0);

  parser.Parse();

  pmchain_cli_options options{};
  options.action = parse_action(args::get(action_arg));
  options.block_count = args::get(block_count_flag);
  options.batch_size = args::get(batch_size_flag);
  options.block_size = args::get(block_bytes_flag);
  options.split_factor = args::get(split_factor_flag);
  options.posmap_bucket_size = args::get(posmap_bucket_flag);
  options.access_workers = args::get(access_workers_flag);
  options.oram_parallelism = args::get(oram_parallel_flag);
  options.bucket_real_size = args::get(bucket_real_flag);
  options.bucket_dummy_size = args::get(bucket_dummy_flag);
  options.routing_depth = args::get(routing_depth_flag);
  options.evict_batch = args::get(evict_batch_flag);
  options.eviction_threads = args::get(eviction_threads_flag);
  options.tiered = tiered_flag.Get();
  options.hot_budget_bytes = sn::util::humanize::parse_bytes(args::get(hot_budget_flag));
  options.cache_budget_bytes = sn::util::humanize::parse_bytes(args::get(cache_budget_flag));
  options.backend_cache_budget_bytes = sn::util::humanize::parse_bytes(args::get(backend_cache_budget_flag));
  options.cache_pack_factor = args::get(cache_pack_factor_flag);
  options.cache_path = args::get(cache_path_flag);
  options.disjoint_epoch_window = args::get(disjoint_epoch_flag);
  options.drop_epoch = drop_epoch_flag.Get();
  options.batch_count = args::get(batches_flag);
  if (options.batch_count == 0) {
    options.batch_count = 1;
  }
  options.write_ratio = args::get(write_ratio_flag);
  if (options.write_ratio < 0.0 || options.write_ratio > 1.0) {
    throw args::Error("write ratio out of range");
  }
  options.dummy_ratio = args::get(dummy_ratio_flag);
  if (options.dummy_ratio < 0.0 || options.dummy_ratio > 1.0) {
    throw args::Error("dummy ratio out of range");
  }
  if (options.split_factor == 0) {
    throw args::Error("split factor must be > 0");
  }
  if (options.tiered) {
    if (options.cache_budget_bytes == 0) {
      throw args::Error("tiered runs need --cache-budget");
    }
    if (options.cache_pack_factor == 0) {
      throw args::Error("--cache-pack-factor must be > 0");
    }
    if (options.cache_path.empty()) {
      throw args::Error("tiered runs need --cache-path");
    }
  } else {
    options.hot_budget_bytes = 0;
    options.cache_budget_bytes = 0;
    options.backend_cache_budget_bytes = 0;
    options.cache_pack_factor = 1;
    options.cache_path.clear();
  }

  if (options.action == action_kind::benchmark) {
    throw args::Error("benchmark mode unavailable for pmchain");
  }

  dispatch_pmchain(options, threads);
}

} // namespace

int main(int argc, char** argv) {
  sn::prof::set_thread_name("main");
  args::ArgumentParser parser("sonic omap demo", "omap demo");
  parser.helpParams.showTerminator = false;
  parser.SetArgumentSeparations(false, false, true, true);
  parser.LongSeparator(" ");

  args::HelpFlag help(parser, "help", "show help", {'h', "help"});
  args::CounterFlag verbose(parser, "verbose", "more logging", {'v'});
  args::Flag quiet(parser, "quiet", "less logging", {'q'});
  sn::util::cli::thread_option_flags thread_flags(
      parser, sn::threads::thread_policy{.affinity = sn::threads::thread_affinity::dedicated}
  );

  args::Group commands(parser, "commands");
  auto with_logging = [&](auto&& fn) {
    return [&](args::Subparser& sub) {
      cli::apply_global_verbosity(verbose.Get(), quiet.Get());
      sn::threads::thread_context threads(thread_flags.resolve());
      threads.bind_current_thread();
      fn(sub, threads);
    };
  };

  args::Command o2th_rwkv_cmd(commands, "o2th-rwkv", "o2th rwkv", with_logging(run_o2th_rwkv));
  args::Command pmchain_cmd(commands, "pmchain", "pmchain", with_logging(run_pmchain));

  try {
    parser.ParseCLI(argc, argv);
  } catch (const args::Help&) {
    std::cout << parser;
    return 0;
  } catch (const args::Error& e) {
    std::cerr << e.what() << '\n';
    std::cerr << parser;
    return 1;
  }

  if (!o2th_rwkv_cmd && !pmchain_cmd) {
    std::cerr << "no command specified, use --help for usage" << '\n';
    return 1;
  }

  return 0;
}
