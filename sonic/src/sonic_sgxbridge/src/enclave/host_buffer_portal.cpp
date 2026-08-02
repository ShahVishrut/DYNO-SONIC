#include "sonic/sgxbridge/enclave/host_buffer_portal.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <sgx_error.h>
#include <sgx_trts.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"

namespace sn::sgxbridge::enclave::hostbuf {

namespace {

[[noreturn]] void throw_error(const char* what, sgx_status_t status) {
  throw std::runtime_error(std::string("hostbuf: ") + what);
}

[[noreturn]] void throw_host_error(const char* what, sn::sgxbridge::hostbuf::status code) {
  throw std::runtime_error(std::string("hostbuf: ") + what);
}

common::enclave_execution_context& require_context() {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    throw std::runtime_error("hostbuf portal");
  }
  return *ctx;
}

void ensure_host_memory(void* ptr, std::size_t size) {
  if (ptr == nullptr && size == 0) {
    return;
  }
  if (ptr == nullptr || !sgx_is_outside_enclave(ptr, size)) {
    throw std::runtime_error("hostbuf pointer");
  }
}

void ensure_success(const char* what, sgx_status_t status, sgx_status_t ocall_status) {
  if (status != SGX_SUCCESS) {
    throw_error(what, status);
  }
  if (ocall_status != SGX_SUCCESS) {
    throw_error(what, ocall_status);
  }
}

sn::sgxbridge::hostbuf::status decode_status(std::uint32_t raw) { return sn::sgxbridge::hostbuf::from_u32(raw); }

buffer make_buffer(std::size_t bytes) {
  auto& ctx = require_context();
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  void* host_ptr = nullptr;
  std::size_t host_size = 0;
  std::uint64_t id = 0;
  std::uint32_t host_status = sn::sgxbridge::hostbuf::to_u32(sn::sgxbridge::hostbuf::status::internal_error);
  const sgx_status_t status =
      sonic_sgxbridge_hostbuf_create(&ocall_status, ctx.host_cookie, bytes, &host_ptr, &host_size, &id, &host_status);
  ensure_success("hostbuf_create", status, ocall_status);
  const auto decoded = decode_status(host_status);
  if (decoded != sn::sgxbridge::hostbuf::status::ok) {
    throw_host_error("hostbuf_create", decoded);
  }
  ensure_host_memory(host_ptr, host_size);
  return buffer(
      sn::sgxbridge::hostbuf::buffer_id{id}, static_cast<std::uint8_t*>(host_ptr), host_size, ctx.host_cookie
  );
}

void release_buffer(sn::sgxbridge::hostbuf::buffer_id id, void* cookie) {
  if (cookie == nullptr || id == 0) {
    return;
  }
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  std::uint32_t host_status = sn::sgxbridge::hostbuf::to_u32(sn::sgxbridge::hostbuf::status::internal_error);
  const sgx_status_t status = sonic_sgxbridge_hostbuf_release(&ocall_status, cookie, id, &host_status);
  ensure_success("hostbuf_release", status, ocall_status);
  const auto decoded = decode_status(host_status);
  if (decoded != sn::sgxbridge::hostbuf::status::ok) {
    throw_host_error("hostbuf_release", decoded);
  }
}

}

buffer::buffer(sn::sgxbridge::hostbuf::buffer_id id, std::uint8_t* ptr, std::size_t size, void* cookie) :
    host_cookie_(cookie), id_(id), data_(ptr), size_(size) {}

buffer::buffer(buffer&& other) noexcept { *this = std::move(other); }

buffer& buffer::operator=(buffer&& other) noexcept {
  if (this != &other) {
    reset();
    host_cookie_ = other.host_cookie_;
    id_ = other.id_;
    data_ = other.data_;
    size_ = other.size_;
    other.host_cookie_ = nullptr;
    other.id_ = 0;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

buffer::~buffer() { reset(); }

buffer buffer::allocate(std::size_t bytes) { return make_buffer(bytes); }

void buffer::reset() noexcept {
  try {
    release_buffer(id_, host_cookie_);
  } catch (...) {
  }
  host_cookie_ = nullptr;
  id_ = 0;
  data_ = nullptr;
  size_ = 0;
}

mapped_view map(sn::sgxbridge::hostbuf::buffer_id id) {
  if (id == 0) {
    return mapped_view{};
  }
  auto& ctx = require_context();
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  void* host_ptr = nullptr;
  std::size_t host_size = 0;
  std::uint32_t host_status = sn::sgxbridge::hostbuf::to_u32(sn::sgxbridge::hostbuf::status::internal_error);
  const sgx_status_t status =
      sonic_sgxbridge_hostbuf_view(&ocall_status, ctx.host_cookie, id, &host_ptr, &host_size, &host_status);
  ensure_success("hostbuf_view", status, ocall_status);
  const auto decoded = decode_status(host_status);
  if (decoded != sn::sgxbridge::hostbuf::status::ok) {
    throw_host_error("hostbuf_view", decoded);
  }
  ensure_host_memory(host_ptr, host_size);
  mapped_view view{};
  view.data = static_cast<std::uint8_t*>(host_ptr);
  view.size = host_size;
  return view;
}

void release(sn::sgxbridge::hostbuf::buffer_id id) { release_buffer(id, require_context().host_cookie); }

}
