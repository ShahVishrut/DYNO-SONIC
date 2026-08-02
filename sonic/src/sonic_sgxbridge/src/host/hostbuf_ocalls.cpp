#include <cstddef>
#include <cstdint>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/host/host_buffer_manager.hpp"

namespace {

sn::sgxbridge::hostbuf::host_buffer_manager* resolve_buffers(void* host_state_ptr) {
  if (host_state_ptr == nullptr) {
    return nullptr;
  }
  auto* ctx = static_cast<sn::sgxbridge::common::host_execution_context*>(host_state_ptr);
  return ctx->host_buffers;
}

sgx_status_t map_hostbuf_status(sn::sgxbridge::hostbuf::status value) {
  using sn::sgxbridge::hostbuf::status;
  switch (value) {
  case status::ok:
    return SGX_SUCCESS;
  case status::invalid_arguments:
    return SGX_ERROR_INVALID_PARAMETER;
  case status::not_found:
    return SGX_ERROR_INVALID_PARAMETER;
  case status::limit_reached:
    return SGX_ERROR_OUT_OF_MEMORY;
  case status::internal_error:
  default:
    return SGX_ERROR_UNEXPECTED;
  }
}

}

extern "C" sgx_status_t sonic_sgxbridge_hostbuf_create(
    void* host_state_ptr, std::size_t size, void** out_ptr, std::size_t* out_size, std::uint64_t* out_id,
    std::uint32_t* out_status
) {
  if (out_ptr == nullptr || out_size == nullptr || out_id == nullptr || out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_ptr = nullptr;
  *out_size = 0;
  *out_id = 0;
  *out_status = sn::sgxbridge::hostbuf::to_u32(sn::sgxbridge::hostbuf::status::invalid_arguments);

  auto* manager = resolve_buffers(host_state_ptr);
  if (manager == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  sn::sgxbridge::hostbuf::descriptor desc{};
  const auto create_result = manager->allocate(size, desc);
  *out_status = sn::sgxbridge::hostbuf::to_u32(create_result.code);
  if (!create_result.succeeded()) {
    return map_hostbuf_status(create_result.code);
  }
  *out_ptr = static_cast<void*>(desc.data);
  *out_size = desc.size;
  *out_id = desc.id;
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_hostbuf_release(
    void* host_state_ptr, std::uint64_t id, std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = sn::sgxbridge::hostbuf::to_u32(sn::sgxbridge::hostbuf::status::invalid_arguments);
  auto* manager = resolve_buffers(host_state_ptr);
  if (manager == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto release_status = manager->release(id);
  *out_status = sn::sgxbridge::hostbuf::to_u32(release_status);
  return map_hostbuf_status(release_status);
}

extern "C" sgx_status_t sonic_sgxbridge_hostbuf_view(
    void* host_state_ptr, std::uint64_t id, void** out_ptr, std::size_t* out_size, std::uint32_t* out_status
) {
  if (out_ptr == nullptr || out_size == nullptr || out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_ptr = nullptr;
  *out_size = 0;
  *out_status = sn::sgxbridge::hostbuf::to_u32(sn::sgxbridge::hostbuf::status::invalid_arguments);
  auto* manager = resolve_buffers(host_state_ptr);
  if (manager == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  sn::sgxbridge::hostbuf::descriptor desc{};
  const auto view_result = manager->view(id, desc);
  *out_status = sn::sgxbridge::hostbuf::to_u32(view_result.code);
  if (!view_result.succeeded()) {
    return map_hostbuf_status(view_result.code);
  }
  *out_ptr = static_cast<void*>(desc.data);
  *out_size = desc.size;
  return SGX_SUCCESS;
}
