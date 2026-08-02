#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sonic/demo/logic/commands/common.hpp"
#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/types/intents.hpp"
#include "sonic/omap/harness/o2th/benchmark.hpp"
#include "sonic/omap/harness/o2th/experiment.hpp"
#include "sonic/omap/harness/o2th/validate.hpp"
#include "sonic/omap/harness/pmchain/experiment.hpp"
#include "sonic/omap/harness/pmchain/validate.hpp"
#include "sonic/omap/o2th/client.hpp"
#include "sonic/omap/pmchain/client.hpp"
#include "sonic/omap/pmchain/threading.hpp"
#include "sonic/omap/pmchain/util/zingoram_setup.hpp"
#include "sonic/oram/harness/validate.hpp"
#include "sonic/oram/zingoram/analysis.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/oram/adapter/direct_block.hpp"
#include "sonic/oram/adapter/split_block.hpp"
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/demo/logic/commands/detail/zingoram_tiered.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/demo/block_size_dispatch.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"

namespace sn::demo::logic::commands::omap {
namespace {

constexpr std::size_t kPosmapEntryBytes = 8;
constexpr std::size_t kBlockBytes = 64;
using sn::demo::logic::commands::detail::cleanup_cache_files;
using sn::demo::logic::commands::detail::log_cache_stats;
using sn::demo::logic::commands::detail::tiered_store_default;
#if defined(SN_SGX_ENCLAVE)
using cache_backend_factory = sn::demo::logic::commands::detail::sgx_cache_backend_factory;
#else
using cache_backend_factory = sn::demo::logic::commands::detail::posix_cache_backend_factory;
#endif

template <std::size_t BlockSize>
types::command_result run_o2th_mode(const types::o2th_intent& intent, execution_context& ctx) {
  if (!ctx.threadpools.available()) {
    return detail::make_error(types::result_status::unsupported, "thread pool provider unavailable");
  }

  if (intent.bucket_size < 64) {
    return detail::make_error(types::result_status::invalid_arguments, "bucket size must be >= 64");
  }

  const auto dataset_u64 = intent.dataset_size;
  const auto request_u64 = intent.request_count;
  if (dataset_u64 == 0 || request_u64 == 0) {
    return detail::make_error(types::result_status::invalid_arguments, "dataset and request counts must be > 0");
  }
  if (dataset_u64 > std::numeric_limits<std::size_t>::max() || request_u64 > std::numeric_limits<std::size_t>::max()) {
    return detail::make_error(types::result_status::invalid_arguments, "dataset size exceeds supported limit");
  }
  const std::size_t dataset_size = static_cast<std::size_t>(dataset_u64);
  const std::size_t request_count = static_cast<std::size_t>(request_u64);

  const auto worker_parallel = sn::threads::resolve_parallelism(static_cast<std::size_t>(intent.worker_parallelism));
  const std::size_t requested_background = worker_parallel.background;
  sn::sgxbridge::tp::session worker_session;
  const auto worker_res = worker_session.open(
      ctx.threadpools, detail::make_threadpool_request(requested_background, "sonic_demo.o2th.workers")
  );
  if (!worker_res.succeeded() || worker_session.pool() == nullptr) {
    return detail::session_error("sonic_demo.o2th.workers", worker_res);
  }

  using table_type = sn::omap::o2th::o2th_rwkv<std::uint32_t, BlockSize>;
  typename table_type::config cfg{};
  cfg.block_count = dataset_size;
  cfg.bucket_size = static_cast<std::size_t>(intent.bucket_size);

  auto table_logger = sn::util::log::create("sonic_demo.o2th");
  table_type table(cfg, sn::threads::thread_team(worker_session.pool_ref(), worker_parallel.logical), table_logger);
  table.initialize();

  auto logger = sn::util::log::create("sonic_demo.o2th");
  const std::size_t logical_workers = worker_parallel.logical;
  const std::size_t batch_count = intent.batch_count == 0 ? 1u : intent.batch_count;
  logger.inff(
      "o2th config: action=%s block_bytes=%zu block_count=%zu bucket_size=%zu requests=%zu worker_parallel=%zu "
      "worker_bg=%zu batches=%zu write_ratio=%.3f phases=%s",
      detail::describe_action(intent.action), BlockSize, dataset_size, cfg.bucket_size, request_count,
      worker_parallel.logical, worker_parallel.background, batch_count, intent.write_ratio, "access_batch,retrieve"
  );

  const std::size_t iterations =
      intent.batch_count == 0 ? std::size_t{1} : static_cast<std::size_t>(intent.batch_count);

  switch (intent.action) {
  case types::oram_action::validate: {
    sn::omap::harness::o2th::validate_result last_stats{};
    for (std::size_t batch = 0; batch < iterations; ++batch) {
      sn::omap::harness::o2th::validate_options opts{};
      opts.iterations = 1;
      last_stats = sn::omap::harness::o2th::validate(table, opts);
      logger.inff(
          "validate[%zu/%zu]: access_one=%zu dummy=%zu batch=%zu", batch + 1, iterations,
          last_stats.access_one_accesses, last_stats.dummy_probe_accesses, last_stats.batch_accesses
      );
    }
    auto text = pfm::format("o2th validate ok: blocks=%zu requests=%zu", dataset_size, request_count);
    types::command_result out{};
    if (!out.output.assign(text)) {
      return detail::make_error(types::result_status::internal_error, "result output truncated");
    }
    out.status = types::result_status::ok;
    return out;
  }
  case types::oram_action::experiment: {
    sn::omap::harness::o2th::experiment_options opts{};
    opts.iterations = iterations;
    opts.workload.real_request_count = dataset_size;
    opts.workload.dataset_queries = request_count;
    opts.workload.write_ratio = intent.write_ratio;
    const auto result = sn::omap::harness::o2th::experiment(table, opts);
    auto text =
        pfm::format("o2th experiment ok: iterations=%zu queries=%zu", result.iterations, result.dataset_queries);
    types::command_result out{};
    if (!out.output.assign(text)) {
      return detail::make_error(types::result_status::internal_error, "result output truncated");
    }
    out.status = types::result_status::ok;
    return out;
  }
  case types::oram_action::benchmark: {
    sn::omap::harness::o2th::benchmark_options opts{};
    opts.iterations = iterations;
    opts.workload.real_request_count = dataset_size;
    opts.workload.dataset_queries = request_count;
    opts.workload.write_ratio = intent.write_ratio;
    const auto bench = sn::omap::harness::o2th::benchmark(table, opts);
    logger.inff(
        "benchmark summary: build=%.6fs access_batch=%.6fs retrieve=%.6fs", bench.build.seconds,
        bench.access_batch.seconds, bench.retrieve.seconds
    );
    types::command_result out{};
    if (!out.output.assign("o2th benchmark ok")) {
      return detail::make_error(types::result_status::internal_error, "result output truncated");
    }
    out.status = types::result_status::ok;
    return out;
  }
  }

  return detail::make_error(types::result_status::unsupported, "unknown o2th action");
}

using o2th_supported_block_sizes = sn::util::demo::block_size_list<8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096>;

inline types::command_result run_o2th(const types::o2th_intent& intent, execution_context& ctx) {
  types::command_result result;
  const bool dispatched =
      sn::util::demo::dispatch_block_size<o2th_supported_block_sizes>(intent.block_size, [&](auto size_tag) {
        constexpr std::size_t BlockSize = decltype(size_tag)::value;
        result = run_o2th_mode<BlockSize>(intent, ctx);
      });
  if (!dispatched) {
    return detail::make_error(types::result_status::invalid_arguments, "unsupported o2th block size");
  }
  return result;
}

using pmchain_supported_block_sizes = sn::util::demo::block_size_list<16, 32, 64, 128, 256, 512, 1024, 2048, 4096>;
using pmchain_supported_split_factors = sn::util::demo::static_param_list<std::size_t, 2>;

struct pmchain_layout_input {
  std::size_t split_factor{1};
  std::size_t logical_block_bytes{0};
  std::size_t logical_block_count{0};
  std::size_t logical_batch_size{0};
  std::size_t physical_block_bytes{0};
  std::size_t physical_block_count{0};
  std::size_t physical_batch_size{0};
  std::uint64_t logical_disjoint_window_request{0};
  std::uint64_t physical_disjoint_window_request{0};
};

struct pmchain_layout {
  std::size_t split_factor{1};
  std::size_t logical_block_bytes{0};
  std::size_t logical_block_count{0};
  std::size_t logical_batch_size{0};
  std::size_t physical_block_bytes{0};
  std::size_t physical_block_count{0};
  std::size_t physical_batch_size{0};
  std::uint64_t logical_disjoint_window{0};
  std::uint64_t physical_disjoint_window{0};
};

template <std::size_t PhysicalBlockBytes>
std::optional<pmchain_layout_input> make_pmchain_layout_input(
    const types::pmchain_intent& intent, std::size_t split_factor, types::command_result& error_out
) {
  const auto fail = [&](std::string_view msg) -> std::optional<pmchain_layout_input> {
    error_out = detail::make_error(types::result_status::invalid_arguments, msg);
    return std::nullopt;
  };

  if (PhysicalBlockBytes > std::numeric_limits<std::size_t>::max() / split_factor) {
    return fail("pmchain: block size overflow");
  }
  if (intent.block_count > std::numeric_limits<std::size_t>::max()) {
    return fail("pmchain: block count exceeds supported limit");
  }
  if (intent.batch_size > std::numeric_limits<std::size_t>::max()) {
    return fail("pmchain: batch size exceeds supported limit");
  }

  pmchain_layout_input input{};
  input.split_factor = split_factor;
  input.logical_block_bytes = PhysicalBlockBytes * split_factor;
  input.logical_block_count = static_cast<std::size_t>(intent.block_count);
  input.logical_batch_size = static_cast<std::size_t>(intent.batch_size);
  input.physical_block_bytes = PhysicalBlockBytes;

  if (input.logical_block_count > std::numeric_limits<std::size_t>::max() / split_factor) {
    return fail("pmchain: block count overflow");
  }
  input.physical_block_count =
      split_factor == 1 ? input.logical_block_count : (input.logical_block_count * split_factor);

  if (input.logical_batch_size > std::numeric_limits<std::size_t>::max() / split_factor) {
    return fail("pmchain: batch size overflow");
  }
  input.physical_batch_size = split_factor == 1 ? input.logical_batch_size : (input.logical_batch_size * split_factor);

  const std::uint64_t logical_disjoint = intent.disjoint_epoch_window;
  std::uint64_t physical_disjoint = logical_disjoint;
  if (split_factor > 1) {
    if (logical_disjoint > std::numeric_limits<std::uint64_t>::max() / split_factor) {
      return fail("pmchain: disjoint_epoch_window overflow");
    }
    physical_disjoint = logical_disjoint * split_factor;
  }
  input.logical_disjoint_window_request = logical_disjoint;
  input.physical_disjoint_window_request = physical_disjoint;

  return input;
}

std::optional<pmchain_layout> finalize_pmchain_layout(
    const pmchain_layout_input& input, std::uint64_t physical_disjoint_window, types::command_result& error_out
) {
  const auto fail = [&](std::string_view msg) -> std::optional<pmchain_layout> {
    error_out = detail::make_error(types::result_status::invalid_arguments, msg);
    return std::nullopt;
  };

  if (input.split_factor > 1 && physical_disjoint_window % input.split_factor != 0) {
    return fail("pmchain: physical disjoint window not divisible by split factor");
  }
  const std::uint64_t logical_disjoint =
      input.split_factor == 1 ? physical_disjoint_window : (physical_disjoint_window / input.split_factor);
  if (logical_disjoint < static_cast<std::uint64_t>(input.logical_batch_size)) {
    return fail("pmchain: logical disjoint_epoch_window smaller than batch size");
  }

  pmchain_layout layout{};
  layout.split_factor = input.split_factor;
  layout.logical_block_bytes = input.logical_block_bytes;
  layout.logical_block_count = input.logical_block_count;
  layout.logical_batch_size = input.logical_batch_size;
  layout.physical_block_bytes = input.physical_block_bytes;
  layout.physical_block_count = input.physical_block_count;
  layout.physical_batch_size = input.physical_batch_size;
  layout.logical_disjoint_window = logical_disjoint;
  layout.physical_disjoint_window = physical_disjoint_window;
  return layout;
}

void log_pmchain_config(
    const pmchain_layout& layout, const types::pmchain_intent& intent, const sn::omap::pmchain::threading& threads,
    sn::util::log::logger& logger
) {
  logger.inff(
      "pmchain config: logical_block_bytes=%zu physical_block_bytes=%zu split_factor=%zu block_count=%zu batch=%zu "
      "posmap_bucket=%u window_phys=%llu window_logical=%llu domain_parallel=%zu domain_bg=%zu access_parallel=%zu "
      "access_bg=%zu evict_parallel=%zu evict_bg=%zu oram_parallel=%zu drop_epoch=%u write_ratio=%.3f "
      "dummy_ratio=%.3f",
      layout.logical_block_bytes, layout.physical_block_bytes, layout.split_factor, layout.logical_block_count,
      layout.logical_batch_size, intent.posmap_bucket_size,
      static_cast<unsigned long long>(layout.physical_disjoint_window),
      static_cast<unsigned long long>(layout.logical_disjoint_window), threads.domain.logical,
      threads.domain.background, threads.access.logical, threads.access.background, threads.eviction.logical,
      threads.eviction.background, threads.oram.logical, intent.drop_epoch, intent.write_ratio, intent.dummy_ratio
  );
}

template <bool IsTiered, typename PosmapType, typename OramClient>
types::command_result run_pmchain_chain(
    const types::pmchain_intent& intent, sn::omap::pmchain::config chain_cfg,
    const sn::threads::parallelism_config& oram_parallel, PosmapType& posmap, OramClient& oram_client,
    sn::threads::thread_team access_team, sn::util::log::logger& logger
) {
  using oram_t = std::decay_t<OramClient>;
  using chain_type = sn::omap::pmchain::client<PosmapType, oram_t>;
  chain_cfg.oram_parallelism = oram_parallel.logical;
  chain_type chain(chain_cfg, posmap, oram_client, std::move(access_team));
  chain.initialize();

#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (IsTiered) {
    auto& stats_state = [&]() -> auto& {
      if constexpr (requires { oram_client.backing_state_ref(); }) {
        return oram_client.backing_state_ref();
      } else {
        return oram_client.state_ref();
      }
    }();
    log_cache_stats(stats_state, "init", logger);
    stats_state.reset_cache_stats();
    if constexpr (requires { stats_state.reset_metrics(); }) {
      stats_state.reset_metrics();
    }
  }
#endif

