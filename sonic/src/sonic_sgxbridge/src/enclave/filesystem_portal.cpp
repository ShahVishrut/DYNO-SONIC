#include "sonic/sgxbridge/fs/filesystem.hpp"

#include <stdexcept>
#include <string>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"

namespace sn::sgxbridge::fs {
namespace {

common::enclave_execution_context& require_context() {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    throw std::runtime_error("sgxbridge fs");
  }
  return *ctx;
}

status decode_status(std::uint32_t raw) { return static_cast<status>(raw); }

remove_result make_remove_error(status code) {
  remove_result out{};
  out.code = code;
  out.removed = false;
  return out;
}

remove_all_result make_remove_all_error(status code) {
  remove_all_result out{};
  out.code = code;
  out.removed = 0;
  return out;
}

exists_result make_exists_error(status code) {
  exists_result out{};
  out.code = code;
  out.exists = false;
  return out;
}

directories_result make_directories_error(status code) {
  directories_result out{};
  out.code = code;
  out.created = false;
  return out;
}

rename_result make_rename_error(status code) {
  rename_result out{};
  out.code = code;
  out.renamed = false;
  return out;
}

copy_file_result make_copy_file_error(status code) {
  copy_file_result out{};
  out.code = code;
  out.copied = false;
  return out;
}

file_size_result make_file_size_error(status code) {
  file_size_result out{};
  out.code = code;
  out.size = 0;
  return out;
}

}

remove_result remove_file(const std::string& path) {
  if (path.empty()) {
    return make_remove_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint8_t removed = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status =
      sonic_sgxbridge_fs_remove(&ocall_status, ctx.host_cookie, path.c_str(), &removed, &host_status);
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_remove_error(status::io_error);
  }
  remove_result out{};
  out.code = decode_status(host_status);
  out.removed = removed != 0;
  return out;
}

remove_all_result remove_all(const std::string& path) {
  if (path.empty()) {
    return make_remove_all_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint64_t removed = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status =
      sonic_sgxbridge_fs_remove_all(&ocall_status, ctx.host_cookie, path.c_str(), &removed, &host_status);
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_remove_all_error(status::io_error);
  }
  remove_all_result out{};
  out.code = decode_status(host_status);
  out.removed = removed;
  return out;
}

exists_result exists(const std::string& path) {
  if (path.empty()) {
    return make_exists_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint8_t exists_flag = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status =
      sonic_sgxbridge_fs_exists(&ocall_status, ctx.host_cookie, path.c_str(), &exists_flag, &host_status);
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_exists_error(status::io_error);
  }
  exists_result out{};
  out.code = decode_status(host_status);
  out.exists = exists_flag != 0;
  return out;
}

directories_result create_directories(const std::string& path) {
  if (path.empty()) {
    return make_directories_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint8_t created = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status =
      sonic_sgxbridge_fs_create_directories(&ocall_status, ctx.host_cookie, path.c_str(), &created, &host_status);
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_directories_error(status::io_error);
  }
  directories_result out{};
  out.code = decode_status(host_status);
  out.created = created != 0;
  return out;
}

rename_result rename(const std::string& from, const std::string& to) {
  if (from.empty() || to.empty()) {
    return make_rename_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint8_t renamed = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status =
      sonic_sgxbridge_fs_rename(&ocall_status, ctx.host_cookie, from.c_str(), to.c_str(), &renamed, &host_status);
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_rename_error(status::io_error);
  }
  rename_result out{};
  out.code = decode_status(host_status);
  out.renamed = renamed != 0;
  return out;
}

copy_file_result copy_file(const std::string& from, const std::string& to, bool overwrite_existing) {
  if (from.empty() || to.empty()) {
    return make_copy_file_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint8_t copied = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status = sonic_sgxbridge_fs_copy_file(
      &ocall_status, ctx.host_cookie, from.c_str(), to.c_str(), overwrite_existing ? 1u : 0u, &copied, &host_status
  );
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_copy_file_error(status::io_error);
  }
  copy_file_result out{};
  out.code = decode_status(host_status);
  out.copied = copied != 0;
  return out;
}

file_size_result file_size(const std::string& path) {
  if (path.empty()) {
    return make_file_size_error(status::invalid_arguments);
  }
  auto& ctx = require_context();
  std::uint64_t size = 0;
  std::uint32_t host_status = static_cast<std::uint32_t>(status::io_error);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t sgx_status =
      sonic_sgxbridge_fs_file_size(&ocall_status, ctx.host_cookie, path.c_str(), &size, &host_status);
  if (sgx_status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return make_file_size_error(status::io_error);
  }
  file_size_result out{};
  out.code = decode_status(host_status);
  out.size = size;
  return out;
}

}
