#pragma once

#include <cstdint>
#include <string>

#include <sgx_eid.h>
#include <sgx_urts.h>

#include "sonic/sgxbridge/host/host_services.hpp"

namespace sn::sgxbridge::host {

class enclave_session {
public:
  enclave_session() = default;
  ~enclave_session();

  bool open(
      const std::string& enclave_path, std::uint32_t debug_flags, sn::threads::thread_context threads,
      bool enable_transport = false
  );
  void close();

  [[nodiscard]] bool is_open() const noexcept { return enclave_id_ != 0; }
  [[nodiscard]] sgx_enclave_id_t id() const noexcept { return enclave_id_; }
  [[nodiscard]] host_services& services() noexcept { return services_; }
  [[nodiscard]] const host_services& services() const noexcept { return services_; }
  [[nodiscard]] sn::sgxbridge::common::host_execution_context* host_state() noexcept { return services_.host_state(); }
  [[nodiscard]] const sn::sgxbridge::common::host_execution_context* host_state() const noexcept {
    return services_.host_state();
  }

private:
  sgx_enclave_id_t enclave_id_{0};
  host_services services_{};
};

}
