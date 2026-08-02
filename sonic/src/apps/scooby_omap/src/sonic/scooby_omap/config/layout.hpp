#pragma once

#include <cstddef>
#include <cstdint>

namespace sn::scooby::omap {

struct layout_info {
  std::uint64_t requests_per_client_batch{0};
  std::uint64_t requests_per_lb_epoch{0};
  std::uint64_t requests_per_epoch_total{0};
  std::uint64_t bins_per_epoch_total{0};
  std::uint64_t bin_capacity{0};
  std::size_t request_slot_bytes{0};
  std::size_t routed_slot_bytes{0};
  std::uint64_t batch_payload_bytes{0};
  std::uint64_t bin_payload_bytes{0};
  std::uint64_t batch_message_bytes{0};
  std::uint64_t bin_message_bytes{0};
  std::uint64_t batch_response_bytes{0};
  std::uint64_t bin_response_bytes{0};
};

struct router_plan {
  std::size_t batch_size{0};
  std::size_t suboram_count{0};
  double lambda{40.0};
  std::size_t bin_capacity{0};
  std::size_t routing_slots{0};
  std::size_t output_slots{0};
};

}
