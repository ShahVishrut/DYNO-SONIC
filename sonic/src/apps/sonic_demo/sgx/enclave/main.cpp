#include <sgx_error.h>

#include "sonic_demo_t.h"

#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/logic/dispatcher.hpp"
#include "sonic/demo/types/intents.hpp"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/support.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"
#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/common/sync.hpp"
#include "sonic/util/log.hpp"

namespace {

struct enclave_environment {
  sn::sgxbridge::common::enclave_execution_context context{};
  sn::sgxbridge::tp::registry registry{};
  sn::sgxbridge::sync::dispatcher dispatcher{};
};

enclave_environment& global_environment() {
  static enclave_environment env;
  env.context.registry = &env.registry;
  env.context.dispatcher = &env.dispatcher;
  return env;
}

}

sgx_status_t sonic_demo_run(
    const void* intent_buffer, void* result_buffer, const sonic_demo_run_args* raw_args, void* host_state
) {
  if (intent_buffer == nullptr || result_buffer == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }

  auto& env = global_environment();
  env.context.host_cookie = host_state;
  env.context.app_context = nullptr;
  env.context.ns_per_cycle = 0.0;
  env.context.has_cycle_scale = false;

  sn::sgxbridge::enclave::execution_context_guard guard(env.context);

  const auto* intent = static_cast<const sn::demo::types::command_intent*>(intent_buffer);
  auto* result = static_cast<sn::demo::types::command_result*>(result_buffer);

  sn::sgxbridge::common::run_arguments run_args{};
  if (raw_args != nullptr) {
    run_args.verbosity = raw_args->verbosity;
    run_args.flags = raw_args->flags;
  }

  sn::sgxbridge::enclave::apply_global_verbosity(run_args.verbosity);

  sn::sgxbridge::enclave::threadpool_portal portal(&env.context);
  auto provider = sn::sgxbridge::enclave::make_threadpool_provider(portal);

  sn::demo::logic::execution_context logic_ctx{};
  logic_ctx.domain = sn::demo::logic::execution_domain::sgx_enclave;
  logic_ctx.threadpools = provider;
  logic_ctx.verbosity = run_args.verbosity;
  logic_ctx.logger = sn::util::log::create("sonic_demo.enclave");

  sn::sgxbridge::enclave::execute_with_exception_boundary(
      logic_ctx.logger, *result,
      [](std::string_view message) {
        return sn::demo::types::make_result(sn::demo::types::result_status::internal_error, message);
      },
      [&]() { return sn::demo::logic::execute_command(*intent, logic_ctx); }, [] {}
  );
  return SGX_SUCCESS;
}
