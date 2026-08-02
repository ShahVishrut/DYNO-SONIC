#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "sonic/crypto/hkdf.hpp"
#include "sonic/omap/lbrouter/helpers.hpp"
#include "sonic/omap/pmchain/util/zingoram_setup.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/scooby_node/types/context.hpp"
#include "sonic/scooby_omap/secure/config.hpp"
#include "sonic/scooby_omap/secure/session.hpp"
#include "sonic/scooby_omap/secure/types.hpp"
#include "sonic/util/demo/block_size_dispatch.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/config/layout.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/wire/payload.hpp"
#include "sonic/scooby_omap/wire/schema.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap {

using pmchain_supported_block_sizes = supported_payload_sizes;
using pmchain_supported_split_factors = sn::util::demo::static_param_list<std::size_t, 2>;

struct plan_config {
  scooby_omap_role role{scooby_omap_role::client};
  bool auto_assign_role{false};
  std::uint32_t role_index{0};
  bool secure_comm_key_override{false};
  std::array<std::uint8_t, 32> secure_comm_key{};
#if defined(SCOOBY_SECURE_COMM)
  secure::key_schedule secure_keys{};
#endif
  std::uint32_t epoch_count{1};
  std::uint32_t client_count{1};
  std::uint32_t load_balancer_count{1};
  std::uint32_t suboram_count{1};
  std::uint32_t lb_input_count{64};
  std::uint32_t payload_bytes{64};
  std::uint32_t suboram_block_count{1024};
  std::uint32_t router_lambda{40};
  scooby_omap_backend backend{scooby_omap_backend::o2th};
  std::uint32_t client_pipeline_depth{2};
  std::uint32_t lb_pipeline_depth{2};
  std::uint32_t suboram_pipeline_depth{1};
  std::uint32_t lbrouter_parallelism{1};
  std::uint32_t o2th_parallelism{1};
  std::uint32_t pmchain_access_parallelism{1};
  std::uint32_t pmchain_oram_parallelism{0};
  std::uint32_t pmchain_eviction_parallelism{1};
  std::uint32_t pmchain_bucket_real{4};
  std::uint32_t pmchain_bucket_dummy{4};
  std::uint32_t pmchain_routing_depth{1};
  std::uint32_t pmchain_evict_batch{1};
  bool pmchain_auto_epoch{false};
  bool pmchain_drop_epoch{false};
  std::uint64_t pmchain_disjoint_window{0};
  std::uint32_t pmchain_split_factor{1};
  std::size_t pmchain_logical_block_bytes{0};
  std::size_t pmchain_physical_block_bytes{0};
  std::size_t pmchain_physical_block_count{0};
  std::size_t pmchain_physical_batch_size{0};
  std::uint64_t pmchain_physical_disjoint_window{0};
  bool pmchain_tiered{false};
  std::uint64_t pmchain_hot_budget_bytes{0};
  std::uint64_t pmchain_cache_budget_bytes{0};
  std::uint64_t pmchain_backend_cache_budget_bytes{0};
  std::uint32_t pmchain_cache_pack_factor{1};
  std::string pmchain_cache_path{};
  std::size_t o2th_batch_size{0};
  std::size_t pmchain_batch_size{0};
  std::uint32_t pmchain_eviction_rate{0};
  std::uint32_t telemetry_interval_epochs{0};
  std::uint32_t stress_runtime_seconds{0};
  router_plan router{};
  layout_info layout{};

  [[nodiscard]] std::uint64_t total_requests_per_epoch() const noexcept { return layout.requests_per_epoch_total; }
  [[nodiscard]] std::uint64_t total_bins_per_epoch() const noexcept { return layout.bins_per_epoch_total; }
};

