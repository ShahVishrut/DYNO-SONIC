#pragma once

#include <optional>

#include <sgx_eid.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/host/host_buffer_manager.hpp"
#include "sonic/sgxbridge/host/threadpool_manager.hpp"
#include "sonic/sgxbridge/host/storage_manager.hpp"
#include "sonic/threads/tuning.hpp"

#if SONIC_SGXBRIDGE_DISTRIBUTED
#include "sonic/sgxbridge/dist/api.hpp"
#endif

namespace sn::sgxbridge::tp {

struct threadpool_services {
  host::threadpool_manager* manager{nullptr};
};

}

namespace sn::sgxbridge::host {

class host_services {
public:
  host_services();
  ~host_services();

  void set_logger(sn::util::log::logger logger);
#if SONIC_SGXBRIDGE_DISTRIBUTED
  void configure_transport(dist::config cfg);
#endif
  void set_host_buffer_limits(std::size_t max_total_bytes);

  bool initialize(
      sgx_enclave_id_t enclave_id, sn::threads::thread_context threads, void* app_context = nullptr,
      bool enable_transport = true
  );
  void shutdown();

  [[nodiscard]] common::host_execution_context execution_context() const noexcept { return context_; }
  [[nodiscard]] common::host_execution_context* host_state() noexcept { return &context_; }
  [[nodiscard]] const common::host_execution_context* host_state() const noexcept { return &context_; }

  [[nodiscard]] threadpool_manager& threadpools() noexcept { return manager_; }
  [[nodiscard]] sn::util::log::logger& logger() noexcept { return logger_; }

private:
  void publish_execution_context(void* app_context, bool dist_enabled) noexcept;
  void reset_execution_context() noexcept;

  threadpool_manager manager_;
  sn::util::log::logger logger_;
  tp::threadpool_services threadpool_services_{};
  hostbuf::host_buffer_manager host_buffers_;
  storage::manager storage_;
#if SONIC_SGXBRIDGE_DISTRIBUTED
  bool dist_initialized_{false};
  std::optional<dist::config> dist_config_;
#endif
  common::host_execution_context context_{};
};

}
