#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace sn::omap::pmchain {

struct config {
  std::size_t block_count = 0;
  std::size_t oram_block_bytes = 0;
  std::size_t batch_size = 0;
  std::size_t oram_parallelism = 0;
  bool drop_epoch = false;
};

template <typename O2THClient, typename OramClient> struct types {
  using o2th_client_type = O2THClient;
  using oram_client_type = OramClient;
  static_assert(
      requires { typename oram_client_type::logical_access_request; },
      "pmchain requires oram client to expose logical_access_request"
  );
  static_assert(
      requires { typename oram_client_type::access_scratch; }, "pmchain requires oram client to expose access_scratch"
  );
  using key_type = typename o2th_client_type::key_type;
  using req_type = typename o2th_client_type::op_request;
  template <typename T> using maybe_dummy = typename o2th_client_type::template maybe_dummy<T>;
  using data_query = typename o2th_client_type::data_query;
  using bucket_index = typename o2th_client_type::bucket_index;
  using oram_state_type = std::remove_reference_t<decltype(std::declval<oram_client_type&>().state_ref())>;
  using oram_block_type = typename oram_state_type::block_t;

  static constexpr std::size_t posmap_block_bytes = o2th_client_type::block_size;

  static_assert(posmap_block_bytes >= 8, "pmchain::types: o2th block size too small for position tuple");
  static_assert(std::is_integral_v<key_type>, "pmchain::types requires integral key type");

  struct operation {
    key_type key{};
    bool is_write = false;
    bool is_dummy = false;
    std::uint32_t extra_data = 0;
  };

  struct addrctr_tuple {
    std::uint32_t address = 0;
    std::uint32_t counter = 0;
  };

  struct worker_state {
    typename oram_state_type::access_scratch scratch{};
  };
};

} // namespace sn::omap::pmchain