  types::command_result out{};

  switch (intent.action) {
  case types::oram_action::validate: {
    sn::omap::harness::pmchain::validate_options opts{};
    opts.batches = intent.batch_count == 0 ? 1u : intent.batch_count;
    opts.write_ratio = intent.write_ratio;
    opts.dummy_ratio = intent.dummy_ratio;
    sn::omap::harness::pmchain::validate(chain, opts);
    if (!out.output.assign("pmchain validate ok")) {
      return detail::make_error(types::result_status::internal_error, "result output truncated");
    }
    out.status = types::result_status::ok;
    break;
  }
  case types::oram_action::experiment: {
    sn::omap::harness::pmchain::experiment_options opts{};
    opts.batches = intent.batch_count == 0 ? 1u : intent.batch_count;
    opts.write_ratio = intent.write_ratio;
    const auto result = sn::omap::harness::pmchain::experiment(chain, opts);
    auto text = pfm::format(
        "pmchain experiment ok: batches=%zu requests=%zu", result.batches_executed, result.total_requests()
    );
    if (!out.output.assign(text)) {
      return detail::make_error(types::result_status::internal_error, "result output truncated");
    }
    out.status = types::result_status::ok;
    break;
  }
  case types::oram_action::benchmark:
    out = detail::make_error(types::result_status::unsupported, "benchmark mode unavailable for pmchain");
    break;
  default:
    out = detail::make_error(types::result_status::unsupported, "unknown pmchain action");
    break;
  }

#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (IsTiered) {
    auto& stats_state = [&]() -> auto& {
      if constexpr (requires { oram_client.backing_state_ref(); }) {
        return oram_client.backing_state_ref();
      } else {
        return oram_client.state_ref();
      }
    }();
    log_cache_stats(stats_state, "access", logger);
  }
#endif