inline void validate_intent(const scooby_omap_intent& intent) {
  const auto supported_payloads = format_supported_payload_sizes();
  sn::util::log::ensuref(intent.epoch_count > 0, "scooby-omap: epochs must be > 0");
  sn::util::log::ensuref(intent.client_count > 0, "scooby-omap: client_count must be > 0");
  sn::util::log::ensuref(intent.load_balancer_count > 0, "scooby-omap: load_balancer_count must be > 0");
  sn::util::log::ensuref(intent.suboram_count > 0, "scooby-omap: suboram_count must be > 0");
  sn::util::log::ensuref(intent.lb_input_count > 0, "scooby-omap: lb_input_count must be > 0");
  sn::util::log::ensuref(intent.payload_bytes > 0, "scooby-omap: payload_bytes must be > 0");
  sn::util::log::ensuref(
      payload_supported(intent.payload_bytes), "scooby-omap: payload_bytes must be one of %s", supported_payloads
  );
  sn::util::log::ensuref(intent.suboram_block_count > 0, "scooby-omap: suboram_block_count must be > 0");
  sn::util::log::ensuref(intent.router_lambda > 0, "scooby-omap: router_lambda must be > 0");
  sn::util::log::ensuref(intent.client_pipeline_depth > 0, "scooby-omap: client pipeline must be > 0");
  sn::util::log::ensuref(intent.lb_pipeline_depth > 0, "scooby-omap: load balancer pipeline must be > 0");
  sn::util::log::ensuref(intent.suboram_pipeline_depth > 0, "scooby-omap: suboram pipeline must be > 0");
  sn::util::log::ensuref(intent.lbrouter_parallelism > 0, "scooby-omap: lbrouter_parallelism must be > 0");
  sn::util::log::ensuref(intent.o2th_parallelism > 0, "scooby-omap: o2th_parallelism must be > 0");
  sn::util::log::ensuref(intent.pmchain_access_parallelism > 0, "scooby-omap: pmchain_access_parallelism must be > 0");
  sn::util::log::ensuref(intent.pmchain_oram_parallelism >= 0, "scooby-omap: pmchain_oram_parallelism must be >= 0");
  sn::util::log::ensuref(intent.pmchain_bucket_real > 0, "scooby-omap: pmchain_bucket_real must be > 0");
  sn::util::log::ensuref(intent.pmchain_bucket_dummy > 0, "scooby-omap: pmchain_bucket_dummy must be > 0");
  sn::util::log::ensuref(intent.pmchain_evict_batch > 0, "scooby-omap: pmchain_evict_batch must be > 0");
  sn::util::log::ensuref(intent.pmchain_split_factor > 0, "scooby-omap: pmchain_split_factor must be > 0");
  sn::util::log::ensuref(
      intent.pmchain_split_factor == 1 ||
          sn::util::demo::is_supported_param(
              pmchain_supported_split_factors{}, static_cast<std::size_t>(intent.pmchain_split_factor)
          ),
      "scooby-omap: unsupported pmchain_split_factor"
  );
  sn::util::log::ensuref(
      !intent.pmchain_auto_epoch || intent.pmchain_disjoint_window == 0,
      "scooby-omap: use pmchain_auto_epoch or pmchain_disjoint_window"
  );
  if (intent.pmchain_tiered) {
    sn::util::log::ensuref(
        intent.pmchain_cache_budget_bytes > 0, "scooby-omap: tiered pmchain needs pmchain_cache_budget_bytes"
    );
    sn::util::log::ensuref(intent.pmchain_cache_pack_factor > 0, "scooby-omap: pmchain_cache_pack_factor must be > 0");
    sn::util::log::ensuref(
        !intent.pmchain_cache_path.view().empty(), "scooby-omap: tiered pmchain needs pmchain cache path"
    );
  }

  if (!intent.auto_assign_role) {
    switch (intent.role) {
    case scooby_omap_role::client:
      sn::util::log::ensuref(intent.role_index < intent.client_count, "scooby-omap: client role_index out of range");
      break;
    case scooby_omap_role::load_balancer:
      sn::util::log::ensuref(
          intent.role_index < intent.load_balancer_count, "scooby-omap: load balancer role_index out of range"
      );
      break;
    case scooby_omap_role::suboram:
      sn::util::log::ensuref(intent.role_index < intent.suboram_count, "scooby-omap: suboram role_index out of range");
      break;
    }
  }
}

