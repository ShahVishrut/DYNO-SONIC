#include "sonic/sgxbridge/dist/api.hpp"

#if SONIC_SGXBRIDGE_DISTRIBUTED

#include <chrono>
#include <stdexcept>
#include <string>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"
#include "sonic/sgxbridge/enclave/host_buffer_portal.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/util/future.hpp"
#include "sonic/util/log.hpp"

namespace sn::sgxbridge::dist {
namespace {

common::enclave_execution_context& require_context() {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    throw std::runtime_error("sgxbridge dist");
  }
  return *ctx;
}

void ensure_success(const char* what, sgx_status_t status, sgx_status_t ocall_status) {
  if (status != SGX_SUCCESS) {
    throw std::runtime_error(std::string(what) + " sgx=0x" + std::to_string(status));
  }
  if (ocall_status != SGX_SUCCESS) {
    throw std::runtime_error(std::string(what) + " host=0x" + std::to_string(ocall_status));
  }
}

sn::util::future<void> make_ready_future() {
  sn::util::promise<void> promise;
  auto fut = promise.get_future();
  promise.set_value();
  return fut;
}

}

void init() {}

void finalize() {}

int rank() {
  auto& ctx = require_context();
  int value = -1;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_dist_rank(&ocall_status, ctx.host_cookie, &value);
  ensure_success("dist_rank", status, ocall_status);
  return value;
}

int world_size() {
  auto& ctx = require_context();
  int value = 0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_dist_world_size(&ocall_status, ctx.host_cookie, &value);
  ensure_success("dist_world_size", status, ocall_status);
  return value;
}

sn::util::future<void> async_send_bytes(int dest_rank, byte_span payload) {
  send_bytes(dest_rank, payload);
  return make_ready_future();
}

void send_bytes(int dest_rank, byte_span payload) {
  const std::size_t bytes = payload.size();
  const std::size_t host_bytes = bytes == 0 ? std::size_t{1} : bytes;
  sn::sgxbridge::enclave::hostbuf::buffer buffer = sn::sgxbridge::enclave::hostbuf::buffer::allocate(host_bytes);
  if (bytes != 0) {
    sn::obliv::memcpy(buffer.data(), payload.data(), bytes);
  }
  send_from_host_buffer(dest_rank, host_buffer_handle{buffer.id()}, 0, bytes);
}

void send_from_host_buffer(int dest_rank, host_buffer_handle handle, std::size_t offset, std::size_t size) {
  if (!handle) {
    throw std::runtime_error("send_from_host_buffer requires valid handle");
  }
  auto& ctx = require_context();
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status =
      sonic_sgxbridge_dist_send_from_hostbuf(&ocall_status, ctx.host_cookie, dest_rank, handle.id, offset, size);
  ensure_success("dist_send_from_hostbuf", status, ocall_status);
}

bool try_recv(message_descriptor& out) {
  auto& ctx = require_context();
  int has_msg = 0;
  int src_rank = -1;
  std::uint64_t buffer_id = 0;
  std::size_t size = 0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status =
      sonic_sgxbridge_dist_try_recv(&ocall_status, ctx.host_cookie, &has_msg, &src_rank, &buffer_id, &size);
  ensure_success("dist_try_recv", status, ocall_status);
  if (!has_msg) {
    return false;
  }
  out.src_rank = src_rank;
  out.buffer.id = buffer_id;
  out.payload_size = size;
  return true;
}

message_descriptor recv() {
  auto& ctx = require_context();
  int src_rank = -1;
  std::uint64_t buffer_id = 0;
  std::size_t size = 0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_dist_recv(&ocall_status, ctx.host_cookie, &src_rank, &buffer_id, &size);
  ensure_success("dist_recv", status, ocall_status);
  message_descriptor msg{};
  msg.src_rank = src_rank;
  msg.buffer.id = buffer_id;
  msg.payload_size = size;
  return msg;
}

bool recv_for(message_descriptor& out, recv_timeout timeout) {
  auto& ctx = require_context();
  int has_msg = 0;
  int src_rank = -1;
  std::uint64_t buffer_id = 0;
  std::size_t size = 0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status =
      sonic_sgxbridge_dist_recv_for(&ocall_status, ctx.host_cookie, timeout, &has_msg, &src_rank, &buffer_id, &size);
  ensure_success("dist_recv_for", status, ocall_status);
  if (!has_msg) {
    return false;
  }
  out.src_rank = src_rank;
  out.buffer.id = buffer_id;
  out.payload_size = size;
  return true;
}

void release(const message_descriptor& msg) {
  if (!msg.buffer) {
    return;
  }
  sn::sgxbridge::enclave::hostbuf::release(msg.buffer.id);
}

std::size_t pending_messages() {
  auto& ctx = require_context();
  std::size_t count = 0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_dist_pending_messages(&ocall_status, ctx.host_cookie, &count);
  ensure_success("dist_pending_messages", status, ocall_status);
  return count;
}

void barrier() {
  auto& ctx = require_context();
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_dist_barrier(&ocall_status, ctx.host_cookie);
  ensure_success("dist_barrier", status, ocall_status);
}

double allreduce_sum(double local_value) {
  auto& ctx = require_context();
  double result = 0.0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_dist_allreduce_sum(&ocall_status, ctx.host_cookie, local_value, &result);
  ensure_success("dist_allreduce_sum", status, ocall_status);
  return result;
}

byte_span map_readonly(host_buffer_handle handle) {
  if (!handle) {
    return byte_span(nullptr, std::size_t{0});
  }
  auto mapping = sn::sgxbridge::enclave::hostbuf::map(handle.id);
  return byte_span(mapping.data, mapping.size);
}

byte_span_mut map_writable(host_buffer_handle handle) {
  if (!handle) {
    return byte_span_mut(nullptr, std::size_t{0});
  }
  auto mapping = sn::sgxbridge::enclave::hostbuf::map(handle.id);
  return byte_span_mut(mapping.data, mapping.size);
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

}

#endif
