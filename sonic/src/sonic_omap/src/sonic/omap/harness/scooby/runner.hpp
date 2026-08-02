#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "sonic/crypto/prng.hpp"
#include "sonic/omap/harness/lbrouter/validate.hpp"
#include "sonic/omap/lbrouter/client.hpp"
#include "sonic/omap/lbrouter/helpers.hpp"
#include "sonic/omap/pmchain/threading.hpp"
#include "sonic/omap/harness/scooby/types.hpp"
#include "sonic/omap/suboram/pmchain_driver.hpp"
#include "sonic/omap/suboram/o2th_driver.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::harness::scooby {

namespace detail {

template <std::size_t PayloadBytes> struct payload_traits {
  using router_type = sn::omap::lbrouter::lbrouter<sn::omap::harness::scooby::key_type, PayloadBytes>;
  using router_request = typename router_type::request;
  using maybe_request = typename router_type::template maybe_dummy<router_request>;
  using payload_buffer = sn::omap::detail::block_data_buffer<PayloadBytes>;
};

template <typename T> inline constexpr bool unsupported_suboram_config_v = false;

template <std::size_t PayloadBytes, typename SubOramConfig> class suboram_endpoint {
  static_assert(unsupported_suboram_config_v<SubOramConfig>, "unsupported suboram config");
};

template <std::size_t PayloadBytes> class suboram_endpoint<PayloadBytes, sn::omap::suboram::pmchain::config> {
public:
  using config_type = sn::omap::suboram::pmchain::config;
  using driver_type = sn::omap::suboram::pmchain::driver<sn::omap::harness::scooby::key_type, PayloadBytes>;

  suboram_endpoint(
      const config_type& cfg, const sn::threads::thread_context& threads, std::size_t workers_per_suboram
  ) :
      thread_plan_(
          sn::omap::pmchain::resolve_threading(
              std::max<std::size_t>(workers_per_suboram, std::size_t{1}), cfg.oram_parallelism,
              std::max<std::size_t>(cfg.eviction_threads, std::size_t{1})
          )
      ),
      domain_pool_(threads, thread_plan_.domain.background, "scooby-pmchain"),
      driver_(
          cfg, sn::threads::thread_team(domain_pool_.pool(), thread_plan_.eviction.logical),
          sn::threads::thread_team(domain_pool_.pool(), thread_plan_.access.logical)
      ) {}

  [[nodiscard]] std::size_t block_count() const noexcept { return driver_.block_count(); }
  [[nodiscard]] std::size_t batch_size() const noexcept { return driver_.batch_size(); }

  template <typename RoutedSpan> void process_bin(RoutedSpan bin) { driver_.process_bin(bin); }
  void maintenance() { driver_.maintenance(); }

private:
  sn::omap::pmchain::threading thread_plan_{};
  sn::threads::pthread_thread_pool domain_pool_;
  driver_type driver_;
};

template <std::size_t PayloadBytes> class suboram_endpoint<PayloadBytes, sn::omap::suboram::o2th::config> {
public:
  using config_type = sn::omap::suboram::o2th::config;
  using driver_type = sn::omap::suboram::o2th::driver<sn::omap::harness::scooby::key_type, PayloadBytes>;

  suboram_endpoint(
      const config_type& cfg, const sn::threads::thread_context& threads, std::size_t workers_per_suboram
  ) :
      worker_pool_(threads, workers_per_suboram > 0 ? workers_per_suboram - 1 : 0, "scooby-o2th"),
      driver_(cfg, sn::threads::thread_team(worker_pool_.pool(), std::max<std::size_t>(workers_per_suboram, 1))) {}

  [[nodiscard]] std::size_t block_count() const noexcept { return driver_.block_count(); }
  [[nodiscard]] std::size_t batch_size() const noexcept { return driver_.batch_size(); }

  template <typename RoutedSpan> void process_bin(RoutedSpan bin) { driver_.process_bin(bin); }
  void maintenance() {} // no extra maintenance required for o2th driver

private:
  sn::threads::pthread_thread_pool worker_pool_;
  driver_type driver_;
};

// system configuration, based on suboram
template <std::size_t PayloadBytes, typename SubOramConfig> struct system_config {
  using traits = payload_traits<PayloadBytes>;
  typename traits::router_type::config router_cfg{};
  double lambda = 40.0;
  std::size_t router_workers = 0;
  SubOramConfig sub_cfg{};
  std::size_t suboram_count = 0;
  std::size_t workers_per_suboram = 1;
};

// full system runtime
template <std::size_t PayloadBytes, typename SubOramConfig> struct scooby_runtime {
  using traits = payload_traits<PayloadBytes>;
  using endpoint_type = suboram_endpoint<PayloadBytes, SubOramConfig>;

  system_config<PayloadBytes, SubOramConfig> cfg{};
  sn::threads::pthread_thread_pool router_workers;
  typename traits::router_type router;
  std::vector<std::unique_ptr<endpoint_type>> suborams;

  explicit scooby_runtime(
      const system_config<PayloadBytes, SubOramConfig>& cfg_in, const sn::threads::thread_context& threads
  ) :
      cfg(cfg_in),
      router_workers(threads, cfg.router_workers, "scooby-router"),
      router(
          cfg.router_cfg,
          sn::threads::thread_team(
              router_workers.pool(), sn::threads::logical_parallelism_from_background(cfg.router_workers)
          )
      ) {}
};

template <std::size_t PayloadBytes, typename SubOramConfig>
std::unique_ptr<scooby_runtime<PayloadBytes, SubOramConfig>> make_runtime(
    system_config<PayloadBytes, SubOramConfig> cfg, const sn::threads::thread_context& threads
) {
  using runtime_type = scooby_runtime<PayloadBytes, SubOramConfig>;
  using endpoint_type = typename runtime_type::endpoint_type;
  sn::util::log::ensure(cfg.router_cfg.batch_size > 0, "scooby: batch size must be positive");
  sn::util::log::ensure(cfg.suboram_count > 0, "scooby: suboram count must be positive");

  cfg.router_cfg.security_parameter_lambda = cfg.lambda;
  const auto derived =
      sn::omap::lbrouter::compute_derived_config<sn::omap::harness::scooby::key_type, PayloadBytes>(cfg.router_cfg);
  sn::util::log::ensure(derived.bin_capacity > 0, "scooby: derived bin capacity must be positive");

  // compute padded batch based on suboram
  // pmchain needs multiple of posmap bucket size (cause we build on rq's)
  // o2th linear scans rqs so we can use any size
  const std::size_t bucket_size = sn::omap::harness::scooby::kScoobyPosmapBucketSize;
  sn::util::log::ensure(bucket_size > 0, "scooby: bucket size must be positive");
  sn::util::log::ensure(
      derived.bin_capacity <= (std::numeric_limits<std::size_t>::max() - (bucket_size - 1)),
      "scooby: bin capacity too large for padded batch"
  );
  const std::size_t padded_batch = ((derived.bin_capacity + bucket_size - 1) / bucket_size) * bucket_size;
  cfg.sub_cfg.batch_size = padded_batch;

  // tweak/finalize config based on suboram
  if constexpr (std::is_same_v<SubOramConfig, sn::omap::suboram::pmchain::config>) {
    // pmchain setup stuff
    sn::util::log::ensure(cfg.sub_cfg.block_count > 0, "scooby: suboram block count must be positive");
    sn::util::log::ensure(cfg.sub_cfg.bucket_real_size > 0, "scooby: bucket real size must be positive");
    sn::util::log::ensure(cfg.sub_cfg.bucket_dummy_size > 0, "scooby: bucket dummy size must be positive");
    sn::util::log::ensure(cfg.sub_cfg.eviction_threads > 0, "scooby: eviction thread count must be positive");

    if (cfg.sub_cfg.posmap_bucket_size == 0) {
      cfg.sub_cfg.posmap_bucket_size = sn::omap::harness::scooby::kScoobyPosmapBucketSize;
    }
    sn::util::log::ensure(
        cfg.sub_cfg.posmap_bucket_size == sn::omap::harness::scooby::kScoobyPosmapBucketSize,
        "scooby: posmap bucket size must be 64"
    );

    // configure disjoint epoch window for pmchain's oram
    if (cfg.sub_cfg.disjoint_epoch_window == 0) {
      cfg.sub_cfg.disjoint_epoch_window = static_cast<std::uint64_t>(cfg.sub_cfg.batch_size);
    } else {
      sn::util::log::ensure(
          cfg.sub_cfg.disjoint_epoch_window >= cfg.sub_cfg.batch_size,
          "scooby: disjoint epoch window must cover pmchain batch"
      );
    }

    // configure oram's access concurrency based on available workers
    cfg.sub_cfg.access_concurrency =
        static_cast<std::uint32_t>(std::max<std::size_t>(cfg.workers_per_suboram, std::size_t{1}));
  } else if constexpr (std::is_same_v<SubOramConfig, sn::omap::suboram::o2th::config>) {
    // o2th-specific setup
    sn::util::log::ensure(cfg.sub_cfg.block_count > 0, "scooby: suboram block count must be positive");
    if (cfg.sub_cfg.bucket_size == 0) {
      cfg.sub_cfg.bucket_size = sn::omap::harness::scooby::kScoobyPosmapBucketSize;
    }
  }

  // prepare full system runtime
  auto runtime = std::make_unique<runtime_type>(cfg, threads);

  // initialize router
  runtime->router.initialize();

  // initialize suborams
  runtime->suborams.reserve(cfg.suboram_count);
  for (std::size_t ix = 0; ix < cfg.suboram_count; ++ix) {
    runtime->suborams.emplace_back(std::make_unique<endpoint_type>(cfg.sub_cfg, threads, cfg.workers_per_suboram));
  }

  return runtime;
}

template <std::size_t PayloadBytes, typename SubOramConfig>
void prepare_batch(
    scooby_runtime<PayloadBytes, SubOramConfig>& runtime, sn::crypto::prng& prng, double dummy_ratio,
    double write_ratio, std::vector<typename payload_traits<PayloadBytes>::maybe_request>& batch
) {
  // construct a request batch, with keys pointing to their respective suborams
  const std::size_t batch_size = runtime.cfg.router_cfg.batch_size;
  const std::size_t sub_count = runtime.cfg.suboram_count;

  // clamp ratios for synthesizing request batch
  const double clamped_dummy = std::clamp(dummy_ratio, 0.0, 0.95);
  const double clamped_write = std::clamp(write_ratio, 0.0, 1.0);
  constexpr std::uint64_t kMaxThreshold = std::numeric_limits<std::uint64_t>::max();
  const auto ratio_to_threshold = [](double ratio) -> std::uint64_t {
    if (ratio <= 0.0) {
      return 0;
    }
    if (ratio >= 1.0) {
      return kMaxThreshold;
    }
    const long double scaled = static_cast<long double>(ratio) * static_cast<long double>(kMaxThreshold);
    return static_cast<std::uint64_t>(scaled);
  };
  const std::uint64_t dummy_threshold = ratio_to_threshold(clamped_dummy);
  const std::uint64_t write_threshold = ratio_to_threshold(clamped_write);

  // construct a synthetic batch of requests
  std::vector<std::size_t> next_keys(sub_count, 0);
  batch.resize(batch_size);
  for (std::size_t slot = 0; slot < batch_size; ++slot) {
    const std::size_t sub_index = slot % sub_count;
    auto& entry = batch[slot];
    entry.is_dummy = false;
    entry.value.suboram_index = static_cast<std::uint32_t>(sub_index);

    const std::size_t block_count = runtime.suborams[sub_index]->block_count();
    const std::size_t key_index = next_keys[sub_index] % block_count;
    next_keys[sub_index] = (next_keys[sub_index] + 1) % block_count;
    entry.value.key = static_cast<sn::omap::harness::scooby::key_type>(key_index);

    const bool force_real = slot < sub_count;
    bool is_dummy = (!force_real) && (prng.random_u64() < dummy_threshold);
    entry.is_dummy = is_dummy;
    if (is_dummy) {
      entry.value.is_write = false;
      entry.value.key = 0;
      entry.value.payload.fill(0);
      continue;
    }

    const bool is_write = prng.random_u64() < write_threshold;
    entry.value.is_write = is_write;
    if (is_write) {
      prng.random_bytes(entry.value.payload.data(), entry.value.payload.size());
    } else {
      entry.value.payload.fill(0);
    }
  }
}

// process a request batch through the system
template <std::size_t PayloadBytes, typename SubOramConfig>
void execute_batch(
    scooby_runtime<PayloadBytes, SubOramConfig>& runtime,
    sn::util::span<const typename payload_traits<PayloadBytes>::maybe_request> batch
) {
  // ingest the request batch from the client
  runtime.router.ingest_batch(batch);

  // process requests, deduplicating and routing to bins
  runtime.router.route();

  // hand each bin to be processed by a suboram
  for (std::size_t sub = 0; sub < runtime.suborams.size(); ++sub) {
    auto span = runtime.router.bin_view(sub);
    runtime.suborams[sub]->process_bin(span);
    runtime.suborams[sub]->maintenance();
  }
}

template <std::size_t PayloadBytes, typename SubOramConfig>
std::vector<typename payload_traits<PayloadBytes>::maybe_request> collect_responses(
    scooby_runtime<PayloadBytes, SubOramConfig>& runtime,
    sn::util::span<const typename payload_traits<PayloadBytes>::maybe_request> batch
) {
  using traits = payload_traits<PayloadBytes>;
  using maybe_request = typename traits::maybe_request;

  // oblivious reassemble responses back to client order
  std::vector<maybe_request> responses(batch.size());
  runtime.router.reassemble(batch, sn::util::span<maybe_request>(responses.data(), responses.size()));
  return responses;
}

template <std::size_t PayloadBytes>
void verify_responses(
    std::size_t suboram_index,
    sn::util::span<const typename payload_traits<PayloadBytes>::router_type::routed_slot> inputs,
    sn::util::span<const typename payload_traits<PayloadBytes>::router_type::routed_slot> outputs,
    sn::util::span<typename payload_traits<PayloadBytes>::payload_buffer> shadow_blocks
) {
  using traits = payload_traits<PayloadBytes>;
  using router_type = typename traits::router_type;
  using routed_slot = typename router_type::routed_slot;

  sn::util::log::ensure(inputs.size() == outputs.size(), "scooby: bin input/output size mismatch");
  const std::size_t block_count = shadow_blocks.size();
  sn::util::log::ensure(block_count > 0, "scooby: suboram must expose blocks");

  std::unordered_map<std::size_t, std::size_t> key_to_input;
  key_to_input.reserve(inputs.size());

  bool saw_dummy_input = false;
  std::size_t real_input_count = 0;
  for (std::size_t idx = 0; idx < inputs.size(); ++idx) {
    const routed_slot& slot = inputs[idx];
    sn::util::log::ensure(slot.item.suboram_index == suboram_index, "scooby: request routed to wrong suboram");
    if (slot.is_dummy) {
      saw_dummy_input = true;
      continue;
    }
    sn::util::log::ensure(!saw_dummy_input, "scooby: real request appears after dummy input");
    const std::size_t key = static_cast<std::size_t>(slot.item.key);
    sn::util::log::ensure(key < block_count, "scooby: request key out of bounds");
    const bool inserted = key_to_input.emplace(key, idx).second;
    sn::util::log::ensure(inserted, "scooby: duplicate key in input bin");
    ++real_input_count;
  }

  const auto describe_buffer = [](const typename traits::payload_buffer& buf) {
    constexpr std::size_t preview_bytes = 16;
    const std::size_t limit = std::min<std::size_t>(preview_bytes, buf.size());
    std::string hex;
    hex.reserve(limit * 2 + 3);
    for (std::size_t ix = 0; ix < limit; ++ix) {
      hex.append(pfm::format("%02x", buf[ix]));
    }
    if (buf.size() > preview_bytes) {
      hex.append("...");
    }
    return hex;
  };

  bool saw_dummy_output = false;
  std::size_t real_output_count = 0;
  for (std::size_t idx = 0; idx < outputs.size(); ++idx) {
    const routed_slot& slot = outputs[idx];
    sn::util::log::ensure(slot.item.suboram_index == suboram_index, "scooby: response routed to wrong suboram");
    if (slot.is_dummy) {
      saw_dummy_output = true;
      continue;
    }
    sn::util::log::ensure(!saw_dummy_output, "scooby: real response appears after dummy");
    const std::size_t key = static_cast<std::size_t>(slot.item.key);
    sn::util::log::ensure(key < block_count, "scooby: response key out of bounds");
    auto it = key_to_input.find(key);
    sn::util::log::ensure(it != key_to_input.end(), "scooby: response key missing from inputs");

    const routed_slot& request = inputs[it->second];
    sn::util::log::ensure(!request.is_dummy, "scooby: matched response with dummy input");
    sn::util::log::ensure(
        request.item.is_write == slot.item.is_write, "scooby: response write flag mismatches request"
    );

    if (request.item.is_write) {
      shadow_blocks[key] = request.item.payload;
    } else {
      const auto& expected = shadow_blocks[key];
      const bool match =
          std::equal(expected.begin(), expected.end(), slot.item.payload.begin(), slot.item.payload.end());
      if (!match) {
        const auto expected_hex = describe_buffer(expected);
        const auto actual_hex = describe_buffer(slot.item.payload);
        sn::util::log::failf(
            "scooby: read payload mismatch (sub=%zu key=%zu) expected=%s actual=%s", suboram_index, key, expected_hex,
            actual_hex
        );
      }
    }

    key_to_input.erase(it);
    ++real_output_count;
  }

  sn::util::log::ensure(real_input_count == real_output_count, "scooby: mismatched request/response count");
  sn::util::log::ensure(key_to_input.empty(), "scooby: missing response for input key");
}

} // namespace detail

template <std::size_t PayloadBytes>
using router_request = typename detail::payload_traits<PayloadBytes>::router_request;

template <std::size_t PayloadBytes> using maybe_request = typename detail::payload_traits<PayloadBytes>::maybe_request;

// type aliases
template <std::size_t PayloadBytes, typename SubOramConfig>
using system_config = detail::system_config<PayloadBytes, SubOramConfig>;

template <std::size_t PayloadBytes, typename SubOramConfig>
using scooby_runtime = detail::scooby_runtime<PayloadBytes, SubOramConfig>;

template <std::size_t PayloadBytes>
using pmchain_system_config = system_config<PayloadBytes, sn::omap::suboram::pmchain::config>;

template <std::size_t PayloadBytes>
using o2th_system_config = system_config<PayloadBytes, sn::omap::suboram::o2th::config>;

using detail::make_runtime;

} // namespace sn::omap::harness::scooby
