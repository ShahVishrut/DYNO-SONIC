#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "sonic/sgxbridge/common/host_buffer.hpp"

namespace sn::sgxbridge::hostbuf {

class host_buffer_manager {
public:
  struct config {
    std::size_t max_total_bytes = 0;
    std::size_t max_single_allocation = 0;
  };

  host_buffer_manager();
  explicit host_buffer_manager(config cfg);

  host_buffer_manager(const host_buffer_manager&) = delete;
  host_buffer_manager& operator=(const host_buffer_manager&) = delete;

  result allocate(std::size_t bytes, descriptor& out);
  result adopt(std::vector<std::uint8_t>&& payload, descriptor& out);
  result wrap_external(std::uint8_t* data, std::size_t size, std::function<void()> on_release, descriptor& out);
  result view(buffer_id id, descriptor& out) const;
  result pin(buffer_id id, descriptor& out);
  status release(buffer_id id);
  status retain(buffer_id id);
  void configure(config cfg);
  void shutdown();

private:
  struct record {
    descriptor desc;
    std::vector<std::uint8_t> storage;
    std::uint32_t refs{1};
    bool external{false};
    std::function<void()> release_cb;
  };

  result insert_locked(std::vector<std::uint8_t>&& storage, descriptor& out);
  result insert_external_locked(
      std::uint8_t* data, std::size_t size, std::function<void()> on_release, descriptor& out
  );

  config config_{};
  mutable std::mutex mutex_;
  std::unordered_map<buffer_id, record> records_;
  buffer_id next_id_{1};
  std::size_t total_bytes_{0};
};

}
