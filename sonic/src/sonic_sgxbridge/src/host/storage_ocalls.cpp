#include <cstddef>
#include <cstdint>

#include <sgx_error.h>

#include <vector>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/host/storage_manager.hpp"

namespace {

using sn::sgxbridge::storage::handle_t;
using sn::sgxbridge::storage::manager;
using sn::sgxbridge::storage::result;
using sn::sgxbridge::storage::status;

manager* resolve_storage(void* host_state_ptr) {
  if (host_state_ptr == nullptr) {
    return nullptr;
  }
  auto* ctx = static_cast<sn::sgxbridge::common::host_execution_context*>(host_state_ptr);
  return ctx->storage;
}

sgx_status_t map_status(status value) {
  switch (value) {
  case status::ok:
    return SGX_SUCCESS;
  case status::invalid_arguments:
  case status::not_found:
    return SGX_ERROR_INVALID_PARAMETER;
  case status::io_error:
  default:
    return SGX_ERROR_UNEXPECTED;
  }
}

sgx_status_t map_result(const result& r) { return map_status(r.code); }

}

extern "C" sgx_status_t sonic_sgxbridge_storage_open(
    void* host_state_ptr, const char* data_path, const char* meta_path, std::uint8_t create, std::uint8_t truncate,
    std::uint64_t* out_handle, std::uint32_t* out_status
) {
  if (out_handle == nullptr || out_status == nullptr || data_path == nullptr || meta_path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_handle = 0;
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);

  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }

  sn::sgxbridge::storage::open_config cfg{};
  cfg.data_path = data_path;
  cfg.meta_path = meta_path;
  cfg.create = create != 0;
  cfg.truncate = truncate != 0;

  const auto res = mgr->open(std::move(cfg), *out_handle);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_close(
    void* host_state_ptr, std::uint64_t handle, std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto st = mgr->close(handle);
  *out_status = static_cast<std::uint32_t>(st);
  return map_status(st);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_resize(
    void* host_state_ptr, std::uint64_t handle, std::uint64_t pages, std::size_t data_bytes, std::size_t meta_bytes,
    std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto res = mgr->resize(handle, pages, data_bytes, meta_bytes);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_read_data(
    void* host_state_ptr, std::uint64_t handle, std::uint64_t page_id, void* dst, std::size_t bytes,
    std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto res = mgr->read_data(handle, page_id, dst, bytes);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_write_data(
    void* host_state_ptr, std::uint64_t handle, std::uint64_t page_id, const void* src, std::size_t bytes,
    std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto res = mgr->write_data(handle, page_id, src, bytes);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_read_meta(
    void* host_state_ptr, std::uint64_t handle, std::uint64_t page_id, void* dst, std::size_t bytes,
    std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto res = mgr->read_meta(handle, page_id, dst, bytes);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_write_meta(
    void* host_state_ptr, std::uint64_t handle, std::uint64_t page_id, const void* src, std::size_t bytes,
    std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto res = mgr->write_meta(handle, page_id, src, bytes);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_write_pages(
    void* host_state_ptr, std::uint64_t handle, std::size_t page_count, const std::uint64_t* page_ids, const void* data,
    const void* meta, std::size_t data_bytes_per_page, std::size_t meta_bytes_per_page, std::uint32_t* out_status
) {
  if (out_status == nullptr || page_ids == nullptr || data == nullptr || meta == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  std::vector<sn::sgxbridge::storage::page_batch_view> views;
  views.reserve(page_count);
  for (std::size_t idx = 0; idx < page_count; ++idx) {
    const auto* data_ptr = static_cast<const std::uint8_t*>(data) + idx * data_bytes_per_page;
    const auto* meta_ptr = static_cast<const std::uint8_t*>(meta) + idx * meta_bytes_per_page;
    views.push_back(sn::sgxbridge::storage::page_batch_view{page_ids[idx], data_ptr, meta_ptr});
  }

  const auto res = mgr->write_pages(handle, views.data(), views.size(), data_bytes_per_page, meta_bytes_per_page);
  *out_status = static_cast<std::uint32_t>(res.code);
  return map_result(res);
}

extern "C" sgx_status_t sonic_sgxbridge_storage_flush(
    void* host_state_ptr, std::uint64_t handle, std::uint32_t* out_status
) {
  if (out_status == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(status::invalid_arguments);
  auto* mgr = resolve_storage(host_state_ptr);
  if (mgr == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto st = mgr->flush(handle);
  *out_status = static_cast<std::uint32_t>(st);
  return map_status(st);
}
