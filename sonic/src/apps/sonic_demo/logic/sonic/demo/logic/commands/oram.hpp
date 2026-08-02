#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "sonic/demo/logic/commands/common.hpp"
#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/types/intents.hpp"
#include "sonic/oram/harness/benchmark.hpp"
#include "sonic/oram/harness/experiment.hpp"
#include "sonic/oram/harness/validate.hpp"
#include "sonic/oram/harness/detail/frontend_support.hpp"
#include "sonic/oram/pathoram/client.hpp"
#include "sonic/oram/pathoram/traits.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/demo/logic/commands/detail/zingoram_tiered.hpp"
#include "sonic/storage/cache/stats.hpp"
#if defined(SN_SGX_ENCLAVE)
#include "sonic/sgxbridge/storage/encrypted_file_backend.hpp"
#else
#include "sonic/storage/io/posix_file_backend.hpp"
#endif
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/bench/minibench.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/util/demo/block_size_dispatch.hpp"

namespace sn::demo::logic::commands::oram {

namespace {

constexpr std::size_t kBlockBytes = 64;

using pathoram_supported_block_sizes = sn::util::demo::block_size_list<64, 256>;
using zingoram_supported_block_sizes = sn::util::demo::block_size_list<64, 256>;

using sn::demo::logic::commands::detail::tiered_store_default;
#if defined(SN_SGX_ENCLAVE)
using sn::demo::logic::commands::detail::sgx_cache_backend_factory;
#else
using sn::demo::logic::commands::detail::posix_cache_backend_factory;
#endif

template <typename options_t>
void log_common_summary(
    sn::util::log::logger& logger, const options_t& opts, std::size_t accesses, std::string_view label,
    types::oram_action action, std::uint32_t bucket_size, std::uint32_t tree_height, std::size_t block_bytes
) {
  logger.inff(
      "%s config: block_bytes=%u bucket_size=%u block_count=%llu tree_height=%u accesses=%zu mode=%s", label.data(),
      static_cast<unsigned>(block_bytes), static_cast<unsigned>(bucket_size),
      static_cast<unsigned long long>(opts.block_count), static_cast<unsigned>(tree_height), accesses,
      detail::describe_action(action)
  );
}

using sn::demo::logic::commands::detail::log_cache_stats;

[[nodiscard]] inline types::command_result make_success_result(std::string text) {
  types::command_result out{};
  if (!out.output.assign(text)) {
    return detail::make_error(types::result_status::internal_error, "result output truncated");
  }
  out.status = types::result_status::ok;
  return out;
}

struct session_thread_resources {
  sn::sgxbridge::tp::session access{};
  sn::sgxbridge::tp::session eviction{};
  sn::sgxbridge::tp::session domain{};
  sn::threads::thread_pool* access_pool = nullptr;
  std::optional<sn::threads::thread_team> eviction_team{};

  [[nodiscard]] bool uses_shared_domain() const noexcept { return domain.pool() != nullptr; }

