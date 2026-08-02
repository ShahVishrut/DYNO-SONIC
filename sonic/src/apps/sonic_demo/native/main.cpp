#include <iostream>

#include "sonic/demo/cli/parser.hpp"
#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/logic/dispatcher.hpp"
#include "sonic/sgxbridge/native/threadpool_provider.hpp"
#include "sonic/threads/tuning.hpp"
#include "sonic/util/log.hpp"

namespace {

class native_environment {
public:
  explicit native_environment(sn::threads::thread_context threads) {
    state_.threads = std::move(threads);
    provider_ = sn::sgxbridge::native::make_pthread_threadpool_provider(state_);
  }

  native_environment(const native_environment&) = delete;
  native_environment& operator=(const native_environment&) = delete;

  [[nodiscard]] sn::sgxbridge::tp::provider provider() const noexcept { return provider_; }

private:
  sn::sgxbridge::native::pthread_threadpool_state state_{};
  sn::sgxbridge::tp::provider provider_{};
};

int run_demo(int argc, const char** argv) {
  auto parse = sn::demo::cli::parse_command_line(argc, argv);
  if (parse.show_help) {
    return 0;
  }
  if (!parse.success) {
    return 1;
  }

  sn::demo::cli::apply_logging_preferences(parse);
  sn::threads::thread_context threads(parse.thread_policy);
  threads.bind_current_thread();

  native_environment env(threads);

  sn::demo::logic::execution_context ctx{};
  ctx.domain = sn::demo::logic::execution_domain::native;
  ctx.threadpools = env.provider();
  ctx.logger = sn::util::log::create("sonic_demo");
  ctx.verbosity = parse.verbosity;

  const auto result = sn::demo::logic::execute_command(parse.intent, ctx);
  if (result.status != sn::demo::types::result_status::ok) {
    std::cerr << "command failed: " << sn::demo::types::describe(result.status) << '\n';
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

int main(int argc, const char** argv) { return run_demo(argc, argv); }
