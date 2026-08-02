#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/sgxbridge/common/host_buffer.hpp"

namespace sn::sgxbridge::common {
struct enclave_execution_context;
}

namespace sn::sgxbridge::enclave::hostbuf {

class buffer {
public:
  buffer() = default;
  buffer(const buffer&) = delete;
  buffer& operator=(const buffer&) = delete;
  buffer(buffer&& other) noexcept;
  buffer& operator=(buffer&& other) noexcept;
  ~buffer();

  static buffer allocate(std::size_t bytes);

  buffer(sn::sgxbridge::hostbuf::buffer_id id, std::uint8_t* ptr, std::size_t size, void* cookie);

  [[nodiscard]] sn::sgxbridge::hostbuf::buffer_id id() const noexcept { return id_; }
  [[nodiscard]] std::uint8_t* data() noexcept { return data_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  explicit operator bool() const noexcept { return data_ != nullptr; }

  void reset() noexcept;

private:
  void* host_cookie_{nullptr};
  sn::sgxbridge::hostbuf::buffer_id id_{0};
  std::uint8_t* data_{nullptr};
  std::size_t size_{0};
};

struct mapped_view {
  std::uint8_t* data{nullptr};
  std::size_t size{0};
};

mapped_view map(sn::sgxbridge::hostbuf::buffer_id id);
void release(sn::sgxbridge::hostbuf::buffer_id id);

}