  return out;
}

template <typename Traits>
types::command_result run_pmchain_with_traits(const types::pmchain_intent& intent, execution_context& ctx) {
  using traits_t = Traits;
  constexpr std::size_t PhysicalBlockBytes = traits_t::block_bytes;
  using posmap_type = sn::omap::o2th::o2th_rwkv<std::uint32_t, kPosmapEntryBytes>;
  using backing_oram_type = sn::oram::zingoram::client<traits_t>;
  using block_t = typename traits_t::block_t;
  using store_t = typename traits_t::block_store_t;
  constexpr bool is_tiered = std::is_base_of_v<sn::oram::zingoram::storage::tiered_store<block_t>, store_t>;

  auto invalid_args = [](std::string_view msg) {
    return detail::make_error(types::result_status::invalid_arguments, msg);
  };

  const auto requested_access = sn::threads::resolve_parallelism(static_cast<std::size_t>(intent.access_workers));
  const std::size_t eviction_threads = intent.eviction_threads != 0
                                           ? static_cast<std::size_t>(intent.eviction_threads)
                                           : sn::oram::zingoram::fit_eviction_threads(
                                                 intent.routing_depth, requested_access.logical
                                             );
  const auto thread_plan = sn::omap::pmchain::resolve_threading(
      requested_access.logical, static_cast<std::size_t>(intent.oram_parallelism), eviction_threads
  );
  const auto access_parallel = thread_plan.access;
  const auto oram_parallel = thread_plan.oram;
  const std::uint32_t access_concurrency = static_cast<std::uint32_t>(access_parallel.logical);
  const std::size_t split_factor = intent.split_factor == 0 ? 1u : static_cast<std::size_t>(intent.split_factor);

  types::command_result layout_error{};
  const auto layout_input_opt = make_pmchain_layout_input<PhysicalBlockBytes>(intent, split_factor, layout_error);
  if (!layout_input_opt) {
    return layout_error;
  }
  const auto layout_input = *layout_input_opt;

  sn::omap::pmchain::util::zingoram_config_input zing_cfg{
      .block_count = layout_input.physical_block_count,
      .batch_size = layout_input.physical_batch_size,
      .bucket_real_size = intent.bucket_real_size,
      .bucket_dummy_size = intent.bucket_dummy_size,
      .routing_depth = intent.routing_depth,
      .evict_batch = intent.evict_batch == 0 ? 1u : intent.evict_batch,
      .access_concurrency = access_concurrency,
      .disjoint_epoch_window = layout_input.physical_disjoint_window_request,
  };
  auto zing_setup = sn::omap::pmchain::util::compute_zingoram_setup<traits_t>(zing_cfg, "pmchain");
  auto oram_opts = zing_setup.opts;

  const auto layout_opt = finalize_pmchain_layout(layout_input, zing_setup.disjoint_window, layout_error);
  if (!layout_opt) {
    return layout_error;
  }
  const auto layout = *layout_opt;

  if constexpr (is_tiered) {
    oram_opts.hot_memory_budget_bytes = intent.hot_budget_bytes;
    oram_opts.cache_memory_budget_bytes = intent.cache_budget_bytes;
    oram_opts.backend_cache_budget_bytes = intent.backend_cache_budget_bytes;
    oram_opts.cache_pack_factor = intent.cache_pack_factor;
    const auto cache_view = intent.cache_path.view();
    oram_opts.cache_path.assign(cache_view.begin(), cache_view.end());
  } else {
    oram_opts.hot_memory_budget_bytes = 0;
    oram_opts.cache_memory_budget_bytes = 0;
    oram_opts.backend_cache_budget_bytes = 0;
    oram_opts.cache_pack_factor = 1;
    oram_opts.cache_path.clear();
  }

  sn::sgxbridge::tp::session pmchain_session;
  const auto pmchain_res = pmchain_session.open(
      ctx.threadpools, detail::make_threadpool_request(thread_plan.domain.background, "sonic_demo.pmchain")
  );
  if (!pmchain_res.succeeded() || pmchain_session.pool() == nullptr) {
    return detail::session_error("sonic_demo.pmchain", pmchain_res);
  }

  const std::size_t build_capacity = layout.logical_batch_size;
  posmap_type posmap(
      typename posmap_type::config{.block_count = build_capacity, .bucket_size = intent.posmap_bucket_size},
      sn::threads::thread_team(pmchain_session.pool_ref(), access_parallel.logical)
  );

  sn::omap::pmchain::config chain_cfg{
      .block_count = layout.logical_block_count,
      .oram_block_bytes = layout.logical_block_bytes,
      .batch_size = layout.logical_batch_size,
      .oram_parallelism = oram_parallel.logical,
  };
  chain_cfg.drop_epoch = intent.drop_epoch != 0;

  auto logger = sn::util::log::create("sonic_demo.pmchain");
  log_pmchain_config(layout, intent, thread_plan, logger);
#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (is_tiered) {
    logger.inff(
        "pmchain tiered: hot_budget=%llu cache_budget=%llu backend_cache=%llu cache_pack_factor=%u",
        static_cast<unsigned long long>(intent.hot_budget_bytes),
        static_cast<unsigned long long>(intent.cache_budget_bytes),
        static_cast<unsigned long long>(intent.backend_cache_budget_bytes), intent.cache_pack_factor
    );
  }
