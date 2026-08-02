#pragma once

#include <sgx_urts.h>

#include <cerrno>
#include <utility>

namespace sn::util::sgx {

inline sgx_status_t destroy_enclave(sgx_enclave_id_t enclave_id) noexcept {
  if (enclave_id == 0) {
    return SGX_SUCCESS;
  }
  return sgx_destroy_enclave(enclave_id);
}

class enclave_session {
public:
  enclave_session() = default;
  explicit enclave_session(sgx_enclave_id_t enclave_id) : enclave_id_(enclave_id) {}

  ~enclave_session() { close(); }

  enclave_session(enclave_session&& other) noexcept : enclave_id_(other.enclave_id_) { other.enclave_id_ = 0; }

  enclave_session& operator=(enclave_session&& other) noexcept {
    if (this != &other) {
      close();
      enclave_id_ = other.enclave_id_;
      other.enclave_id_ = 0;
    }
    return *this;
  }

  enclave_session(const enclave_session&) = delete;
  enclave_session& operator=(const enclave_session&) = delete;

  [[nodiscard]] bool is_valid() const noexcept { return enclave_id_ != 0; }

  [[nodiscard]] sgx_enclave_id_t id() const noexcept { return enclave_id_; }

  sgx_status_t close() noexcept {
    sgx_status_t result = destroy_enclave(enclave_id_);
    enclave_id_ = 0;
    return result;
  }

private:
  sgx_enclave_id_t enclave_id_{0};
};

struct enclave_create_result {
  enclave_session session{};
  sgx_status_t status{SGX_ERROR_UNEXPECTED};
  int host_errno{0};

  [[nodiscard]] bool succeeded() const noexcept { return status == SGX_SUCCESS && session.is_valid(); }
};

inline enclave_create_result create_enclave_session(
    const char* path, bool debug, sgx_launch_token_t* token = nullptr, int* updated = nullptr,
    sgx_misc_attribute_t* misc_attr = nullptr
) {
  sgx_launch_token_t local_token{};
  int local_updated = 0;

  sgx_launch_token_t* token_ptr = token ? token : &local_token;
  int* updated_ptr = updated ? updated : &local_updated;

  sgx_enclave_id_t enclave_id = 0;
  errno = 0;
  sgx_status_t rc = sgx_create_enclave(path, debug ? 1 : 0, token_ptr, updated_ptr, &enclave_id, misc_attr);
  int err = errno;

  if (rc == SGX_SUCCESS) {
    return {enclave_session(enclave_id), rc, err};
  }

  return {enclave_session(), rc, err};
}

}
