#include <cstddef>
#include <cstdint>

#include <sgx_error.h>

#include "sonic/sgxbridge/fs/filesystem.hpp"

extern "C" sgx_status_t sonic_sgxbridge_fs_remove(
    void* host_state_ptr, const char* path, std::uint8_t* out_removed, std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_removed == nullptr || out_status == nullptr || path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = sn::sgxbridge::fs::remove_file(path);
  *out_removed = result.removed ? 1u : 0u;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_fs_remove_all(
    void* host_state_ptr, const char* path, std::uint64_t* out_removed, std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_removed == nullptr || out_status == nullptr || path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = sn::sgxbridge::fs::remove_all(path);
  *out_removed = result.removed;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_fs_exists(
    void* host_state_ptr, const char* path, std::uint8_t* out_exists, std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_exists == nullptr || out_status == nullptr || path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = sn::sgxbridge::fs::exists(path);
  *out_exists = result.exists ? 1u : 0u;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_fs_create_directories(
    void* host_state_ptr, const char* path, std::uint8_t* out_created, std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_created == nullptr || out_status == nullptr || path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = sn::sgxbridge::fs::create_directories(path);
  *out_created = result.created ? 1u : 0u;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_fs_rename(
    void* host_state_ptr, const char* from_path, const char* to_path, std::uint8_t* out_renamed,
    std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_renamed == nullptr || out_status == nullptr || from_path == nullptr || to_path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = sn::sgxbridge::fs::rename(from_path, to_path);
  *out_renamed = result.renamed ? 1u : 0u;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_fs_copy_file(
    void* host_state_ptr, const char* from_path, const char* to_path, std::uint8_t overwrite_existing,
    std::uint8_t* out_copied, std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_copied == nullptr || out_status == nullptr || from_path == nullptr || to_path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const bool overwrite = overwrite_existing != 0;
  const auto result = sn::sgxbridge::fs::copy_file(from_path, to_path, overwrite);
  *out_copied = result.copied ? 1u : 0u;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_fs_file_size(
    void* host_state_ptr, const char* path, std::uint64_t* out_size, std::uint32_t* out_status
) {
  (void) host_state_ptr;
  if (out_size == nullptr || out_status == nullptr || path == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = sn::sgxbridge::fs::file_size(path);
  *out_size = result.size;
  *out_status = static_cast<std::uint32_t>(result.code);
  return SGX_SUCCESS;
}
