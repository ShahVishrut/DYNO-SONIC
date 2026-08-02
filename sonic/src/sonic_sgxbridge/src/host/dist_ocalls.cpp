#if SONIC_SGXBRIDGE_DISTRIBUTED
#include "sonic/sgxbridge/dist/api.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/host/host_buffer_manager.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace {

using sn::sgxbridge::common::host_execution_context;
using sn::sgxbridge::hostbuf::host_buffer_manager;

host_execution_context* get_context(void* host_state_ptr) {
  return static_cast<host_execution_context*>(host_state_ptr);
}

bool dist_enabled(void* host_state_ptr) {
  auto* ctx = get_context(host_state_ptr);
  return ctx != nullptr && ctx->dist_enabled;
}

host_buffer_manager* get_buffers(void* host_state_ptr) {
  auto* ctx = get_context(host_state_ptr);
  if (ctx == nullptr) {
    return nullptr;
  }
  return ctx->host_buffers;
}

sn::util::log::logger* get_logger(void* host_state_ptr) {
  auto* ctx = get_context(host_state_ptr);
  if (ctx == nullptr) {
    return nullptr;
  }
  return ctx->logger;
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

extern "C" sgx_status_t sonic_sgxbridge_dist_rank(void* host_state_ptr, int* out_rank) {
  if (out_rank == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    *out_rank = sn::sgxbridge::dist::rank();
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_world_size(void* host_state_ptr, int* out_size) {
  if (out_size == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    *out_size = sn::sgxbridge::dist::world_size();
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_send_from_hostbuf(
    void* host_state_ptr, int dest_rank, std::uint64_t buffer_id, std::size_t offset, std::size_t length
) {
  auto* log = get_logger(host_state_ptr);
  if (buffer_id == 0) {
    if (log != nullptr) {
      log->err("dist buffer");
    }
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    if (log != nullptr) {
      log->err("dist state");
    }
    return SGX_ERROR_INVALID_STATE;
  }
  auto* buffers = get_buffers(host_state_ptr);
  if (buffers == nullptr) {
    if (log != nullptr) {
      log->err("dist buffer");
    }
    return SGX_ERROR_INVALID_STATE;
  }

  sn::sgxbridge::hostbuf::descriptor desc{};
  const auto view_result = buffers->view(buffer_id, desc);
  if (!view_result.succeeded()) {
    if (log != nullptr) {
      log->err("dist buffer");
    }
    return map_hostbuf_status(view_result.code);
  }
  if (offset > desc.size || length > desc.size || (offset + length) > desc.size) {
    if (log != nullptr) {
      log->err("dist range");
    }
    return SGX_ERROR_INVALID_PARAMETER;
  }

  try {
    sn::sgxbridge::dist::host_buffer_handle handle{buffer_id};
    sn::sgxbridge::dist::send_from_host_buffer(dest_rank, handle, offset, length);
  } catch (const std::exception&) {
    if (log != nullptr) {
      log->err("dist send");
    }
    return SGX_ERROR_UNEXPECTED;
  } catch (...) {
    if (log != nullptr) {
      log->err("dist send");
    }
    return SGX_ERROR_UNEXPECTED;
  }

  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_try_recv(
    void* host_state_ptr, int* out_has_msg, int* out_src_rank, std::uint64_t* out_buffer_id, std::size_t* out_size
) {
  if (out_has_msg == nullptr || out_src_rank == nullptr || out_buffer_id == nullptr || out_size == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }

  sn::sgxbridge::dist::message_descriptor msg{};
  try {
    if (!sn::sgxbridge::dist::try_recv(msg)) {
      *out_has_msg = 0;
      *out_src_rank = -1;
      *out_buffer_id = 0;
      *out_size = 0;
      return SGX_SUCCESS;
    }
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }

  *out_has_msg = 1;
  *out_src_rank = msg.src_rank;
  *out_buffer_id = msg.buffer.id;
  *out_size = msg.payload_size;
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_recv(
    void* host_state_ptr, int* out_src_rank, std::uint64_t* out_buffer_id, std::size_t* out_size
) {
  if (out_src_rank == nullptr || out_buffer_id == nullptr || out_size == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    auto msg = sn::sgxbridge::dist::recv();
    *out_src_rank = msg.src_rank;
    *out_buffer_id = msg.buffer.id;
    *out_size = msg.payload_size;
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_recv_for(
    void* host_state_ptr, std::uint64_t timeout_ms, int* out_has_msg, int* out_src_rank, std::uint64_t* out_buffer_id,
    std::size_t* out_size
) {
  if (out_has_msg == nullptr || out_src_rank == nullptr || out_buffer_id == nullptr || out_size == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    sn::sgxbridge::dist::message_descriptor msg{};
    const auto clamped = std::min(timeout_ms, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
    if (!sn::sgxbridge::dist::recv_for(msg, std::chrono::milliseconds(clamped))) {
      *out_has_msg = 0;
      *out_src_rank = -1;
      *out_buffer_id = 0;
      *out_size = 0;
      return SGX_SUCCESS;
    }
    *out_has_msg = 1;
    *out_src_rank = msg.src_rank;
    *out_buffer_id = msg.buffer.id;
    *out_size = msg.payload_size;
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_pending_messages(void* host_state_ptr, std::size_t* out_count) {
  if (out_count == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    *out_count = sn::sgxbridge::dist::pending_messages();
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_barrier(void* host_state_ptr) {
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    sn::sgxbridge::dist::barrier();
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_dist_allreduce_sum(
    void* host_state_ptr, double local_value, double* out_value
) {
  if (out_value == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  if (!dist_enabled(host_state_ptr)) {
    return SGX_ERROR_INVALID_STATE;
  }
  try {
    *out_value = sn::sgxbridge::dist::allreduce_sum(local_value);
  } catch (...) {
    return SGX_ERROR_UNEXPECTED;
  }
  return SGX_SUCCESS;
}

#endif