#endif

  types::command_result result{};

  if (layout.split_factor == 1) {
    backing_oram_type backing_oram(
        oram_opts, sn::threads::thread_team(pmchain_session.pool_ref(), thread_plan.eviction.logical)
    );
    using direct_adapter = sn::oram::adapter::direct_block<backing_oram_type>;
    typename direct_adapter::options_t logical_opts{
        .block_count = layout.logical_block_count, .disjoint_epoch_window = layout.logical_disjoint_window
    };
    direct_adapter oram_client(backing_oram, logical_opts);
    result = run_pmchain_chain<is_tiered>(
        intent, chain_cfg, oram_parallel, posmap, oram_client,
        sn::threads::thread_team(pmchain_session.pool_ref(), access_parallel.logical), logger
    );
  } else {
    const bool dispatched = sn::util::demo::dispatch_static_param<pmchain_supported_split_factors>(
        layout.split_factor, [&](auto split_tag) {
          constexpr std::size_t Split = decltype(split_tag)::value;
          using adapter_type = sn::oram::adapter::split_block<backing_oram_type, Split>;
          typename adapter_type::options_t logical_opts{
              .block_count = layout.logical_block_count, .disjoint_epoch_window = layout.logical_disjoint_window
          };
          backing_oram_type backing_oram(
              oram_opts, sn::threads::thread_team(pmchain_session.pool_ref(), thread_plan.eviction.logical)
          );
          adapter_type oram_client(backing_oram, logical_opts);
          result = run_pmchain_chain<is_tiered>(
              intent, chain_cfg, oram_parallel, posmap, oram_client,
              sn::threads::thread_team(pmchain_session.pool_ref(), access_parallel.logical), logger
          );
        }
    );
    if (!dispatched) {
      return invalid_args(
          std::string("pmchain: unsupported split factor; supported factors are 1, ") +
          sn::util::demo::format_supported_params(pmchain_supported_split_factors::values)
      );
    }
  }

