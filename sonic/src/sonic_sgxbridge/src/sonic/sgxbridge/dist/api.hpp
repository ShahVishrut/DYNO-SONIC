#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "sonic/sgxbridge/common/host_buffer.hpp"
#include "sonic/util/future.hpp"
#include "sonic/util/span.hpp"

namespace sn::sgxbridge::hostbuf {
class host_buffer_manager;
}

namespace sn::sgxbridge::dist {

using byte_span = sn::util::span<const std::uint8_t>;
using byte_span_mut = sn::util::span<std::uint8_t>;

struct runtime_options {
  int progress_sleep_ms = 0;
  std::size_t max_pending_commands = 0;
  std::size_t max_pending_messages = 0;
  std::size_t recv_slots = 4;
  std::size_t recv_slot_bytes = 256 * 1024 * 1024;
};

struct config {
  runtime_options runtime{};
  std::size_t max_host_buffer_bytes = 0;
  hostbuf::host_buffer_manager* external_host_buffers = nullptr;
};

struct host_buffer_handle {
  sn::sgxbridge::hostbuf::buffer_id id{0};
  explicit operator bool() const noexcept { return id != 0; }
};

struct message_descriptor {
  int src_rank = -1;
  host_buffer_handle buffer;
  std::size_t payload_size = 0;
};

byte_span map_readonly(host_buffer_handle handle);
byte_span_mut map_writable(host_buffer_handle handle);
byte_span map_payload(const message_descriptor& msg);
byte_span_mut map_payload_mut(const message_descriptor& msg);
void release(const message_descriptor& msg);

class scoped_message {
public:
  scoped_message() = default;
  explicit scoped_message(message_descriptor desc) noexcept : desc_(desc) {}

  scoped_message(const scoped_message&) = delete;
  scoped_message& operator=(const scoped_message&) = delete;

  scoped_message(scoped_message&& other) noexcept { *this = std::move(other); }
  scoped_message& operator=(scoped_message&& other) noexcept {
    if (this != &other) {
      reset();
      desc_ = other.desc_;
      other.desc_.buffer.id = 0;
      other.desc_.payload_size = 0;
      other.desc_.src_rank = -1;
    }
    return *this;
  }

  ~scoped_message() noexcept { reset(); }

  [[nodiscard]] message_descriptor& get() noexcept { return desc_; }
  [[nodiscard]] const message_descriptor& get() const noexcept { return desc_; }
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(desc_.buffer); }

  void reset() noexcept {
    if (!desc_.buffer) {
      return;
    }
#if defined(SN_SGX_ENCLAVE)
    try {
      release(desc_);
    } catch (...) {

    }
#else
    try {
      release(desc_);
    } catch (...) {
      std::abort();
    }
#endif
    desc_.buffer.id = 0;
    desc_.payload_size = 0;
    desc_.src_rank = -1;
  }

  [[nodiscard]] message_descriptor release_raw() noexcept {
    message_descriptor tmp = desc_;
    desc_.buffer.id = 0;
    desc_.payload_size = 0;
    desc_.src_rank = -1;
    return tmp;
  }

  [[nodiscard]] byte_span payload() const { return map_payload(desc_); }
  [[nodiscard]] byte_span_mut payload_mut() { return map_payload_mut(desc_); }

private:
  message_descriptor desc_{};
};

#if defined(SN_SGX_ENCLAVE)
using recv_timeout = std::uint64_t;
#else
using recv_timeout = std::chrono::milliseconds;
#endif

#if defined(SN_SGX_ENCLAVE)
void init();
#else
void init(const config& cfg = {});
#endif

void finalize();

int rank();
int world_size();

sn::util::future<void> async_send_bytes(int dest_rank, byte_span payload);
void send_bytes(int dest_rank, byte_span payload);
void send_from_host_buffer(int dest_rank, host_buffer_handle handle, std::size_t offset, std::size_t size);

bool try_recv(message_descriptor& out);
message_descriptor recv();
bool recv_for(message_descriptor& out, recv_timeout timeout);
scoped_message recv_scoped();
bool try_recv(scoped_message& out);
bool recv_for(scoped_message& out, recv_timeout timeout);
std::size_t pending_messages();
void barrier();
double allreduce_sum(double local_value);

#if !defined(SN_SGX_ENCLAVE)
host_buffer_handle allocate_host_buffer(std::size_t bytes);
void free_host_buffer(host_buffer_handle handle);

class host_buffer {
public:
  host_buffer() = default;
  explicit host_buffer(std::size_t bytes) { allocate(bytes); }

  host_buffer(const host_buffer&) = delete;
  host_buffer& operator=(const host_buffer&) = delete;

  host_buffer(host_buffer&& other) noexcept { *this = std::move(other); }
  host_buffer& operator=(host_buffer&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      size_ = other.size_;
      other.handle_.id = 0;
      other.size_ = 0;
    }
    return *this;
  }

  ~host_buffer() noexcept { reset(); }

  void allocate(std::size_t bytes) {
    reset();
    if (bytes == 0) {
      return;
    }
    handle_ = allocate_host_buffer(bytes);
    size_ = map_readonly(handle_).size();
  }

  [[nodiscard]] byte_span readonly() const { return map_readonly(handle_); }
  [[nodiscard]] byte_span_mut writable() const { return map_writable(handle_); }
  [[nodiscard]] host_buffer_handle handle() const noexcept { return handle_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  void reset() noexcept {
    if (!handle_) {
      size_ = 0;
      return;
    }
    try {
      free_host_buffer(handle_);
    } catch (...) {
      std::abort();
    }
    handle_.id = 0;
    size_ = 0;
  }

private:
  host_buffer_handle handle_{};
  std::size_t size_{0};
};
#endif

inline scoped_message recv_scoped() { return scoped_message(recv()); }

inline bool try_recv(scoped_message& out) {
  message_descriptor tmp{};
  if (!try_recv(tmp)) {
    out.reset();
    return false;
  }
  out = scoped_message(tmp);
  return true;
}

inline bool recv_for(scoped_message& out, recv_timeout timeout) {
  message_descriptor tmp{};
  if (!recv_for(tmp, timeout)) {
    out.reset();
    return false;
  }
  out = scoped_message(tmp);
  return true;
}

}
