#include "sonic/sgxbridge/host/enclave_session.hpp"

namespace sn::sgxbridge::host {

enclave_session::~enclave_session() { close(); }

bool enclave_session::open(
    const std::string& enclave_path, std::uint32_t debug_flags, sn::threads::thread_context threads,
    bool enable_transport
) {
  if (is_open()) {
    return true;
  }

  sgx_launch_token_t launch_token{};
  int updated = 0;
  sgx_enclave_id_t new_id = 0;
  const sgx_status_t status =
      sgx_create_enclave(enclave_path.c_str(), debug_flags, &launch_token, &updated, &new_id, nullptr);
  if (status != SGX_SUCCESS) {
    return false;
  }

  enclave_id_ = new_id;
  if (!services_.initialize(enclave_id_, std::move(threads), nullptr, enable_transport)) {
    sgx_destroy_enclave(enclave_id_);
    enclave_id_ = 0;
    return false;
  }
  return true;
}

void enclave_session::close() {
  if (!is_open()) {
    return;
  }
  services_.shutdown();
  sgx_destroy_enclave(enclave_id_);
  enclave_id_ = 0;
}

}
