#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <sgx_error.h>

#include "sonic_demo_u.h"

#include "sonic/demo/cli/parser.hpp"
#include "sonic/demo/types/intents.hpp"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/host/enclave_session.hpp"
#include "sonic/threads/tuning.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/sgx/path.hpp"
#include "sonic/util/sgx/status.hpp"

namespace {

std::string resolve_enclave_path() {
  if (const char* override_path = std::getenv("SONIC_DEMO_SGX_ENCLAVE"); override_path != nullptr) {
    if (std::strlen(override_path) != 0) {
      return std::string(override_path);
    }
  }
  return sn::util::sgx::default_enclave_path("sonic_demo_enclave.signed.so");
}

sn::demo::types::command_result make_error(sn::demo::types::result_status status, const std::string& message) {
  return sn::demo::types::make_result(status, message);
}

sn::demo::types::command_result format_sgx_error(const char* context, sgx_status_t status) {
  std::string text = context;
  text += ": ";
  text += sn::util::sgx::status_string(status);
  text += " (0x";
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned>(status));
  text += buffer;
  text += ")";
  return make_error(sn::demo::types::result_status::internal_error, text);
}

sn::demo::types::command_result invoke_enclave(
    sn::sgxbridge::host::enclave_session& session, const sn::demo::types::command_intent& intent,
    std::uint32_t verbosity
) {
  if (!session.is_open()) {
    return make_error(sn::demo::types::result_status::internal_error, "enclave session not open");
  }

  sn::demo::types::command_intent intent_copy = intent;
  sn::demo::types::command_result result{};

  static_assert(sizeof(sonic_demo_run_args) == sizeof(sn::sgxbridge::common::run_arguments), "run arg layout mismatch");

  sonic_demo_run_args args{};
  args.verbosity = verbosity;
  args.flags = 0;

  sgx_status_t enclave_status = SGX_ERROR_UNEXPECTED;
  auto* host_state = session.host_state();
  if (host_state == nullptr) {
    return make_error(sn::demo::types::result_status::internal_error, "host services unavailable");
  }

  const sgx_status_t status = sonic_demo_run(session.id(), &enclave_status, &intent_copy, &result, &args, host_state);
  if (status != SGX_SUCCESS) {
    return format_sgx_error("ECALL sonic_demo_run failed", status);
  }
  if (enclave_status != SGX_SUCCESS) {
    return format_sgx_error("enclave rejected command", enclave_status);
  }
  return result;
}

int run_host(int argc, const char** argv) {
  auto parse = sn::demo::cli::parse_command_line(argc, argv);
  if (parse.show_help) {
    return 0;
  }
  if (!parse.success) {
    return 1;
  }

  sn::demo::cli::apply_logging_preferences(parse);
  auto host_logger = sn::util::log::create("sonic_demo.host");

  sn::threads::thread_context threads(parse.thread_policy);
  threads.bind_current_thread();

  const std::string enclave_path = resolve_enclave_path();

  sn::sgxbridge::host::enclave_session session;
  session.services().set_logger(host_logger);
  const std::uint32_t debug_flag =
#if defined(SONIC_DEMO_SGX_DEBUG_FLAG)
      static_cast<std::uint32_t>(SONIC_DEMO_SGX_DEBUG_FLAG);
#else
      1u;
#endif
  if (!session.open(enclave_path, debug_flag, threads)) {
    std::cerr << "failed to open enclave at " << enclave_path << '\n';
    return 1;
  }

  const auto result = invoke_enclave(session, parse.intent, parse.verbosity);
  if (result.status != sn::demo::types::result_status::ok) {
    std::cerr << "enclave command failed: " << sn::demo::types::describe(result.status) << '\n';
    if (!result.output.empty()) {
      std::cerr << result.output.c_str() << '\n';
    }
    return 1;
  }

  if (!result.output.empty()) {
    std::cout << result.output.c_str() << '\n';
  }
  return 0;
}

}

int main(int argc, const char** argv) { return run_host(argc, argv); }