  void park_eviction_if_separate() {
    if (eviction.pool() != nullptr) {
      eviction.pool_ref().park();
    }
  }
};

[[nodiscard]] inline std::optional<types::command_result> acquire_thread_resources(
    session_thread_resources& out, const sn::sgxbridge::tp::provider& provider, const sn::oram::zingoram::threading& plan
) {
  if (plan.domain.has_value()) {
    const auto req = detail::make_threadpool_request(plan.domain->background, "sonic_demo.oram.domain");
    const auto res = out.domain.open(provider, req);
    if (!res.succeeded() || out.domain.pool() == nullptr) {
      return detail::session_error("domain", res);
    }
    out.access_pool = out.domain.pool();
    out.eviction_team.emplace(out.domain.pool_ref(), plan.eviction.logical);
    return std::nullopt;
  }

  if (plan.access.background > 0) {
    const auto req = detail::make_threadpool_request(plan.access.background, "sonic_demo.oram.access");
    const auto res = out.access.open(provider, req);
    if (!res.succeeded()) {
      return detail::session_error("access", res);
    }
    out.access_pool = out.access.pool();
  }

  const auto eviction_req = detail::make_threadpool_request(plan.eviction.background, "sonic_demo.oram.eviction");
  const auto eviction_res = out.eviction.open(provider, eviction_req);
  if (!eviction_res.succeeded() || out.eviction.pool() == nullptr) {
    return detail::session_error("eviction", eviction_res);
  }
  out.eviction_team.emplace(out.eviction.pool_ref(), plan.eviction.logical);
  return std::nullopt;
}

template <typename supported_sizes_t, typename RunFn>
[[nodiscard]] inline types::command_result dispatch_block_bytes_or_error(
    std::size_t block_bytes, RunFn&& run, std::string_view label
) {
  types::command_result result{};
  const bool dispatched = sn::util::demo::dispatch_block_size<supported_sizes_t>(block_bytes, [&](auto size_tag) {
    result = run(size_tag);
  });
  if (!dispatched) {
    return detail::make_error(
        types::result_status::invalid_arguments, "unsupported " + std::string(label) + " block size"
    );
  }
  return result;
}

template <typename Traits, typename Client>
types::command_result dispatch_action(
    types::oram_action action, Client& client, std::size_t accesses, std::string_view label,
    std::optional<std::size_t> disjoint_window, sn::threads::thread_pool* access_pool, std::size_t access_workers,
    bool online_only, sn::util::log::logger& logger
) {
  using namespace sn::oram::harness;
  const std::string label_text(label);
  const auto run = sn::oram::harness::detail::make_harness_run_options(
      accesses, disjoint_window, access_pool, access_workers, online_only
  );
  switch (action) {
  case types::oram_action::validate: {
    validate_options opts{};
    opts.run = run;
    opts.batch_accesses = accesses;
    const auto result = validate<Traits>(client, opts);
    logger.inff("%s validation completed", label_text);
    if (result.dummy_probe_accesses != 0) {
      logger.inff("  dummy probe accesses: %zu", result.dummy_probe_accesses);
    }
    if (result.round_trip_accesses != 0) {
      logger.inff("  round trip accesses: %zu", result.round_trip_accesses);
    }
    if (result.batch_accesses != 0) {
      logger.inff("  batch accesses: %zu", result.batch_accesses);
    }
    auto text = pfm::format(
        "%s validate ok: dummy=%zu round_trip=%zu batch=%zu", label_text, result.dummy_probe_accesses,
        result.round_trip_accesses, result.batch_accesses
    );
    return make_success_result(std::move(text));
  }
  case types::oram_action::experiment: {
    sn::oram::harness::detail::await_profiler_if_needed(logger);
    experiment_options opts{};
    opts.run = run;
    const auto result = experiment<Traits, Client>(client, opts);
    logger.inff("%s experiment summary:", label_text);
    logger.inff("  access_count: %zu", result.access_count);
    logger.inff("  concurrency: %zu", result.concurrency);
    logger.inff("  elapsed: %.6f s", result.elapsed_seconds);
    logger.inff("  throughput_ops_per_sec: %.6f op/s", result.throughput_ops_per_sec);
    logger.inff("  throughput_bytes_per_sec: %.6f B/s", result.throughput_bytes_per_sec);
    auto text = pfm::format(
        "%s experiment ok: ops=%zu throughput=%.2f ops/s", label_text, result.access_count,
        result.throughput_ops_per_sec
    );
    return make_success_result(std::move(text));
  }
  case types::oram_action::benchmark: {
    sn::oram::harness::detail::await_profiler_if_needed(logger);
    benchmark_options opts{};
    opts.run = run;
    auto bench = benchmark<Traits, Client>(client, opts);
    sn::util::bench::format_options fmt{};
    fmt.show_samples = true;
    fmt.show_environment = true;
    fmt.style = sn::util::bench::format_style::pretty;
    logger.inff("%s benchmark summary:", label_text);
    logger.inff("  latency: %s", sn::util::bench::format(bench.latency, fmt));
    logger.inff("  throughput: %s", sn::util::bench::format(bench.throughput, fmt));
    return make_success_result(pfm::format("%s benchmark ok", label_text));
  }
  }
  return detail::make_error(types::result_status::unsupported, "unknown ORAM action");
}

template <typename options_t>
std::optional<std::size_t> try_compute_zingoram_online_only_window(
    const options_t& opts, std::size_t accesses, sn::util::log::logger& logger
) {
  auto computed = sn::oram::harness::detail::try_compute_zingoram_online_only_window(opts, accesses);
  if (!computed.has_value()) {
    logger.errf("try_compute_zingoram_online_only_window: invalid online-only window");
  }
  return computed;
}

}

template <std::size_t BlockBytes>
types::command_result run_pathoram_impl(const types::pathoram_intent& intent, execution_context& ctx) {
  using traits_t = sn::oram::pathoram::traits<BlockBytes>;
  typename traits_t::options_t opts{};
  opts.block_count = intent.block_count;

  using client_t = sn::oram::pathoram::client<traits_t>;
  client_t client(opts);
  client.initialize();

  const std::size_t accesses = detail::clamp_accesses(intent.accesses);
  log_common_summary(
      ctx.logger, opts, accesses, "pathoram", intent.action, static_cast<std::uint32_t>(traits_t::bucket_size),
      static_cast<std::uint32_t>(client.shape().height), BlockBytes
  );

  return dispatch_action<traits_t>(
      intent.action, client, accesses, "pathoram", std::nullopt, nullptr, 1, false, ctx.logger
  );
}

inline types::command_result run_pathoram(const types::pathoram_intent& intent, execution_context& ctx) {
  if (intent.block_count == 0) {
    return detail::make_error(types::result_status::invalid_arguments, "block count must be positive");
  }

  return dispatch_block_bytes_or_error<pathoram_supported_block_sizes>(
      intent.block_bytes,
      [&](auto size_tag) {
        constexpr std::size_t BlockBytes = decltype(size_tag)::value;
        return run_pathoram_impl<BlockBytes>(intent, ctx);
      },
      "pathoram"
  );
}

template <typename Traits>
types::command_result run_zingoram_mode_impl(const types::zingoram_intent& intent, execution_context& ctx) {
  using traits_t = Traits;
  using block_t = typename traits_t::block_t;
  using store_t = typename traits_t::block_store_t;
  constexpr bool is_tiered = std::is_base_of_v<sn::oram::zingoram::storage::tiered_store<block_t>, store_t>;

  typename traits_t::options_t opts{};
  opts.block_count = intent.block_count;
  opts.bucket_real_size = intent.bucket_real;
  opts.bucket_dummy_size = intent.bucket_dummy;
  opts.eviction_rate = intent.eviction_rate;
  opts.routing_depth = intent.routing_depth;
  opts.evict_batch = intent.evict_batch;
  opts.access_concurrency = std::max<std::uint32_t>(intent.access_concurrency, 1u);
  if constexpr (is_tiered) {
    if (intent.cache_budget_bytes == 0) {
      return detail::make_error(
          types::result_status::invalid_arguments, "cache budget must be positive for tiered store"
      );
    }
    if (intent.cache_pack_factor == 0) {
      return detail::make_error(
          types::result_status::invalid_arguments, "cache pack factor must be positive (levels or blocks per page)"
      );
    }
    const auto cache_path_view = intent.cache_path.view();
    if (cache_path_view.empty()) {
      return detail::make_error(types::result_status::invalid_arguments, "cache path is required for tiered store");
    }
    opts.hot_memory_budget_bytes = intent.hot_budget_bytes;
    opts.cache_memory_budget_bytes = intent.cache_budget_bytes;
    opts.backend_cache_budget_bytes = intent.backend_cache_budget_bytes;
    opts.cache_pack_factor = intent.cache_pack_factor;
    opts.cache_path.assign(cache_path_view.begin(), cache_path_view.end());
  } else {
    opts.hot_memory_budget_bytes = 0;
    opts.cache_memory_budget_bytes = 0;
    opts.backend_cache_budget_bytes = 0;
    opts.cache_pack_factor = 1;
    opts.cache_path.clear();
  }

  const std::size_t accesses = detail::clamp_accesses(intent.accesses);
  std::optional<std::size_t> disjoint_window;
  bool online_only = false;
  if constexpr (traits_t::mode == sn::oram::zingoram::epoch_mode::disjoint_epoch) {
    online_only = intent.online_only != 0;
    if (online_only) {
      auto computed = try_compute_zingoram_online_only_window(opts, accesses, ctx.logger);
      if (!computed.has_value()) {
        return detail::make_error(types::result_status::invalid_arguments, "failed to compute disjoint epoch window");
      }
      disjoint_window = computed.value();
      opts.disjoint_epoch_window = computed.value();
    } else if (intent.disjoint_window_present != 0) {
      disjoint_window = static_cast<std::size_t>(intent.disjoint_window);
      opts.disjoint_epoch_window = disjoint_window.value();
    } else {
      return detail::make_error(types::result_status::invalid_arguments, "missing disjoint epoch window");
    }
  } else {
    opts.disjoint_epoch_window = 0;
  }

  const auto thread_plan = sn::oram::zingoram::resolve_threading<traits_t::mode>(
      sn::oram::zingoram::threading_input{
          .access = static_cast<std::size_t>(opts.access_concurrency),
          .eviction = intent.eviction_threads,
          .routing_depth = opts.routing_depth,
          .online_only = online_only,
      }
  );
  if (thread_plan.access.logical > std::numeric_limits<std::uint32_t>::max() ||
      thread_plan.eviction.logical > std::numeric_limits<std::uint32_t>::max()) {
    return detail::make_error(types::result_status::invalid_arguments, "thread count exceeds supported limit");
  }
  opts.access_concurrency = static_cast<std::uint32_t>(thread_plan.access.logical);

  session_thread_resources thread_resources{};
  if (auto error = acquire_thread_resources(thread_resources, ctx.threadpools, thread_plan); error.has_value()) {
    return std::move(*error);
  }

  using client_t = sn::oram::zingoram::client<traits_t>;
  client_t client(opts, std::move(*thread_resources.eviction_team));
  client.initialize();
  if (online_only && !thread_resources.uses_shared_domain()) {
    thread_resources.park_eviction_if_separate();
  }

  const char* label =
      traits_t::mode == sn::oram::zingoram::epoch_mode::disjoint_epoch ? "zingoram-disjoint" : "zingoram";
  log_common_summary(
      ctx.logger, opts, accesses, label, intent.action, static_cast<std::uint32_t>(client.bucket_size()),
      static_cast<std::uint32_t>(client.shape().height), traits_t::block_bytes
  );

#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (is_tiered) {
    log_cache_stats(client.state_ref(), "init", ctx.logger);
    client.state_ref().reset_cache_stats();
    if constexpr (requires { client.state_ref().reset_metrics(); }) {
      client.state_ref().reset_metrics();
    }
  }
#endif