inline plan_config seed_plan(const scooby_omap_intent& intent) {
  plan_config plan{};
  plan.role = intent.role;
  plan.auto_assign_role = intent.auto_assign_role;
  plan.role_index = intent.role_index;
  plan.secure_comm_key_override = intent.secure_comm_key_override;
  plan.secure_comm_key = plan.secure_comm_key_override ? intent.secure_comm_key : secure::kDefaultSecureKey;
  plan.epoch_count = intent.epoch_count;
  plan.client_count = intent.client_count;
  plan.load_balancer_count = intent.load_balancer_count;
  plan.suboram_count = intent.suboram_count;
  plan.lb_input_count = intent.lb_input_count;
  plan.payload_bytes = intent.payload_bytes;
  plan.suboram_block_count = intent.suboram_block_count;
  plan.router_lambda = intent.router_lambda;
  plan.backend = intent.backend;
  plan.client_pipeline_depth = intent.client_pipeline_depth;
  plan.lb_pipeline_depth = intent.lb_pipeline_depth;
  plan.suboram_pipeline_depth = intent.suboram_pipeline_depth;
  plan.lbrouter_parallelism = intent.lbrouter_parallelism == 0 ? 1u : intent.lbrouter_parallelism;
  plan.o2th_parallelism = intent.o2th_parallelism == 0 ? 1u : intent.o2th_parallelism;
  plan.pmchain_access_parallelism = intent.pmchain_access_parallelism == 0 ? 1u : intent.pmchain_access_parallelism;
  plan.pmchain_oram_parallelism = intent.pmchain_oram_parallelism;
  plan.pmchain_bucket_real = intent.pmchain_bucket_real == 0 ? 4u : intent.pmchain_bucket_real;
  plan.pmchain_bucket_dummy = intent.pmchain_bucket_dummy == 0 ? 4u : intent.pmchain_bucket_dummy;
  plan.pmchain_routing_depth = intent.pmchain_routing_depth;
  plan.pmchain_evict_batch = intent.pmchain_evict_batch == 0 ? 1u : intent.pmchain_evict_batch;
  plan.pmchain_auto_epoch = intent.pmchain_auto_epoch;
  plan.pmchain_drop_epoch = intent.pmchain_drop_epoch;
  plan.pmchain_disjoint_window = intent.pmchain_disjoint_window;
  plan.pmchain_split_factor = intent.pmchain_split_factor == 0 ? 1u : intent.pmchain_split_factor;
  plan.pmchain_tiered = intent.pmchain_tiered;
  plan.pmchain_hot_budget_bytes = intent.pmchain_hot_budget_bytes;
  plan.pmchain_cache_budget_bytes = intent.pmchain_cache_budget_bytes;
  plan.pmchain_backend_cache_budget_bytes = intent.pmchain_backend_cache_budget_bytes;
  plan.pmchain_cache_pack_factor = intent.pmchain_cache_pack_factor == 0 ? 1u : intent.pmchain_cache_pack_factor;
  plan.pmchain_cache_path = intent.pmchain_cache_path.view();
  if (plan.pmchain_cache_path.empty()) {
    plan.pmchain_cache_path = "/tmp/scooby_omap_cache.dat";
  }
  plan.telemetry_interval_epochs = intent.telemetry_interval_epochs;
  plan.stress_runtime_seconds = intent.stress_runtime_seconds;
  plan.router.batch_size = plan.lb_input_count;
  plan.router.suboram_count = plan.suboram_count;
  plan.router.lambda = intent.router_lambda == 0 ? 40.0 : static_cast<double>(intent.router_lambda);
  return plan;
}

