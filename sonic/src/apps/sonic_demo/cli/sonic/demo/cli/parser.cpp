#include "sonic/demo/cli/parser.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "sonic/demo/types/string_buffer.hpp"
#include "sonic/oram/pathoram/traits.hpp"
#include "sonic/util/cli/thread_options.hpp"
#include "sonic/util/ext/args.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/log.hpp"

namespace sn::demo::cli {
namespace {

constexpr std::size_t kOramBlockBytes = 64;
constexpr const char* kDefaultCachePath = "/tmp/sonic_oram_sgx_cache.dat";

types::oram_action parse_oram_action(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (lowered == "validate") {
    return types::oram_action::validate;
  }
  if (lowered == "experiment") {
    return types::oram_action::experiment;
  }
  if (lowered == "benchmark") {
    return types::oram_action::benchmark;
  }
  throw args::Error("unknown action");
}

void configure_hello(args::Subparser& subparser, types::command_intent& intent) {
  args::Positional<std::string> name_arg(subparser, "name", "name", "world");
  args::ValueFlag<std::uint32_t> repeat_arg(subparser, "count", "greetings", {'c', "count"}, 1);
  args::Flag enthusiasm_flag(subparser, "enthusiastic", "add excitement", {'e', "enthusiastic"});

  subparser.Parse();

  types::hello_intent hello{};
  const std::string name_value = args::get(name_arg);
  if (!hello.name.assign(name_value)) {
    throw args::Error("name is too long");
  }
  hello.repeat = args::get(repeat_arg);
  hello.enthusiastic = enthusiasm_flag.Get() ? 1u : 0u;

  intent.tag = types::command_tag::hello;
  intent.hello = hello;
}

void configure_parallel_scan(args::Subparser& subparser, types::command_intent& intent) {
  args::ValueFlag<std::uint32_t> items_arg(subparser, "items", "items", {'n', "items"}, 1024);
  args::ValueFlag<std::uint32_t> threads_arg(subparser, "threads", "worker threads; 0 for auto", {'T', "threads"}, 0);

  subparser.Parse();

  const std::uint32_t items = args::get(items_arg);
  if (items == 0) {
    throw args::Error("items must be > 0");
  }

  types::parallel_scan_intent scan{};
  scan.elements = items;
  scan.requested_workers = args::get(threads_arg);

  intent.tag = types::command_tag::parallel_scan;
  intent.parallel_scan = scan;
}

void configure_pathoram(args::Subparser& subparser, types::command_intent& intent) {
  args::Positional<std::string> action_arg(subparser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::uint64_t> block_count_flag(
      subparser, "block-count", "block count", {'N', "block-count"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> block_bytes_flag(
      subparser, "block-bytes", "block size", {'B', "block-bytes"}, kOramBlockBytes
  );
  args::ValueFlag<std::size_t> bucket_size_flag(subparser, "bucket-size", "bucket size", {'Z', "bucket-size"}, 4);
  args::ValueFlag<std::size_t> accesses_flag(subparser, "accesses", "accesses", {'m', "accesses"}, 0);

  subparser.Parse();

  const auto action = parse_oram_action(args::get(action_arg));
  const auto block_count = args::get(block_count_flag);
  const auto block_bytes = args::get(block_bytes_flag);
  const auto accesses = args::get(accesses_flag);

  if (block_bytes != 64 && block_bytes != 256) {
    throw args::Error("pathoram block size must be 64 or 256 bytes");
  }
  if (args::get(bucket_size_flag) != sn::oram::pathoram::traits<64>::bucket_size) {
    throw args::Error("bucket size must match pathoram build");
  }

  types::pathoram_intent path{};
  path.action = action;
  path.block_bytes = static_cast<std::uint32_t>(block_bytes);
  path.block_count = block_count;
  path.accesses = accesses;

  intent.tag = types::command_tag::pathoram;
  intent.pathoram = path;
}

void configure_zingoram(
    args::Subparser& subparser, types::command_intent& intent, bool disjoint_mode, bool tiered_mode
) {
  args::Positional<std::string> action_arg(subparser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::uint64_t> block_count_flag(
      subparser, "block-count", "block count", {'N', "block-count"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> block_bytes_flag(
      subparser, "block-bytes", "block size", {'B', "block-bytes"}, kOramBlockBytes
  );
  args::ValueFlag<std::uint32_t> bucket_real_flag(
      subparser, "bucket-real", "real bucket size", {'Z', "bucket-real"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> bucket_dummy_flag(
      subparser, "bucket-dummy", "dummy bucket size", {'S', "bucket-dummy"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> eviction_rate_flag(
      subparser, "eviction-rate", "eviction rate", {'A', "eviction-rate"}, 0
  );
  args::ValueFlag<std::uint32_t> routing_depth_flag(subparser, "routing-depth", "routing depth", {"routing-depth"}, 0);
  args::ValueFlag<std::uint32_t> evict_batch_flag(subparser, "evict-batch", "eviction batch size", {"evict-batch"}, 1);
  args::ValueFlag<std::uint32_t> access_concurrency_flag(
      subparser, "access-concurrency", "access concurrency", {'T', "access-concurrency"}, 1
  );
  args::ValueFlag<std::size_t> accesses_flag(subparser, "accesses", "accesses", {'m', "accesses"}, 0);
  args::ValueFlag<std::uint32_t> eviction_threads_flag(
      subparser, "evict-threads", "evict threads; 0 for auto", {"evict-threads"}, 0
  );

  std::optional<std::uint64_t> disjoint_window;
  bool online_only_mode = false;
  std::unique_ptr<args::ValueFlag<std::uint64_t>> disjoint_flag;
  std::unique_ptr<args::Flag> online_only_flag;
  std::unique_ptr<args::ValueFlag<std::string>> hot_budget_flag;
  std::unique_ptr<args::ValueFlag<std::string>> cache_budget_flag;
  std::unique_ptr<args::ValueFlag<std::string>> backend_cache_budget_flag;
  std::unique_ptr<args::ValueFlag<std::uint32_t>> cache_pack_factor_flag;
  std::unique_ptr<args::ValueFlag<std::string>> cache_path_flag;

  if (disjoint_mode) {
    disjoint_flag.reset(
        new args::ValueFlag<std::uint64_t>(subparser, "disjoint-epoch", "epoch window", {"disjoint-epoch"})
    );
    online_only_flag.reset(new args::Flag(subparser, "online-only", "skip epoch flush", {"online-only"}));
  }

  if (tiered_mode) {
    hot_budget_flag.reset(
        new args::ValueFlag<std::string>(subparser, "hot-budget", "hot-tier budget", {"hot-budget"}, "0")
    );
    cache_budget_flag.reset(
        new args::ValueFlag<std::string>(subparser, "cache-budget", "cache budget", {"cache-budget"}, "0")
    );
    backend_cache_budget_flag.reset(new args::ValueFlag<std::string>(
        subparser, "backend-cache-budget", "backend cache budget", {"backend-cache-budget"}, "0"
    ));
    cache_pack_factor_flag.reset(new args::ValueFlag<std::uint32_t>(
        subparser, "cache-pack-factor", "cache pack factor", {"cache-pack-factor"}, 1
    ));
    cache_path_flag.reset(
        new args::ValueFlag<std::string>(subparser, "cache-path", "cache path", {"cache-path"}, kDefaultCachePath)
    );
  }

  subparser.Parse();

  if (disjoint_mode) {
    online_only_mode = online_only_flag->Get();
    if (disjoint_flag->operator bool()) {
      disjoint_window = static_cast<std::uint64_t>(args::get(*disjoint_flag));
    }
    if (online_only_mode && disjoint_window.has_value()) {
      throw args::Error("use --online-only or --disjoint-epoch");
    }
    if (!online_only_mode && !disjoint_window.has_value()) {
      throw args::Error("set --disjoint-epoch or --online-only");
    }
  }

  const auto action = parse_oram_action(args::get(action_arg));
  if (disjoint_mode && online_only_mode && action == types::oram_action::validate) {
    throw args::Error("--online-only needs experiment or benchmark");
  }

  types::zingoram_intent zing{};
  zing.action = action;
  zing.block_count = args::get(block_count_flag);
  zing.block_bytes = static_cast<std::uint32_t>(args::get(block_bytes_flag));
  zing.bucket_real = args::get(bucket_real_flag);
  zing.bucket_dummy = args::get(bucket_dummy_flag);
  zing.eviction_rate = args::get(eviction_rate_flag);
  zing.routing_depth = args::get(routing_depth_flag);
  zing.evict_batch = args::get(evict_batch_flag);
  zing.access_concurrency = args::get(access_concurrency_flag);
  zing.accesses = args::get(accesses_flag);
  zing.eviction_threads = args::get(eviction_threads_flag);
  zing.mode = disjoint_mode ? 1u : 0u;
  zing.online_only = online_only_mode ? 1u : 0u;
  zing.disjoint_window_present = disjoint_window.has_value() ? 1u : 0u;
  zing.disjoint_window = disjoint_window.value_or(0);
  zing.tiered = tiered_mode ? 1u : 0u;

  if (tiered_mode) {
    const auto hot_budget = sn::util::humanize::parse_bytes(args::get(*hot_budget_flag));
    const auto cache_budget = sn::util::humanize::parse_bytes(args::get(*cache_budget_flag));
    const auto backend_cache_budget = sn::util::humanize::parse_bytes(args::get(*backend_cache_budget_flag));
    const auto pack_factor = args::get(*cache_pack_factor_flag);
    const std::string cache_path_value = args::get(*cache_path_flag);
    if (cache_budget == 0) {
      throw args::Error("tiered runs need --cache-budget");
    }
    if (pack_factor == 0) {
      throw args::Error("--cache-pack-factor must be > 0");
    }
    if (cache_path_value.empty()) {
      throw args::Error("tiered runs need --cache-path");
    }
    zing.hot_budget_bytes = hot_budget;
    zing.cache_budget_bytes = cache_budget;
    zing.backend_cache_budget_bytes = backend_cache_budget;
    zing.cache_pack_factor = pack_factor;
    if (!zing.cache_path.assign(cache_path_value)) {
      throw args::Error("cache path too long");
    }
  } else {
    zing.hot_budget_bytes = 0;
    zing.cache_budget_bytes = 0;
    zing.backend_cache_budget_bytes = 0;
    zing.cache_pack_factor = 1;
    zing.cache_path.clear();
  }

  intent.tag = types::command_tag::zingoram;
  intent.zingoram = zing;
}

void configure_o2th(args::Subparser& subparser, types::command_intent& intent) {
  args::Positional<std::string> action_arg(subparser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::size_t> dataset_size_flag(
      subparser, "dataset-size", "dataset size", {'Y', "dataset-size"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> request_count_flag(
      subparser, "request-count", "request count", {'M', "request-count"}, 0
  );
  args::ValueFlag<std::size_t> block_bytes_flag(
      subparser, "block-bytes", "block size", {'B', "block-bytes"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> bucket_size_flag(subparser, "bucket-size", "bucket size", {'Z', "bucket-size"}, 64);
  args::ValueFlag<std::size_t> worker_parallelism_flag(
      subparser, "worker-parallelism", "workers", {'W', "worker-parallelism"}, 1
  );
  args::ValueFlag<std::size_t> access_concurrency_flag(
      subparser, "access-concurrency", "access concurrency", {'C', "access-concurrency"}, 0
  );
  args::ValueFlag<std::size_t> batches_flag(subparser, "batches", "batches", {'m', "batches"}, 1);
  args::ValueFlag<double> write_ratio_flag(subparser, "write-ratio", "write ratio", {"write-ratio"}, 0.5);

  subparser.Parse();

  types::o2th_intent o2th{};
  o2th.action = parse_oram_action(args::get(action_arg));
  const auto dataset_size = args::get(dataset_size_flag);
  if (dataset_size == 0) {
    throw args::Error("dataset size must be > 0");
  }
  o2th.dataset_size = static_cast<std::uint64_t>(dataset_size);

  std::size_t request_count = args::get(request_count_flag);
  if (request_count == 0) {
    request_count = dataset_size;
  }
  if (request_count == 0) {
    throw args::Error("request count must be > 0");
  }
  if (o2th.action == types::oram_action::validate && request_count != dataset_size) {
    throw args::Error("validate mode needs request-count to match dataset-size");
  }
  o2th.request_count = static_cast<std::uint64_t>(request_count);

  o2th.block_size = static_cast<std::uint32_t>(args::get(block_bytes_flag));
  o2th.bucket_size = static_cast<std::uint32_t>(args::get(bucket_size_flag));
  o2th.worker_parallelism = static_cast<std::uint32_t>(args::get(worker_parallelism_flag));
  o2th.access_concurrency = static_cast<std::uint32_t>(args::get(access_concurrency_flag));
  const auto batches = args::get(batches_flag) == 0 ? std::size_t{1} : args::get(batches_flag);
  o2th.batch_count = static_cast<std::uint32_t>(batches);

  const double write_ratio = args::get(write_ratio_flag);
  if (write_ratio < 0.0 || write_ratio > 1.0) {
    throw args::Error("write ratio out of range");
  }
  o2th.write_ratio = write_ratio;

  if (o2th.bucket_size < 64) {
    throw args::Error("bucket size must be >= 64");
  }

  intent.tag = types::command_tag::o2th;
  intent.o2th = o2th;
}

void configure_pmchain(args::Subparser& subparser, types::command_intent& intent) {
  args::Positional<std::string> action_arg(subparser, "action", "run mode", args::Options::Required);
  args::ValueFlag<std::size_t> block_count_flag(
      subparser, "block-count", "block count", {'N', "block-count"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> batch_size_flag(
      subparser, "batch-size", "batch size", {'m', "batch-size"}, args::Options::Required
  );
  args::ValueFlag<std::size_t> block_bytes_flag(
      subparser, "block-bytes", "block size", {'B', "block-bytes"}, kOramBlockBytes
  );
  args::ValueFlag<std::size_t> split_factor_flag(subparser, "split-factor", "split factor", {"split-factor"}, 1);
  args::ValueFlag<std::size_t> posmap_bucket_flag(subparser, "posmap-bucket", "posmap bucket", {"posmap-bucket"}, 64);
  args::ValueFlag<std::size_t> access_workers_flag(
      subparser, "access-workers", "access workers", {'W', "access-workers"}, 1
  );
  args::ValueFlag<std::size_t> oram_parallel_flag(
      subparser, "oram-parallelism", "oram workers; 0 for access", {"oram-parallelism"}, 0
  );
  args::ValueFlag<std::uint32_t> bucket_real_flag(
      subparser, "bucket-real", "bucket real", {'Z', "bucket-real"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> bucket_dummy_flag(
      subparser, "bucket-dummy", "bucket dummy", {'S', "bucket-dummy"}, args::Options::Required
  );
  args::ValueFlag<std::uint32_t> routing_depth_flag(
      subparser, "routing-depth", "routing depth", {'R', "routing-depth"}, 0
  );
  args::ValueFlag<std::uint32_t> evict_batch_flag(subparser, "evict-batch", "evict batch", {"evict-batch"}, 1);
  args::ValueFlag<std::size_t> eviction_threads_flag(
      subparser, "evict-threads", "evict threads; 0 for auto", {"evict-threads"}, 0
  );
  args::Flag tiered_flag(subparser, "tiered-storage", "use tiered storage", {"tiered-storage"});
  args::ValueFlag<std::string> hot_budget_flag(subparser, "hot-budget", "hot-tier budget", {"hot-budget"}, "0");
  args::ValueFlag<std::string> cache_budget_flag(subparser, "cache-budget", "cache budget", {"cache-budget"}, "0");
  args::ValueFlag<std::string> backend_cache_budget_flag(
      subparser, "backend-cache-budget", "backend cache budget", {"backend-cache-budget"}, "0"
  );
  args::ValueFlag<std::uint32_t> cache_pack_factor_flag(
      subparser, "cache-pack-factor", "cache pack factor", {"cache-pack-factor"}, 1
  );
  args::ValueFlag<std::string> cache_path_flag(
      subparser, "cache-path", "cache path", {"cache-path"}, kDefaultCachePath
  );
  args::ValueFlag<std::uint64_t> disjoint_epoch_flag(
      subparser, "disjoint-epoch", "epoch window; 0 for auto", {"disjoint-epoch"}, 0
  );
  args::Flag drop_epoch_flag(subparser, "drop-epoch", "drop pending epoch", {"drop-epoch"});
  args::ValueFlag<std::size_t> batches_flag(subparser, "batches", "batches", {"batches"}, 1);
  args::ValueFlag<double> write_ratio_flag(subparser, "write-ratio", "write ratio", {"write-ratio"}, 0.5);
  args::ValueFlag<double> dummy_ratio_flag(subparser, "dummy-ratio", "dummy ratio", {"dummy-ratio"}, 0.0);

  subparser.Parse();

  types::pmchain_intent pmchain{};
  pmchain.action = parse_oram_action(args::get(action_arg));
  pmchain.block_count = static_cast<std::uint64_t>(args::get(block_count_flag));
  pmchain.batch_size = static_cast<std::uint64_t>(args::get(batch_size_flag));
  pmchain.block_bytes = static_cast<std::uint32_t>(args::get(block_bytes_flag));
  pmchain.split_factor = static_cast<std::uint32_t>(args::get(split_factor_flag));
  pmchain.posmap_bucket_size = static_cast<std::uint32_t>(args::get(posmap_bucket_flag));
  pmchain.access_workers = static_cast<std::uint32_t>(args::get(access_workers_flag));
  pmchain.oram_parallelism = static_cast<std::uint32_t>(args::get(oram_parallel_flag));
  pmchain.bucket_real_size = args::get(bucket_real_flag);
  pmchain.bucket_dummy_size = args::get(bucket_dummy_flag);
  pmchain.routing_depth = args::get(routing_depth_flag);
  pmchain.evict_batch = args::get(evict_batch_flag);
  pmchain.eviction_threads = static_cast<std::uint32_t>(args::get(eviction_threads_flag));
  pmchain.disjoint_epoch_window = args::get(disjoint_epoch_flag);
  pmchain.drop_epoch = drop_epoch_flag.Get() ? 1u : 0u;
  pmchain.tiered = tiered_flag.Get() ? 1u : 0u;
  pmchain.hot_budget_bytes = sn::util::humanize::parse_bytes(args::get(hot_budget_flag));
  pmchain.cache_budget_bytes = sn::util::humanize::parse_bytes(args::get(cache_budget_flag));
  pmchain.backend_cache_budget_bytes = sn::util::humanize::parse_bytes(args::get(backend_cache_budget_flag));
  pmchain.cache_pack_factor = args::get(cache_pack_factor_flag);
  const std::string cache_path_value = args::get(cache_path_flag);
  if (!pmchain.cache_path.assign(cache_path_value)) {
    throw args::Error("cache path too long");
  }
  const auto batches = args::get(batches_flag) == 0 ? std::size_t{1} : args::get(batches_flag);
  pmchain.batch_count = static_cast<std::uint32_t>(batches);

  const double write_ratio = args::get(write_ratio_flag);
  const double dummy_ratio = args::get(dummy_ratio_flag);
  if (write_ratio < 0.0 || write_ratio > 1.0) {
    throw args::Error("write ratio out of range");
  }
  if (dummy_ratio < 0.0 || dummy_ratio > 1.0) {
    throw args::Error("dummy ratio out of range");
  }
  pmchain.write_ratio = write_ratio;
  pmchain.dummy_ratio = dummy_ratio;

  if (pmchain.split_factor == 0) {
    throw args::Error("split factor must be > 0");
  }

  if (pmchain.tiered != 0) {
    if (pmchain.cache_budget_bytes == 0) {
      throw args::Error("tiered runs need --cache-budget");
    }
    if (pmchain.cache_pack_factor == 0) {
      throw args::Error("--cache-pack-factor must be > 0");
    }
    if (pmchain.cache_path.empty()) {
      throw args::Error("tiered runs need --cache-path");
    }
  } else {
    pmchain.hot_budget_bytes = 0;
    pmchain.cache_budget_bytes = 0;
    pmchain.backend_cache_budget_bytes = 0;
    pmchain.cache_pack_factor = 1;
    pmchain.cache_path.clear();
  }
  if (pmchain.action == types::oram_action::benchmark) {
    throw args::Error("benchmark mode unavailable for pmchain");
  }

  intent.tag = types::command_tag::pmchain;
  intent.pmchain = pmchain;
}

}

parse_result parse_command_line(int argc, const char** argv) {
  parse_result result{};

  args::ArgumentParser parser("sonic-demo", "demo tools");
  parser.helpParams.showTerminator = false;
  parser.SetArgumentSeparations(false, false, true, true);

  args::Group global(parser, "global options");
  args::HelpFlag help_flag(global, "help", "show help", {'h', "help"});
  args::CounterFlag verbose_flag(global, "verbose", "more logging", {'v', "verbose"});
  sn::util::cli::thread_option_flags thread_flags(
      global, sn::threads::thread_policy{.affinity = sn::threads::thread_affinity::inherit}
  );

  args::Group commands(parser, "commands");
  args::Command hello(commands, "hello", "greet", [&](args::Subparser& subparser) {
    configure_hello(subparser, result.intent);
  });
  args::Command parallel(commands, "parallel", "parallel scan", [&](args::Subparser& subparser) {
    configure_parallel_scan(subparser, result.intent);
  });
  args::Command pathoram(commands, "pathoram", "pathoram", [&](args::Subparser& subparser) {
    configure_pathoram(subparser, result.intent);
  });
  args::Command zingoram(commands, "zingoram", "zingoram", [&](args::Subparser& subparser) {
    configure_zingoram(subparser, result.intent, false, false);
  });
  args::Command zingoram_disjoint(commands, "zingoram-disjoint", "zingoram disjoint", [&](args::Subparser& subparser) {
    configure_zingoram(subparser, result.intent, true, false);
  });
  args::Command zingoram_tiered(commands, "zingoram-tiered", "zingoram tiered", [&](args::Subparser& subparser) {
    configure_zingoram(subparser, result.intent, false, true);
  });
  args::Command zingoram_disjoint_tiered(
      commands, "zingoram-disjoint-tiered", "zingoram disjoint tiered",
      [&](args::Subparser& subparser) { configure_zingoram(subparser, result.intent, true, true); }
  );
  args::Command o2th(commands, "o2th", "o2th rwkv", [&](args::Subparser& subparser) {
    configure_o2th(subparser, result.intent);
  });
  args::Command pmchain(commands, "pmchain", "pmchain", [&](args::Subparser& subparser) {
    configure_pmchain(subparser, result.intent);
  });

  try {
    parser.ParseCLI(argc, argv);
  } catch (const args::Help&) {
    std::cout << parser;
    result.show_help = true;
    result.success = true;
    return result;
  } catch (const args::Error& ex) {
    std::cerr << ex.what() << '\n';
    std::cerr << parser;
    result.success = false;
    return result;
  }

  if (result.intent.tag == types::command_tag::none) {
    std::cerr << "no command specified" << '\n';
    std::cerr << parser;
    result.success = false;
    return result;
  }

  result.verbosity = static_cast<std::uint32_t>(verbose_flag.Get());
  result.thread_policy = thread_flags.resolve();
  result.success = true;
  return result;
}

void apply_logging_preferences(const parse_result& result) {
  auto& root_logger = sn::util::log::global_logger();
  const int base = static_cast<int>(sn::util::log::level::info);
  const int max_level = static_cast<int>(sn::util::log::level::annoying);
  int requested = base + static_cast<int>(result.verbosity);
  if (requested > max_level) {
    requested = max_level;
  }
  root_logger.set_verbosity(static_cast<sn::util::log::level>(requested));
}

}