  auto result = dispatch_action<traits_t>(
      intent.action, client, accesses, label, disjoint_window, thread_resources.access_pool, thread_plan.access.logical,
      online_only, ctx.logger
  );
#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (is_tiered) {
    log_cache_stats(client.state_ref(), "access", ctx.logger);
    sn::demo::logic::commands::detail::cleanup_cache_files(opts.cache_path);
  }
#endif
  return result;
}

template <sn::oram::zingoram::epoch_mode Mode>
types::command_result run_zingoram_mode(const types::zingoram_intent& intent, execution_context& ctx) {
  if (!ctx.threadpools.available()) {
    return detail::make_error(types::result_status::unsupported, "thread pool provider unavailable");
  }
  if (intent.block_count == 0) {
    return detail::make_error(types::result_status::invalid_arguments, "block count must be positive");
  }

  return dispatch_block_bytes_or_error<zingoram_supported_block_sizes>(
      intent.block_bytes,
      [&](auto size_tag) {
        constexpr std::size_t BlockBytes = decltype(size_tag)::value;
        if (intent.tiered != 0) {
#if defined(SONIC_ORAM_TIERED_STORAGE)
          using cache_factory =
#if defined(SN_SGX_ENCLAVE)
              sgx_cache_backend_factory;
#else
              posix_cache_backend_factory;
#endif
          using tiered_traits = sn::oram::zingoram::traits<BlockBytes, Mode, tiered_store_default, cache_factory>;
          return run_zingoram_mode_impl<tiered_traits>(intent, ctx);
#else
          return detail::make_error(types::result_status::unsupported, "tiered storage disabled at build time");
#endif
        }

        using slab_traits = sn::oram::zingoram::traits<
            BlockBytes, Mode, sn::oram::zingoram::storage::slab_store,
            sn::oram::zingoram::detail::null_cache_backend_factory>;
        return run_zingoram_mode_impl<slab_traits>(intent, ctx);
      },
      "zingoram"
  );
}

inline types::command_result run_zingoram(const types::zingoram_intent& intent, execution_context& ctx) {
  if (intent.mode == 0) {
    return run_zingoram_mode<sn::oram::zingoram::epoch_mode::default_epoch>(intent, ctx);
  }
  return run_zingoram_mode<sn::oram::zingoram::epoch_mode::disjoint_epoch>(intent, ctx);
}

}