inline layout_info derive_layout(plan_config& plan) {
  layout_info layout{};
  layout.requests_per_client_batch = plan.lb_input_count;
  layout.requests_per_lb_epoch = layout.requests_per_client_batch * plan.client_count;
  layout.requests_per_epoch_total = layout.requests_per_lb_epoch * plan.load_balancer_count;
  layout.bins_per_epoch_total = plan.client_count * plan.load_balancer_count * plan.suboram_count;

  dispatch_payload(plan.payload_bytes, [&](auto tag) {
    using router_types = sn::omap::lbrouter::router_types<key_type, tag.value>;
    typename router_types::config router_cfg{};
    router_cfg.batch_size = plan.router.batch_size;
    router_cfg.suboram_count = plan.router.suboram_count;
    router_cfg.security_parameter_lambda = plan.router.lambda;
    router_cfg.invalid_key = invalid_key;
    const auto derived = sn::omap::lbrouter::compute_derived_config<key_type, tag.value>(router_cfg);
    plan.router.bin_capacity = derived.bin_capacity;
    plan.router.routing_slots = derived.routing_capacity;
    plan.router.output_slots = derived.output_slots;
    layout.bin_capacity = derived.bin_capacity;
    layout.request_slot_bytes = slot_size<request_slot<key_type, tag.value>>();
    layout.routed_slot_bytes = slot_size<routed_slot<key_type, tag.value>>();
  });

  sn::util::log::ensuref(plan.router.bin_capacity > 0, "scooby-omap: router bin capacity must be > 0");
  const auto request_slot_bytes_u64 = static_cast<std::uint64_t>(layout.request_slot_bytes);
  const auto routed_slot_bytes_u64 = static_cast<std::uint64_t>(layout.routed_slot_bytes);
  sn::util::log::ensuref(
      layout.requests_per_client_batch == 0 ||
          request_slot_bytes_u64 <= std::numeric_limits<std::uint64_t>::max() / layout.requests_per_client_batch,
      "scooby-omap: batch payload size overflow"
  );
  sn::util::log::ensuref(
      layout.bin_capacity == 0 ||
          routed_slot_bytes_u64 <= std::numeric_limits<std::uint64_t>::max() / layout.bin_capacity,
      "scooby-omap: bin payload size overflow"
  );
  layout.batch_payload_bytes = layout.requests_per_client_batch * request_slot_bytes_u64;
  layout.bin_payload_bytes = layout.bin_capacity * routed_slot_bytes_u64;
#if defined(SCOOBY_SECURE_COMM)
  constexpr std::size_t secure_overhead = sn::sgxbridge::secure::aes_gcm_traits::overhead;
#else
  constexpr std::size_t secure_overhead = 0;
#endif
  layout.batch_message_bytes =
      global_header_bytes() + batch_request_header_bytes() + layout.batch_payload_bytes + secure_overhead;
  layout.bin_message_bytes =
      global_header_bytes() + bin_dispatch_header_bytes() + layout.bin_payload_bytes + secure_overhead;
  layout.batch_response_bytes =
      global_header_bytes() + batch_response_header_bytes() + layout.batch_payload_bytes + secure_overhead;
  layout.bin_response_bytes =
      global_header_bytes() + bin_response_header_bytes() + layout.bin_payload_bytes + secure_overhead;
  return layout;
}

