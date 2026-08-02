#include "sonic/sgxbridge/host/host_services.hpp"

#include <utility>

namespace sn::sgxbridge::host {

host_services::host_services() : logger_(sn::util::log::create("sgx.threadpool.host")) { reset_execution_context(); }

host_services::~host_services() { shutdown(); }

void host_services::set_logger(sn::util::log::logger logger) {
  logger_ = std::move(logger);
  context_.logger = &logger_;
}

#if SONIC_SGXBRIDGE_DISTRIBUTED
void host_services::configure_transport(dist::config cfg) {
  set_host_buffer_limits(cfg.max_host_buffer_bytes);
  dist_config_ = std::move(cfg);
}
#endif

void host_services::set_host_buffer_limits(std::size_t max_total_bytes) {
  hostbuf::host_buffer_manager::config cfg{};
  cfg.max_total_bytes = max_total_bytes;
  cfg.max_single_allocation = max_total_bytes;
  host_buffers_.configure(cfg);
}

bool host_services::initialize(
    sgx_enclave_id_t enclave_id, sn::threads::thread_context threads, void* app_context, bool enable_transport
) {
  shutdown();
  manager_.configure(enclave_id, std::move(threads), &logger_);
  bool dist_enabled = false;
#if SONIC_SGXBRIDGE_DISTRIBUTED
  if (enable_transport) {
    dist::config cfg = dist_config_.value_or(dist::config{});
    cfg.external_host_buffers = &host_buffers_;
    dist::init(cfg);
    dist_initialized_ = true;
    dist_enabled = true;
  }
#else
  (void) enable_transport;
#endif
  publish_execution_context(app_context, dist_enabled);
  return true;
}

void host_services::shutdown() {
  manager_.shutdown();
#if SONIC_SGXBRIDGE_DISTRIBUTED
  if (dist_initialized_) {
    dist::finalize();
    dist_initialized_ = false;
  }
#endif
  storage_.shutdown();
  host_buffers_.shutdown();
  reset_execution_context();
}

void host_services::publish_execution_context(void* app_context, bool dist_enabled) noexcept {
  threadpool_services_.manager = &manager_;
  context_.threadpools = &threadpool_services_;
  context_.logger = &logger_;
  context_.host_buffers = &host_buffers_;
  context_.storage = &storage_;
  context_.app_context = app_context;
  context_.dist_enabled = dist_enabled;
}

void host_services::reset_execution_context() noexcept {
  threadpool_services_.manager = nullptr;
  context_ = {};
  context_.logger = &logger_;
}

}
