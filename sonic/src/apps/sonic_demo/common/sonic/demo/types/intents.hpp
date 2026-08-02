#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "sonic/demo/types/string_buffer.hpp"

namespace sn::demo::types {

constexpr std::size_t k_hello_name_capacity = 96;
constexpr std::size_t k_output_capacity = 512;
constexpr std::size_t k_cache_path_capacity = 192;

using hello_name = string_buffer<k_hello_name_capacity>;
using command_text = string_buffer<k_output_capacity>;

enum class command_tag : std::uint32_t {
  none = 0,
  hello = 1,
  parallel_scan = 2,
  pathoram = 3,
  zingoram = 4,
  o2th = 5,
  pmchain = 6,
};

enum class result_status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  unsupported = 2,
  internal_error = 3,
};

struct hello_intent {
  hello_name name{};
  std::uint32_t repeat{1};
  std::uint8_t enthusiastic{0};
};

struct parallel_scan_intent {
  std::uint32_t elements{1024};
  std::uint32_t requested_workers{0};
};

enum class oram_action : std::uint32_t {
  validate = 0,
  experiment = 1,
  benchmark = 2,
};

struct pathoram_intent {
  oram_action action{oram_action::validate};
  std::uint32_t block_bytes{64};
  std::uint64_t block_count{0};
  std::uint64_t accesses{0};
};

struct zingoram_intent {
  oram_action action{oram_action::validate};
  std::uint32_t block_bytes{64};
  std::uint32_t bucket_real{0};
  std::uint32_t bucket_dummy{0};
  std::uint32_t eviction_rate{0};
  std::uint32_t routing_depth{0};
  std::uint32_t evict_batch{1};
  std::uint32_t access_concurrency{1};
  std::uint32_t accesses{0};
  std::uint32_t eviction_threads{0};
  std::uint32_t mode{0};
  std::uint32_t online_only{0};
  std::uint32_t disjoint_window_present{0};
  std::uint32_t reserved{0};
  std::uint64_t block_count{0};
  std::uint64_t disjoint_window{0};
  std::uint64_t hot_budget_bytes{0};
  std::uint64_t cache_budget_bytes{0};
  std::uint64_t backend_cache_budget_bytes{0};
  std::uint32_t cache_pack_factor{1};
  std::uint8_t tiered{0};
  std::uint8_t reserved_flags[3]{0};
  string_buffer<k_cache_path_capacity> cache_path{};
};

struct o2th_intent {
  oram_action action{oram_action::validate};
  std::uint32_t block_size{64};
  std::uint32_t bucket_size{64};
  std::uint32_t worker_parallelism{0};
  std::uint32_t access_concurrency{0};
  std::uint32_t batch_count{1};
  std::uint32_t reserved{0};
  double write_ratio{0.5};
  std::uint64_t dataset_size{0};
  std::uint64_t request_count{0};
  std::uint8_t reserved_flags[8]{0};
};

struct pmchain_intent {
  oram_action action{oram_action::validate};
  std::uint32_t block_bytes{64};
  std::uint32_t split_factor{1};
  std::uint32_t posmap_bucket_size{0};
  std::uint32_t bucket_real_size{0};
  std::uint32_t bucket_dummy_size{0};
  std::uint32_t routing_depth{0};
  std::uint32_t evict_batch{1};
  std::uint32_t access_workers{0};
  std::uint32_t oram_parallelism{0};
  std::uint32_t eviction_threads{0};
  std::uint32_t batch_count{1};
  std::uint8_t drop_epoch{0};
  std::uint8_t reserved_bytes[3]{0};
  double write_ratio{0.5};
  double dummy_ratio{0.0};
  std::uint64_t block_count{0};
  std::uint64_t batch_size{0};
  std::uint64_t disjoint_epoch_window{0};
  std::uint64_t hot_budget_bytes{0};
  std::uint64_t cache_budget_bytes{0};
  std::uint64_t backend_cache_budget_bytes{0};
  std::uint32_t cache_pack_factor{1};
  std::uint8_t tiered{0};
  std::uint8_t reserved_bytes2[7]{0};
  string_buffer<k_cache_path_capacity> cache_path{};
};

struct command_intent {
  command_tag tag{command_tag::none};
  hello_intent hello{};
  parallel_scan_intent parallel_scan{};
  pathoram_intent pathoram{};
  zingoram_intent zingoram{};
  o2th_intent o2th{};
  pmchain_intent pmchain{};
};

struct command_result {
  result_status status{result_status::internal_error};
  command_text output{};
};

inline const char* describe(result_status status) {
  switch (status) {
  case result_status::ok:
    return "ok";
  case result_status::invalid_arguments:
    return "invalid_arguments";
  case result_status::unsupported:
    return "unsupported";
  case result_status::internal_error:
    return "internal_error";
  }
  return "unknown";
}

inline command_result make_result(result_status status, std::string_view message = {}) {
  command_result result{};
  result.status = status;
  (void) result.output.assign(message);
  return result;
}

static_assert(std::is_trivially_copyable_v<command_intent>, "command_intent must remain POD");
static_assert(std::is_trivially_copyable_v<command_result>, "command_result must remain POD");

}