inline void configure_backend(plan_config& plan, const scooby_omap_intent& intent, const layout_info& layout) {
  plan.o2th_batch_size = round_up_to_multiple(static_cast<std::size_t>(layout.bin_capacity), k_o2th_bucket_size);
  plan.pmchain_batch_size = round_up_to_multiple(static_cast<std::size_t>(layout.bin_capacity), k_pmchain_bucket_size);
  plan.pmchain_logical_block_bytes = plan.payload_bytes;
  plan.pmchain_physical_block_bytes = 0;
  plan.pmchain_physical_block_count = 0;
  plan.pmchain_physical_batch_size = 0;
  plan.pmchain_physical_disjoint_window = 0;

  if (plan.backend == scooby_omap_backend::pmchain) {
    const std::size_t split_factor = plan.pmchain_split_factor == 0 ? 1u : plan.pmchain_split_factor;
    sn::util::log::ensuref(
        split_factor == 1 || sn::util::demo::is_supported_param(pmchain_supported_split_factors{}, split_factor),
        "scooby-omap: unsupported pmchain split factor"
    );
    sn::util::log::ensuref(
        plan.pmchain_routing_depth < sizeof(std::size_t) * 8, "scooby-omap: pmchain routing depth too large"
    );
    const std::size_t required_eviction = std::size_t{1} << plan.pmchain_routing_depth;
    const std::uint32_t requested_eviction = intent.pmchain_eviction_parallelism == 0
                                                 ? static_cast<std::uint32_t>(required_eviction)
                                                 : intent.pmchain_eviction_parallelism;
    sn::util::log::ensuref(
        requested_eviction == required_eviction, "scooby pmchain eviction"
    );
    plan.pmchain_eviction_parallelism = requested_eviction == 0 ? 1u : requested_eviction;

    plan.pmchain_oram_parallelism = intent.pmchain_oram_parallelism;

    sn::util::log::ensuref(
        plan.pmchain_logical_block_bytes % split_factor == 0,
        "scooby-omap: logical block bytes must be divisible by split factor"
    );
    const std::size_t physical_block_bytes = plan.pmchain_logical_block_bytes / split_factor;
    const auto supported_sizes = format_supported_payload_sizes();
    const bool block_supported =
        sn::util::demo::dispatch_block_size<pmchain_supported_block_sizes>(physical_block_bytes, [](auto) {
          return true;
        });
    sn::util::log::ensuref(
        block_supported, "scooby-omap: unsupported pmchain block size (supported %s)", supported_sizes
    );
    plan.pmchain_physical_block_bytes = physical_block_bytes;

    sn::util::log::ensuref(
        plan.suboram_block_count <= std::numeric_limits<std::size_t>::max() / split_factor,
        "scooby-omap: pmchain physical block count overflow"
    );
    plan.pmchain_physical_block_count = plan.suboram_block_count * split_factor;

    sn::util::log::ensuref(
        plan.pmchain_batch_size <= std::numeric_limits<std::size_t>::max() / split_factor,
        "scooby-omap: pmchain physical batch overflow"
    );
    plan.pmchain_physical_batch_size = plan.pmchain_batch_size * split_factor;

    if (plan.pmchain_auto_epoch) {
      plan.pmchain_disjoint_window = static_cast<std::uint64_t>(plan.pmchain_batch_size);
    }
    std::uint64_t logical_disjoint = plan.pmchain_disjoint_window;
    if (logical_disjoint == 0) {
      logical_disjoint = static_cast<std::uint64_t>(plan.pmchain_batch_size);
    }
    sn::util::log::ensuref(
        logical_disjoint <= std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(split_factor),
        "scooby-omap: pmchain disjoint window overflow"
    );
    const std::uint64_t physical_disjoint_request = logical_disjoint * static_cast<std::uint64_t>(split_factor);

    const auto compute_setup = [&](auto pm_tag) {
      using pmchain_traits = sn::oram::zingoram::traits<pm_tag.value, sn::oram::zingoram::epoch_mode::disjoint_epoch>;
      sn::omap::pmchain::util::zingoram_config_input zing_cfg{
          .block_count = plan.pmchain_physical_block_count,
          .batch_size = plan.pmchain_physical_batch_size,
          .bucket_real_size = plan.pmchain_bucket_real,
          .bucket_dummy_size = plan.pmchain_bucket_dummy,
          .routing_depth = plan.pmchain_routing_depth,
          .evict_batch = plan.pmchain_evict_batch,
          .access_concurrency = std::max<std::uint32_t>(plan.pmchain_access_parallelism, 1u),
          .disjoint_epoch_window = physical_disjoint_request,
      };
      auto zing_setup = sn::omap::pmchain::util::compute_zingoram_setup<pmchain_traits>(zing_cfg, "scooby-omap");
      plan.pmchain_physical_disjoint_window = zing_setup.disjoint_window;
      sn::util::log::ensuref(
          plan.pmchain_physical_disjoint_window % split_factor == 0,
          "scooby-omap: pmchain physical disjoint window not divisible by split factor"
      );
      plan.pmchain_disjoint_window = plan.pmchain_physical_disjoint_window / split_factor;
      sn::util::log::ensuref(
          plan.pmchain_disjoint_window >= plan.pmchain_batch_size,
          "scooby-omap: pmchain logical disjoint window smaller than batch"
      );
      plan.pmchain_eviction_rate = static_cast<std::uint32_t>(zing_setup.opts.eviction_rate);
    };

    const bool configured =
        sn::util::demo::dispatch_block_size<pmchain_supported_block_sizes>(physical_block_bytes, compute_setup);
    sn::util::log::ensuref(configured, "scooby-omap: pmchain unsupported block size (supported %s)", supported_sizes);
#if !defined(SONIC_ORAM_TIERED_STORAGE)
    sn::util::log::ensuref(!plan.pmchain_tiered, "scooby-omap: tiered storage disabled at build time");
#else
    if (!plan.pmchain_tiered) {
      plan.pmchain_hot_budget_bytes = 0;
      plan.pmchain_cache_budget_bytes = 0;
      plan.pmchain_backend_cache_budget_bytes = 0;
      plan.pmchain_cache_pack_factor = 1;
      plan.pmchain_cache_path.clear();
    }
#endif
  } else if (plan.pmchain_eviction_parallelism == 0) {
    plan.pmchain_eviction_parallelism = 1;
  }
}

