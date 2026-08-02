#include "sonic/sgxbridge/dist/api.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "sonic/dist/runtime.hpp"
#include "sonic/sgxbridge/host/host_buffer_manager.hpp"
#include "sonic/util/log.hpp"

namespace sn::sgxbridge::dist {
namespace {
struct runtime_state {
  std::mutex mutex;
  bool active = false;
  config cfg{};
  std::unique_ptr<hostbuf::host_buffer_manager> internal_buffers;
  hostbuf::host_buffer_manager* buffers = nullptr;
} g_state;

sn::dist::runtime_config to_runtime_config(const runtime_options& opts) {
  sn::dist::runtime_config out{};
  out.progress_sleep_ms = opts.progress_sleep_ms;
  out.max_pending_commands = opts.max_pending_commands;
  out.max_pending_messages = opts.max_pending_messages;
  out.recv_slots = opts.recv_slots;
  out.recv_slot_bytes = opts.recv_slot_bytes;
  return out;
}

hostbuf::host_buffer_manager& require_buffers() {
  if (g_state.buffers == nullptr) {
    throw std::runtime_error("sgxbridge dist runtime has no host buffer manager");
  }
  return *g_state.buffers;
}

host_buffer_handle wrap_external_slot(const sn::dist::message_view& msg) {
  if (msg.payload.empty()) {
    sn::dist::release(msg);
    return host_buffer_handle{};
  }
  auto& buffers = require_buffers();
  hostbuf::descriptor desc{};
  const auto status = buffers.wrap_external(
      const_cast<std::uint8_t*>(msg.payload.data()), msg.payload.size(),
      [slot_id = msg.slot_id]() {

        sn::dist::message_view view{};
        view.slot_id = slot_id;
        sn::dist::release(view);
      },
      desc
  );
  if (!status.succeeded()) {
    sn::dist::message_view tmp{};
    tmp.slot_id = msg.slot_id;
    sn::dist::release(tmp);
    throw std::runtime_error("failed to wrap inbound message in host buffer");
  }
  return host_buffer_handle{desc.id};
}

message_descriptor adopt_message(const sn::dist::message_view& view) {
  message_descriptor desc{};
  desc.src_rank = view.src_rank;
  desc.buffer = wrap_external_slot(view);
  desc.payload_size = view.payload.size();
  return desc;
}

void ensure_active() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (!g_state.active) {
    throw std::runtime_error("sgxbridge dist runtime is not initialized");
  }
}

}

void init(const config& cfg) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.active) {
    throw std::runtime_error("sgxbridge dist runtime already initialized");
  }
  g_state.cfg = cfg;
  if (cfg.external_host_buffers != nullptr) {
    g_state.buffers = cfg.external_host_buffers;
  } else {
    hostbuf::host_buffer_manager::config buf_cfg{};
    buf_cfg.max_total_bytes = cfg.max_host_buffer_bytes;
    buf_cfg.max_single_allocation = cfg.max_host_buffer_bytes;
    g_state.internal_buffers = std::make_unique<hostbuf::host_buffer_manager>(buf_cfg);
    g_state.buffers = g_state.internal_buffers.get();
  }
  sn::dist::runtime_config native_cfg = to_runtime_config(cfg.runtime);
  sn::dist::init(native_cfg);
  g_state.active = true;
}

void finalize() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (!g_state.active) {
    return;
  }
  if (g_state.cfg.external_host_buffers == nullptr && g_state.internal_buffers != nullptr) {
    g_state.internal_buffers->shutdown();
    g_state.internal_buffers.reset();
  }
  sn::dist::finalize();
  g_state.buffers = nullptr;
  g_state.cfg = config{};
  g_state.active = false;
}

int rank() {
  ensure_active();
  return sn::dist::rank();
}

int world_size() {
  ensure_active();
  return sn::dist::world_size();
}

sn::util::future<void> async_send_bytes(int dest_rank, byte_span payload) {
  ensure_active();
  return sn::dist::async_send(dest_rank, payload);
}

void send_bytes(int dest_rank, byte_span payload) {
  auto fut = async_send_bytes(dest_rank, payload);
  fut.get();
}