#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (is_tiered) {
    cleanup_cache_files(oram_opts.cache_path);
  }
#endif
  return result;
}

inline types::command_result run_pmchain(const types::pmchain_intent& intent, execution_context& ctx) {
  if (!ctx.threadpools.available()) {
    return detail::make_error(types::result_status::unsupported, "thread pool provider unavailable");
  }
  if (intent.block_count == 0 || intent.batch_size == 0) {
    return detail::make_error(types::result_status::invalid_arguments, "block count and batch size must be > 0");
  }
  if (intent.posmap_bucket_size == 0) {
    return detail::make_error(types::result_status::invalid_arguments, "posmap bucket size must be > 0");
  }
  if (intent.bucket_real_size == 0 || intent.bucket_dummy_size == 0) {
    return detail::make_error(types::result_status::invalid_arguments, "bucket sizes must be > 0");
  }
  if (intent.routing_depth >= 63) {
    return detail::make_error(types::result_status::invalid_arguments, "routing depth must be < 63");
  }
  if (intent.batch_size % intent.posmap_bucket_size != 0) {
    return detail::make_error(
        types::result_status::invalid_arguments, "pmchain: batch size must align with bucket size"
    );
  }

  if (intent.tiered != 0) {
#if !defined(SONIC_ORAM_TIERED_STORAGE)
    return detail::make_error(types::result_status::unsupported, "tiered storage disabled at build time");
#endif
    if (intent.cache_budget_bytes == 0) {
      return detail::make_error(types::result_status::invalid_arguments, "cache budget must be > 0 for tiered storage");
    }
    if (intent.cache_pack_factor == 0) {
      return detail::make_error(types::result_status::invalid_arguments, "cache pack factor must be > 0");
    }
    if (intent.cache_path.empty()) {
      return detail::make_error(types::result_status::invalid_arguments, "tiered storage needs cache path");
    }
  }

  const std::size_t split_factor = intent.split_factor == 0 ? 1u : static_cast<std::size_t>(intent.split_factor);
  if (split_factor != 1 && !sn::util::demo::is_supported_param(pmchain_supported_split_factors{}, split_factor)) {
    return detail::make_error(
        types::result_status::invalid_arguments,
        std::string("pmchain: unsupported split factor; supported factors are 1, ") +
            sn::util::demo::format_supported_params(pmchain_supported_split_factors::values)
    );
  }

  types::command_result result{};
  const bool dispatched = sn::util::demo::dispatch_block_size<pmchain_supported_block_sizes>(
      static_cast<std::size_t>(intent.block_bytes), [&](auto size_tag) {
        constexpr std::size_t BlockBytes = decltype(size_tag)::value;
        using slab_traits = sn::oram::zingoram::traits<
            BlockBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::slab_store,
            sn::oram::zingoram::detail::null_cache_backend_factory>;
#if defined(SONIC_ORAM_TIERED_STORAGE)
        using tiered_traits = sn::oram::zingoram::traits<
            BlockBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch, tiered_store_default, cache_backend_factory>;
#endif
        if (intent.tiered != 0) {
#if defined(SONIC_ORAM_TIERED_STORAGE)
          result = run_pmchain_with_traits<tiered_traits>(intent, ctx);
#else
          result = detail::make_error(types::result_status::unsupported, "tiered storage disabled at build time");
#endif
          return;
        }
        result = run_pmchain_with_traits<slab_traits>(intent, ctx);
      }
  );
  if (!dispatched) {
    return detail::make_error(
        types::result_status::invalid_arguments,
        std::string("pmchain: unsupported block size; supported sizes are ") +
            sn::util::demo::format_supported_sizes(pmchain_supported_block_sizes::values) + " bytes"
    );
  }
  return result;
}

}

}