inline void derive_secure_keys(plan_config& plan) {
#if defined(SCOOBY_SECURE_COMM)
  std::array<std::uint8_t, sn::crypto::hkdf_sha256::hash_len> prk{};
  sn::crypto::hkdf_sha256::extract(
      sn::util::span<const std::uint8_t>(),
      sn::util::span<const std::uint8_t>(plan.secure_comm_key.data(), plan.secure_comm_key.size()),
      sn::util::span<std::uint8_t>(prk)
  );
  auto derive = [&](const char* label, sn::sgxbridge::secure::aes_gcm_traits::key_type& key) {
    const std::string info = std::string("scooby-omap/") + label;
    sn::crypto::hkdf_sha256::expand(
        sn::util::span<const std::uint8_t>(prk),
        sn::util::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(info.data()), info.size()),
        sn::util::span<std::uint8_t>(key.bytes.data(), key.bytes.size())
    );
  };
  derive("client->lb", plan.secure_keys.client_to_lb);
  derive("lb->client", plan.secure_keys.lb_to_client);
  derive("lb->suboram", plan.secure_keys.lb_to_suboram);
  derive("suboram->lb", plan.secure_keys.suboram_to_lb);
#endif
}

inline plan_config make_plan(const scooby_omap_intent& intent) {
  validate_intent(intent);
  plan_config plan = seed_plan(intent);
  layout_info layout = derive_layout(plan);
  configure_backend(plan, intent, layout);
  plan.layout = layout;
  derive_secure_keys(plan);
  return plan;
}

inline void log_plan_summary(const plan_config& plan, types::execution_context& ctx) {
  ctx.logger.inf(
      pfm::format(
          "scooby plan role=%s index=%u epochs=%u clients=%u lbs=%u suborams=%u backend=%s payload=%u "
          "requests=%llu",
          describe_role(plan.role), plan.role_index, plan.epoch_count, plan.client_count, plan.load_balancer_count,
          plan.suboram_count, describe_backend(plan.backend), plan.payload_bytes,
          static_cast<unsigned long long>(plan.total_requests_per_epoch())
      )
  );
  ctx.logger.inf(
      pfm::format(
          "scooby layout bin=%llu request=%zu routed=%zu batch=%llu bin_msg=%llu",
          static_cast<unsigned long long>(plan.layout.bin_capacity), plan.layout.request_slot_bytes,
          plan.layout.routed_slot_bytes, static_cast<unsigned long long>(plan.layout.batch_message_bytes),
          static_cast<unsigned long long>(plan.layout.bin_message_bytes)
      )
  );
#if defined(SCOOBY_SECURE_COMM)
  constexpr bool build_secure = kSecureCommEnabled;
  constexpr std::size_t secure_overhead = sn::sgxbridge::secure::aes_gcm_traits::overhead;
#else
  constexpr bool build_secure = false;
  constexpr std::size_t secure_overhead = 0;
#endif
  ctx.logger.inf(
      pfm::format(
          "scooby secure build=%s overhead=%zu key=%s", build_secure ? "enabled" : "disabled",
          secure_overhead, plan.secure_comm_key_override ? "override" : "default"
      )
  );
  if (plan.backend == scooby_omap_backend::o2th) {
    ctx.logger.inf(
        pfm::format(
            "scooby o2th blocks=%u batch=%zu threads=%u", plan.suboram_block_count, plan.o2th_batch_size,
            plan.o2th_parallelism
        )
    );
  } else if (plan.backend == scooby_omap_backend::pmchain) {
    ctx.logger.inf(
        pfm::format(
            "scooby pmchain batch=%zu/%zu block=%zu/%zu split=%u count=%u/%zu "
            "threads=%u/%u/%u tiered=%u",
            plan.pmchain_batch_size, plan.pmchain_physical_batch_size, plan.pmchain_logical_block_bytes,
            plan.pmchain_physical_block_bytes, plan.pmchain_split_factor, plan.suboram_block_count,
            plan.pmchain_physical_block_count, plan.pmchain_access_parallelism, plan.pmchain_eviction_parallelism,
            plan.pmchain_evict_batch, plan.pmchain_tiered ? 1u : 0u
        )
    );
    if (plan.pmchain_tiered) {
      ctx.logger.inf(
          pfm::format(
              "scooby pmchain tiered hot=%llu cache=%llu backend=%llu pack=%u",
              static_cast<unsigned long long>(plan.pmchain_hot_budget_bytes),
              static_cast<unsigned long long>(plan.pmchain_cache_budget_bytes),
              static_cast<unsigned long long>(plan.pmchain_backend_cache_budget_bytes), plan.pmchain_cache_pack_factor
          )
      );
    }
  }
}

}
