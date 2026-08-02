#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/util/span.hpp"

#if defined(SN_SGX_ENCLAVE)
#include "sonic/sgxbridge/enclave/host_buffer_portal.hpp"
#endif

namespace sn::scooby::omap {

struct host_buffer_pool_config {
  std::size_t slot_bytes = 0;
  std::size_t pool_size = 0;
};

class host_buffer_pool {
public:
#if defined(SN_SGX_ENCLAVE)
  using buffer_type = sn::sgxbridge::enclave::hostbuf::buffer;
#else
  using buffer_type = sn::sgxbridge::dist::host_buffer;
#endif

  host_buffer_pool() = default;

  explicit host_buffer_pool(const host_buffer_pool_config& cfg) { configure(cfg); }

  void configure(const host_buffer_pool_config& cfg) {
    buffers_.clear();
    buffers_.reserve(cfg.pool_size);
    const std::size_t bytes = cfg.slot_bytes == 0 ? 1 : cfg.slot_bytes;
    slot_bytes_ = bytes;
    for (std::size_t i = 0; i < cfg.pool_size; ++i) {
      buffers_.emplace_back(make_buffer(bytes));
    }
    next_index_ = 0;
  }

  buffer_type& acquire() {
    if (buffers_.empty()) {
      throw std::runtime_error("host_buffer_pool empty");
    }
    buffer_type& buf = buffers_[next_index_];
    next_index_ = (next_index_ + 1) % buffers_.size();
    return buf;
  }

  std::size_t size() const noexcept { return buffers_.size(); }
  std::size_t slot_bytes() const noexcept { return slot_bytes_; }

private:
  static buffer_type make_buffer(std::size_t bytes) {
#if defined(SN_SGX_ENCLAVE)
    return sn::sgxbridge::enclave::hostbuf::buffer::allocate(bytes);
#else
    return sn::sgxbridge::dist::host_buffer(bytes);
#endif
  }

  std::vector<buffer_type> buffers_{};
  std::size_t next_index_{0};
  std::size_t slot_bytes_{0};
};

inline sn::util::span<std::uint8_t> map_writable(host_buffer_pool::buffer_type& buffer) {
#if defined(SN_SGX_ENCLAVE)
  return sn::util::span<std::uint8_t>(buffer.data(), buffer.size());
#else
  return buffer.writable();
#endif
}

inline sn::util::span<const std::uint8_t> map_readonly(const host_buffer_pool::buffer_type& buffer) {
#if defined(SN_SGX_ENCLAVE)
  return sn::util::span<const std::uint8_t>(buffer.data(), buffer.size());
#else
  return buffer.readonly();
#endif
}

inline sn::sgxbridge::dist::host_buffer_handle buffer_handle(host_buffer_pool::buffer_type& buffer) {
#if defined(SN_SGX_ENCLAVE)
  return sn::sgxbridge::dist::host_buffer_handle{buffer.id()};
#else
  return buffer.handle();
#endif
}

}