void send_from_host_buffer(int dest_rank, host_buffer_handle handle, std::size_t offset, std::size_t size) {
  ensure_active();
  if (!handle) {
    throw std::invalid_argument("send_from_host_buffer requires valid handle");
  }
  auto& buffers = require_buffers();
  hostbuf::descriptor desc{};
  const auto pin_result = buffers.pin(handle.id, desc);
  if (!pin_result.succeeded()) {
    throw std::runtime_error("send_from_host_buffer: unknown buffer id");
  }
  if (offset > desc.size || size > desc.size || (offset + size) > desc.size) {
    buffers.release(handle.id);
    throw std::runtime_error("send_from_host_buffer: invalid offset/size");
  }
  try {
    byte_span span(desc.data + offset, size);
    auto fut = sn::dist::async_send(dest_rank, span);
    fut.get();
  } catch (...) {
    buffers.release(handle.id);
    throw;
  }
  buffers.release(handle.id);
}

bool try_recv(message_descriptor& out) {
  ensure_active();
  sn::dist::message_view view{};
  if (!sn::dist::try_recv(view)) {
    return false;
  }
  out = adopt_message(view);
  return true;
}

message_descriptor recv() {
  ensure_active();
  sn::dist::message_view view = sn::dist::recv();
  return adopt_message(view);
}

bool recv_for(message_descriptor& out, recv_timeout timeout) {
  ensure_active();
  sn::dist::message_view view{};
  if (!sn::dist::recv_for(view, timeout)) {
    return false;
  }
  out = adopt_message(view);
  return true;
}

void release(const message_descriptor& msg) {
  if (!msg.buffer) {
    return;
  }
  auto& buffers = require_buffers();
  const auto status = buffers.release(msg.buffer.id);
  sn::util::log::ensuref(
      status == hostbuf::status::ok, "sgxbridge::dist::release failed id=%llu status=%u",
      static_cast<unsigned long long>(msg.buffer.id), static_cast<unsigned>(status)
  );
}

std::size_t pending_messages() {
  ensure_active();
  return sn::dist::pending_messages();
}

void barrier() {
  ensure_active();
  sn::dist::barrier();
}

double allreduce_sum(double local_value) {
  ensure_active();
  return sn::dist::allreduce_sum(local_value);
}

byte_span map_readonly(host_buffer_handle handle) {
  if (!handle) {
    return byte_span(nullptr, std::size_t{0});
  }
  hostbuf::descriptor desc{};
  auto& buffers = require_buffers();
  const auto view_result = buffers.view(handle.id, desc);
  if (!view_result.succeeded()) {
    throw std::runtime_error("map_readonly: unknown host buffer handle");
  }
  return byte_span(desc.data, desc.size);
}

byte_span_mut map_writable(host_buffer_handle handle) {
  if (!handle) {
    return byte_span_mut(nullptr, std::size_t{0});
  }
  hostbuf::descriptor desc{};
  auto& buffers = require_buffers();
  const auto view_result = buffers.view(handle.id, desc);
  if (!view_result.succeeded()) {
    throw std::runtime_error("map_writable: unknown host buffer handle");
  }
  return byte_span_mut(desc.data, desc.size);
}

byte_span map_payload(const message_descriptor& msg) {
  auto view = map_readonly(msg.buffer);
  if (msg.payload_size > view.size()) {
    throw std::runtime_error("map_payload: payload exceeds host buffer");
  }
  return byte_span(view.data(), msg.payload_size);
}

byte_span_mut map_payload_mut(const message_descriptor& msg) {
  auto view = map_writable(msg.buffer);
  if (msg.payload_size > view.size()) {
    throw std::runtime_error("map_payload_mut: payload exceeds host buffer");
  }
  return byte_span_mut(view.data(), msg.payload_size);
}

#if !defined(SN_SGX_ENCLAVE)
host_buffer_handle allocate_host_buffer(std::size_t bytes) {
  ensure_active();
  hostbuf::descriptor desc{};
  auto& buffers = require_buffers();
  const auto create_result = buffers.allocate(bytes, desc);
  if (!create_result.succeeded()) {
    throw std::runtime_error("allocate_host_buffer failed");
  }
  return host_buffer_handle{desc.id};
}

void free_host_buffer(host_buffer_handle handle) {
  if (!handle) {
    return;
  }
  auto& buffers = require_buffers();
  const auto status = buffers.release(handle.id);
  sn::util::log::ensuref(
      status == hostbuf::status::ok, "free_host_buffer failed id=%llu status=%u",
      static_cast<unsigned long long>(handle.id), static_cast<unsigned>(status)
  );
}
#endif

}
