#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "sonic/scooby_node/types/string_buffer.hpp"

namespace sn::scooby::types {

constexpr std::size_t k_output_capacity = 256;
constexpr std::size_t k_cache_path_capacity = 192;

using command_text = string_buffer<k_output_capacity>;

enum class command_tag : std::uint32_t {
  none = 0,
  scooby_omap = 1,
};

enum class result_status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  internal_error = 2,
};

enum class scooby_omap_role : std::uint32_t {
  client = 0,
  load_balancer = 1,
  suboram = 2,
};

enum class scooby_omap_backend : std::uint32_t {
  o2th = 0,
  pmchain = 1,
};

struct scooby_omap_intent {
  scooby_omap_role role{scooby_omap_role::client};
  bool auto_assign_role{false};
  std::uint32_t role_index{0};
  std::array<std::uint8_t, 32> secure_comm_key{};
  bool secure_comm_key_override{false};
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
  std::uint32_t pmchain_eviction_parallelism{0};
  std::uint32_t pmchain_bucket_real{4};
  std::uint32_t pmchain_bucket_dummy{4};
  std::uint32_t pmchain_routing_depth{1};
  std::uint32_t pmchain_evict_batch{1};
  bool pmchain_auto_epoch{false};
  bool pmchain_drop_epoch{false};
  std::uint64_t pmchain_disjoint_window{0};
  std::uint32_t pmchain_split_factor{1};
  bool pmchain_tiered{false};
  std::uint64_t pmchain_hot_budget_bytes{0};
  std::uint64_t pmchain_cache_budget_bytes{0};
  std::uint64_t pmchain_backend_cache_budget_bytes{0};
  std::uint32_t pmchain_cache_pack_factor{1};
  string_buffer<k_cache_path_capacity> pmchain_cache_path{};
  std::uint32_t telemetry_interval_epochs{0};
  std::uint32_t stress_runtime_seconds{0};
};

struct command_intent {
  command_tag tag{command_tag::none};
  scooby_omap_intent scooby_omap{};
};

struct command_result {
  result_status status{result_status::internal_error};
  command_text output{};
};

inline command_result make_result(result_status status, std::string_view message = {}) {
  command_result result{};
  result.status = status;
  (void) result.output.assign(message);
  return result;
}

inline const char* describe(result_status status) {
  switch (status) {
  case result_status::ok:
    return "ok";
  case result_status::invalid_arguments:
    return "invalid_arguments";
  case result_status::internal_error:
    return "internal_error";
  }
  return "unknown";
}

static_assert(std::is_trivially_copyable_v<command_intent>, "command_intent must be POD for SGX marshalling");
static_assert(std::is_trivially_copyable_v<command_result>, "command_result must be POD for SGX marshalling");
static_assert(std::is_trivially_copyable_v<scooby_omap_intent>, "scooby_omap_intent must be POD for SGX marshalling");

}
