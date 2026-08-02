#pragma once

#include <sgx_error.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace sn::util::sgx {

inline std::string status_string(sgx_status_t status) {
  switch (status) {
  case SGX_SUCCESS:
    return "SGX_SUCCESS";
  case SGX_ERROR_UNEXPECTED:
    return "SGX_ERROR_UNEXPECTED";
  case SGX_ERROR_INVALID_PARAMETER:
    return "SGX_ERROR_INVALID_PARAMETER";
  case SGX_ERROR_OUT_OF_MEMORY:
    return "SGX_ERROR_OUT_OF_MEMORY";
  case SGX_ERROR_ENCLAVE_LOST:
    return "SGX_ERROR_ENCLAVE_LOST";
  case SGX_ERROR_DEVICE_BUSY:
    return "SGX_ERROR_DEVICE_BUSY";
  case SGX_ERROR_INVALID_ENCLAVE:
    return "SGX_ERROR_INVALID_ENCLAVE";
  case SGX_ERROR_INVALID_ENCLAVE_ID:
    return "SGX_ERROR_INVALID_ENCLAVE_ID";
  case SGX_ERROR_INVALID_SIGNATURE:
    return "SGX_ERROR_INVALID_SIGNATURE";
  case SGX_ERROR_OUT_OF_EPC:
    return "SGX_ERROR_OUT_OF_EPC";
  case SGX_ERROR_NO_DEVICE:
    return "SGX_ERROR_NO_DEVICE";
  case SGX_ERROR_MEMORY_MAP_CONFLICT:
    return "SGX_ERROR_MEMORY_MAP_CONFLICT";
  case SGX_ERROR_INVALID_STATE:
    return "SGX_ERROR_INVALID_STATE";
  case SGX_ERROR_SERVICE_UNAVAILABLE:
    return "SGX_ERROR_SERVICE_UNAVAILABLE";
  case SGX_ERROR_SERVICE_TIMEOUT:
    return "SGX_ERROR_SERVICE_TIMEOUT";
  case SGX_ERROR_SERVICE_INVALID_PRIVILEGE:
    return "SGX_ERROR_SERVICE_INVALID_PRIVILEGE";
  case SGX_ERROR_ENCLAVE_FILE_ACCESS:
    return "SGX_ERROR_ENCLAVE_FILE_ACCESS";
  default:
    return "SGX_ERROR_UNKNOWN(" + std::to_string(static_cast<int>(status)) + ")";
  }
}

inline const char* status_description(sgx_status_t status) {
  switch (status) {
  case SGX_SUCCESS:
    return "operation completed successfully";
  case SGX_ERROR_UNEXPECTED:
    return "unexpected error from SGX runtime";
  case SGX_ERROR_INVALID_PARAMETER:
    return "invalid parameter passed to SGX runtime";
  case SGX_ERROR_OUT_OF_MEMORY:
    return "not enough memory available for the enclave";
  case SGX_ERROR_ENCLAVE_LOST:
    return "enclave lost after a power event or crash";
  case SGX_ERROR_DEVICE_BUSY:
    return "SGX device is busy; retry later";
  case SGX_ERROR_INVALID_ENCLAVE:
    return "enclave image is invalid or corrupted";
  case SGX_ERROR_INVALID_ENCLAVE_ID:
    return "invalid enclave identifier";
  case SGX_ERROR_INVALID_SIGNATURE:
    return "enclave signature verification failed";
  case SGX_ERROR_OUT_OF_EPC:
    return "not enough EPC memory to complete the operation";
  case SGX_ERROR_NO_DEVICE:
    return "SGX device not found or disabled";
  case SGX_ERROR_MEMORY_MAP_CONFLICT:
    return "memory mapping conflict detected";
  case SGX_ERROR_INVALID_STATE:
    return "enclave is in an invalid state for this operation";
  case SGX_ERROR_SERVICE_UNAVAILABLE:
    return "platform service is unavailable";
  case SGX_ERROR_SERVICE_TIMEOUT:
    return "platform service timed out";
  case SGX_ERROR_SERVICE_INVALID_PRIVILEGE:
    return "privilege level insufficient for platform service";
  case SGX_ERROR_ENCLAVE_FILE_ACCESS:
    return "failed to access enclave file";
  default:
    return "";
  }
}

inline std::string errno_string(int err) { return std::strerror(err); }

inline std::string current_errno_string() { return errno_string(errno); }

}
