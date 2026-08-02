#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <filesystem>

#include "cli_common.hpp"
#include "sonic/oram/pathoram/client.hpp"
#include "sonic/oram/pathoram/traits.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#if defined(SONIC_ORAM_TIERED_STORAGE)
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/storage/cache/stats.hpp"
#include "sonic/util/demo/zingoram_tiered.hpp"
#endif
#include "sonic/oram/harness/benchmark.hpp"
#include "sonic/oram/harness/experiment.hpp"
#include "sonic/oram/harness/validate.hpp"
#include "sonic/oram/harness/detail/frontend_support.hpp"
#include "sonic/util/cli/thread_options.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/threads/thread_pool.hpp"
#include "sonic/util/bench/minibench.hpp"
#include "sonic/util/ext/args.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/util/demo/block_size_dispatch.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/humanize.hpp"

namespace log_ns = sn::util::log;
namespace cli = sn::demo::cli;
#if defined(SONIC_ORAM_TIERED_STORAGE)
using sn::util::demo::log_cache_stats;
#endif
namespace {
constexpr std::size_t kBlockBytes = 64;

using pathoram_supported_block_sizes = sn::util::demo::block_size_list<64, 256>;
using zingoram_supported_block_sizes = sn::util::demo::block_size_list<64, 256>;

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

template <typename traits_t, typename client_t>
auto bucket_size_for(const client_t&, int) -> decltype(traits_t::bucket_size, std::size_t{}) {
  return traits_t::bucket_size;
}

template <typename traits_t, typename client_t> std::size_t bucket_size_for(const client_t& client, long) {
  return client.bucket_size();
}

template <typename traits_t, typename client_t>
void log_summary(const client_t& client, std::size_t accesses, std::string_view label, action_kind action) {
  auto logger = log_ns::create("demo");
  const char* mode = nullptr;
  switch (action) {
  case action_kind::validate:
    mode = "validate";
    break;
  case action_kind::experiment:
    mode = "experiment";
    break;
  case action_kind::benchmark:
    mode = "benchmark";
    break;
  }
  logger.inf(
      pfm::format(
          "%s config: block_bytes=%zu bucket_size=%zu block_count=%zu tree_height=%zu accesses=%zu mode=%s",
          std::string(label), static_cast<std::size_t>(traits_t::block_bytes),
          static_cast<std::size_t>(bucket_size_for<traits_t>(client, 0)), client.options().block_count,
          client.shape().height, accesses, mode
      )
  );
}

template <typename traits_t, typename client_t>
void dispatch_action(
    action_kind action, client_t& client, std::size_t accesses, std::string_view label,
    std::optional<std::size_t> disjoint_window, sn::threads::thread_pool* access_pool, std::size_t access_workers,
    bool online_only
);

template <typename traits_t, typename client_t>
void run_fixed_bucket_mode(
    action_kind action, std::uint64_t block_count, std::size_t bucket_size, std::size_t accesses,
    std::string_view label, std::string_view family_name
) {
  if (bucket_size != traits_t::bucket_size) {
    throw args::Error(std::string("bucket size must match ") + std::string(family_name) + " build");
  }

  typename traits_t::options_t opts{};
  opts.block_count = block_count;

  client_t client(opts);
  client.initialize();

  log_summary<traits_t>(client, accesses, label, action);
  dispatch_action<traits_t>(action, client, accesses, label, std::nullopt, nullptr, 1, false);
}

template <typename supported_sizes_t, typename RunFn>
void dispatch_block_bytes_or_throw(std::size_t block_bytes, RunFn&& run, std::string_view what) {
  const bool dispatched = sn::util::demo::dispatch_block_size<supported_sizes_t>(block_bytes, std::forward<RunFn>(run));
  if (!dispatched) {
    throw args::Error(
        "unsupported block size; supported sizes are " +
        sn::util::demo::format_supported_sizes(supported_sizes_t::values) + " bytes for " + std::string(what)
    );
  }
}

template <typename options_t>
[[nodiscard]] std::size_t require_zingoram_online_only_window(const options_t& opts, std::size_t accesses) {
  const auto computed = sn::oram::harness::detail::try_compute_zingoram_online_only_window(opts, accesses);
  sn::util::log::ensure(computed.has_value(), "require_zingoram_online_only_window: invalid online-only window");
  return computed.value();
}

template <typename traits_t, typename client_t>
void dispatch_action(
    action_kind action, client_t& client, std::size_t accesses, std::string_view label,
    std::optional<std::size_t> disjoint_window, sn::threads::thread_pool* access_pool, std::size_t access_workers,
    bool online_only
) {
  const auto run = sn::oram::harness::detail::make_harness_run_options(
      accesses, disjoint_window, access_pool, access_workers, online_only
  );

  switch (action) {
  case action_kind::validate: {
    sn::oram::harness::validate_options opts{};
    opts.run = run;
    opts.batch_accesses = accesses;
    const auto result = sn::oram::harness::validate<traits_t>(client, opts);

    auto logger = log_ns::create("demo");
    logger.inf(std::string(label) + " validation completed successfully");
    if (result.dummy_probe_accesses != 0) {
      logger.inf("  dummy probe accesses: " + std::to_string(result.dummy_probe_accesses));
    }
    if (result.round_trip_accesses != 0) {
      logger.inf("  round trip accesses: " + std::to_string(result.round_trip_accesses));
    }
    if (result.batch_accesses != 0) {
      logger.inf("  batch accesses: " + std::to_string(result.batch_accesses));
    }
    break;
  }
  case action_kind::experiment: {
    auto logger = log_ns::create("demo");
    sn::oram::harness::detail::await_profiler_if_needed(logger);
    sn::oram::harness::experiment_options opts{};
    opts.run = run;

    const auto result = sn::oram::harness::experiment<traits_t, client_t>(client, opts);

    logger.inf(std::string(label) + " experiment summary:");
    logger.inf("  access_count: " + std::to_string(result.access_count));
    logger.inf("  concurrency: " + std::to_string(result.concurrency));
    logger.inf("  elapsed: " + std::to_string(result.elapsed_seconds) + " s");
    logger.inf("  throughput_ops_per_sec: " + std::to_string(result.throughput_ops_per_sec) + " op/s");
    logger.inf("  throughput_bytes_per_sec: " + std::to_string(result.throughput_bytes_per_sec) + " B/s");
    break;
  }
  case action_kind::benchmark: {
    auto wait_logger = log_ns::create("demo");
    sn::oram::harness::detail::await_profiler_if_needed(wait_logger);

    sn::oram::harness::benchmark_options opts{};
    opts.run = run;

    const auto bench = sn::oram::harness::benchmark<traits_t, client_t>(client, opts);

    auto logger = log_ns::create("bench");
    sn::util::bench::format_options fmt{};
    fmt.show_samples = true;
    fmt.show_environment = true;
    fmt.style = sn::util::bench::format_style::pretty;

    logger.inf("  latency: " + sn::util::bench::format(bench.latency, fmt));
    logger.inf("  throughput: " + sn::util::bench::format(bench.throughput, fmt));
    break;
  }
  }
}

void run_pathoram(args::Subparser& parser) {
  args::Positional<std::string> action_arg(parser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::uint64_t> block_count_flag(
      parser, "block-count", "block count", {'N', "block-count"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> block_bytes_flag(parser, "block-bytes", "block size", {'B', "block-bytes"}, kBlockBytes);
  args::ValueFlag<std::size_t> bucket_size_flag(parser, "bucket-size", "bucket size", {'Z', "bucket-size"}, 4);
  args::ValueFlag<std::size_t> accesses_flag(parser, "accesses", "accesses", {'m', "accesses"}, 0);

  parser.Parse();

  const auto action = parse_action(args::get(action_arg));
  const auto block_count = args::get(block_count_flag);
  const auto block_bytes = args::get(block_bytes_flag);
  const auto bucket_size = args::get(bucket_size_flag);
  const auto accesses = args::get(accesses_flag);

  dispatch_block_bytes_or_throw<pathoram_supported_block_sizes>(
      block_bytes,
      [&](auto size_tag) {
        constexpr std::size_t BlockBytes = decltype(size_tag)::value;
        using traits_t = sn::oram::pathoram::traits<BlockBytes>;
        using client_t = sn::oram::pathoram::client<traits_t>;
        run_fixed_bucket_mode<traits_t, client_t>(action, block_count, bucket_size, accesses, "pathoram", "PathORAM");
      },
      "pathoram"
  );
}

struct zingoram_cli_options {
  action_kind action;
  std::uint64_t block_count;
  std::size_t block_bytes;
  std::size_t accesses;
  std::uint32_t bucket_real;
  std::uint32_t bucket_dummy;
  std::uint32_t eviction_rate;
  std::uint32_t routing_depth;
  std::uint32_t evict_batch;
  std::uint32_t access_concurrency;
  std::size_t eviction_threads;
  std::optional<std::size_t> disjoint_window;
  bool online_only = false;
  std::uint64_t hot_budget_bytes = 0;
  std::uint64_t cache_budget_bytes = 0;
  std::uint64_t backend_cache_budget_bytes = 0;
  std::uint32_t cache_pack_factor = 1;
  std::string cache_path = "/tmp/sonic_oram_demo.dat";
};

zingoram_cli_options parse_zingoram_cli(args::Subparser& parser, bool disjoint_mode, bool allow_hot_budget) {
  args::Positional<std::string> action_arg(parser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::uint64_t> block_count_flag(
      parser, "block-count", "block count", {'N', "block-count"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> block_bytes_flag(parser, "block-bytes", "block size", {'B', "block-bytes"}, kBlockBytes);
  args::ValueFlag<std::uint32_t> bucket_real_flag(
      parser, "bucket-real", "bucket real", {'Z', "bucket-real"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> bucket_dummy_flag(
      parser, "bucket-dummy", "bucket dummy", {'S', "bucket-dummy"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> eviction_rate_flag(
      parser, "eviction-rate", "eviction rate", {'A', "eviction-rate"}, 0
  );
  args::ValueFlag<std::uint32_t> routing_depth_flag(parser, "routing-depth", "routing depth", {"routing-depth"}, 0);
  args::ValueFlag<std::uint32_t> evict_batch_flag(parser, "evict-batch", "evict batch", {"evict-batch"}, 1);
  args::ValueFlag<std::uint32_t> access_concurrency_flag(
      parser, "access-concurrency", "access concurrency", {'T', "access-concurrency"}, 1
  );
  args::ValueFlag<std::size_t> accesses_flag(parser, "accesses", "accesses", {'m', "accesses"}, 0);
  args::ValueFlag<std::size_t> eviction_threads_flag(
      parser, "evict-threads", "evict threads; 0 for auto", {"evict-threads"}, 0
  );
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

  zingoram_cli_options cli_opts{};

  if (disjoint_mode) {
    args::ValueFlag<std::uint64_t> disjoint_window_flag(parser, "disjoint-epoch", "epoch window", {"disjoint-epoch"});
    args::Flag online_only_flag(parser, "online-only", "skip epoch flush", {"online-only"});
    parser.Parse();
    cli_opts.online_only = online_only_flag.Get();
    if (disjoint_window_flag) {
      cli_opts.disjoint_window = static_cast<std::size_t>(args::get(disjoint_window_flag));
    }
    if (cli_opts.online_only && cli_opts.disjoint_window.has_value()) {
      throw args::Error("use --online-only or --disjoint-epoch");
    }
    if (!cli_opts.online_only && !cli_opts.disjoint_window.has_value()) {
      throw args::Error("set --disjoint-epoch or --online-only");
    }
  } else {
    parser.Parse();
    cli_opts.online_only = false;
  }

  cli_opts.action = parse_action(args::get(action_arg));
  cli_opts.block_count = args::get(block_count_flag);
  cli_opts.accesses = args::get(accesses_flag);
  cli_opts.bucket_real = args::get(bucket_real_flag);
  cli_opts.bucket_dummy = args::get(bucket_dummy_flag);
  cli_opts.eviction_rate = args::get(eviction_rate_flag);
  cli_opts.routing_depth = args::get(routing_depth_flag);
  cli_opts.evict_batch = args::get(evict_batch_flag);
  cli_opts.access_concurrency = args::get(access_concurrency_flag);
  cli_opts.eviction_threads = args::get(eviction_threads_flag);
  if (allow_hot_budget) {
    cli_opts.hot_budget_bytes = sn::util::humanize::parse_bytes(args::get(hot_budget_flag));
    cli_opts.cache_budget_bytes = sn::util::humanize::parse_bytes(args::get(cache_budget_flag));
    cli_opts.backend_cache_budget_bytes = sn::util::humanize::parse_bytes(args::get(backend_cache_budget_flag));
    cli_opts.cache_pack_factor = args::get(cache_pack_factor_flag);
    cli_opts.cache_path = args::get(cache_path_flag);
    if (cli_opts.cache_pack_factor == 0) {
      throw args::Error("--cache-pack-factor must be > 0");
    }
    if (cli_opts.cache_budget_bytes == 0) {
      throw args::Error("tiered runs need --cache-budget");
    }
  }

  cli_opts.block_bytes = args::get(block_bytes_flag);

  return cli_opts;
}

template <std::size_t BlockBytes, sn::oram::zingoram::epoch_mode mode_v, template <typename, typename...> class StoreT>
void run_zingoram_mode_impl(
    const zingoram_cli_options& cli_opts, const char* label, sn::threads::thread_context& threads
) {
  auto logger = log_ns::create("demo");

#if defined(SONIC_ORAM_TIERED_STORAGE)
  using block_t = sn::oram::tree::block<BlockBytes>;
  constexpr bool store_is_tiered = std::is_same_v<StoreT<block_t>, sn::oram::zingoram::storage::tiered_store<block_t>>;
  using traits_t = std::conditional_t<
      store_is_tiered,
      sn::oram::zingoram::traits<BlockBytes, mode_v, StoreT, sn::util::demo::posix_cache_backend_factory>,
      sn::oram::zingoram::traits<BlockBytes, mode_v, StoreT>>;
#else
  using traits_t = sn::oram::zingoram::traits<BlockBytes, mode_v, StoreT>;
#endif
  typename traits_t::options_t opts{};
  opts.block_count = cli_opts.block_count;
  opts.bucket_real_size = cli_opts.bucket_real;
  opts.bucket_dummy_size = cli_opts.bucket_dummy;
  opts.eviction_rate = cli_opts.eviction_rate;
  opts.routing_depth = cli_opts.routing_depth;
  opts.evict_batch = cli_opts.evict_batch;
  opts.access_concurrency = std::max<std::uint32_t>(cli_opts.access_concurrency, 1);
  opts.hot_memory_budget_bytes = cli_opts.hot_budget_bytes;
  opts.cache_memory_budget_bytes = cli_opts.cache_budget_bytes;
  opts.backend_cache_budget_bytes = cli_opts.backend_cache_budget_bytes;
  opts.cache_pack_factor = cli_opts.cache_pack_factor;
  opts.cache_path = cli_opts.cache_path;

  std::optional<std::size_t> disjoint_window = cli_opts.disjoint_window;
  if constexpr (mode_v == sn::oram::zingoram::epoch_mode::disjoint_epoch) {
    std::size_t window_value = 0;
    if (cli_opts.online_only) {
      window_value = require_zingoram_online_only_window(opts, cli_opts.accesses);
      disjoint_window = window_value;
    } else {
      window_value = disjoint_window.value();
    }
    opts.disjoint_epoch_window = window_value;
  } else {
    opts.disjoint_epoch_window = 0;
  }

  const auto thread_plan = sn::oram::zingoram::resolve_threading<mode_v>(
      sn::oram::zingoram::threading_input{
          .access = static_cast<std::size_t>(opts.access_concurrency),
          .eviction = cli_opts.eviction_threads,
          .routing_depth = opts.routing_depth,
          .online_only = cli_opts.online_only,
      }
  );
  opts.access_concurrency = static_cast<std::uint32_t>(thread_plan.access.logical);

  std::optional<sn::threads::pthread_thread_pool> access_pool;
  std::optional<sn::threads::pthread_thread_pool> eviction_pool;
  std::optional<sn::threads::pthread_thread_pool> domain_pool;
  sn::threads::thread_pool* access_pool_ptr = nullptr;
  std::optional<sn::threads::thread_team> eviction_team;

  if (thread_plan.domain.has_value()) {
    domain_pool.emplace(threads, thread_plan.domain->background, "oram-domain");
    access_pool_ptr = thread_plan.domain->background > 0 ? &domain_pool->pool() : nullptr;
    eviction_team.emplace(domain_pool->pool(), thread_plan.eviction.logical);
  } else {
    if (thread_plan.access.background > 0) {
      access_pool.emplace(threads, thread_plan.access.background, "oram-access");
      access_pool_ptr = &access_pool->pool();
    }
    eviction_pool.emplace(threads, thread_plan.eviction.background, "oram-evict");
    eviction_team.emplace(eviction_pool->pool(), thread_plan.eviction.logical);
  }

  using client_t = sn::oram::zingoram::client<traits_t>;
  client_t client(opts, std::move(*eviction_team));
  client.initialize();
  if (cli_opts.online_only && eviction_pool.has_value()) {
    eviction_pool->pool().park();
  }
  log_summary<traits_t>(client, cli_opts.accesses, label, cli_opts.action);

#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (store_is_tiered) {
    log_cache_stats(client.state_ref(), "init");
    client.state_ref().reset_cache_stats();
  }
#endif

  dispatch_action<traits_t>(
      cli_opts.action, client, cli_opts.accesses, label, disjoint_window, access_pool_ptr, thread_plan.access.logical,
      cli_opts.online_only
  );

#if defined(SONIC_ORAM_TIERED_STORAGE)
  if constexpr (store_is_tiered) {
    log_cache_stats(client.state_ref(), "access");

    std::error_code ec;
    std::filesystem::remove(cli_opts.cache_path, ec);
    (void) ec; // best effort
  }
#endif
}

template <sn::oram::zingoram::epoch_mode mode_v, template <typename, typename...> class StoreT>
void run_zingoram_mode(
    args::Subparser& parser, const char* label, bool allow_hot_budget, sn::threads::thread_context& threads
) {
  const bool disjoint_mode = mode_v == sn::oram::zingoram::epoch_mode::disjoint_epoch;
  auto cli_opts = parse_zingoram_cli(parser, disjoint_mode, allow_hot_budget);

  dispatch_block_bytes_or_throw<zingoram_supported_block_sizes>(
      cli_opts.block_bytes,
      [&](auto size_tag) {
        constexpr std::size_t BlockBytes = decltype(size_tag)::value;
        run_zingoram_mode_impl<BlockBytes, mode_v, StoreT>(cli_opts, label, threads);
      },
      label
  );
}

void run_zingoram(args::Subparser& parser, sn::threads::thread_context& threads) {
  run_zingoram_mode<sn::oram::zingoram::epoch_mode::default_epoch, sn::oram::zingoram::storage::slab_store>(
      parser, "zingoram", /*allow_hot_budget=*/false, threads
  );
}

void run_zingoram_disjoint(args::Subparser& parser, sn::threads::thread_context& threads) {
  run_zingoram_mode<sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::slab_store>(
      parser, "zingoram-disjoint", /*allow_hot_budget=*/false, threads
  );
}

#if defined(SONIC_ORAM_TIERED_STORAGE)
void run_zingoram_tiered(args::Subparser& parser, sn::threads::thread_context& threads) {
  run_zingoram_mode<sn::oram::zingoram::epoch_mode::default_epoch, sn::oram::zingoram::storage::tiered_store>(
      parser, "zingoram-tiered", /*allow_hot_budget=*/true, threads
  );
}

void run_zingoram_disjoint_tiered(args::Subparser& parser, sn::threads::thread_context& threads) {
  run_zingoram_mode<sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::tiered_store>(
      parser, "zingoram-disjoint-tiered", /*allow_hot_budget=*/true, threads
  );
}
#endif

} // namespace

int main(int argc, char** argv) {
  sn::prof::set_thread_name("main");
  args::ArgumentParser parser("sonic oram demo", "oram demo");
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

  args::Command path_cmd(commands, "pathoram", "pathoram", with_logging([](args::Subparser& sub, auto&) {
                           run_pathoram(sub);
                         }));
  args::Command zing_cmd(commands, "zingoram", "zingoram", with_logging(run_zingoram));
  args::Command zing_disjoint_cmd(
      commands, "zingoram-disjoint", "zingoram disjoint", with_logging(run_zingoram_disjoint)
  );
#if defined(SONIC_ORAM_TIERED_STORAGE)
  args::Command zing_tiered_cmd(commands, "zingoram-tiered", "zingoram tiered", with_logging(run_zingoram_tiered));
  args::Command zing_disjoint_tiered_cmd(
      commands, "zingoram-disjoint-tiered", "zingoram disjoint tiered", with_logging(run_zingoram_disjoint_tiered)
  );
#endif

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

  if (
      !path_cmd && !zing_cmd && !zing_disjoint_cmd
#if defined(SONIC_ORAM_TIERED_STORAGE)
      && !zing_tiered_cmd && !zing_disjoint_tiered_cmd
#endif
  ) {
    std::cerr << "no command specified, use --help for usage" << '\n';
    return 1;
  }

  return 0;
}
