#pragma once

#include "sonic/demo/logic/commands/hello.hpp"
#include "sonic/demo/logic/commands/parallel_scan.hpp"
#include "sonic/demo/logic/commands/oram.hpp"
#include "sonic/demo/logic/commands/omap.hpp"
#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/types/intents.hpp"

namespace sn::demo::logic {

inline types::command_result execute_command(const types::command_intent& intent, execution_context& ctx) {
  switch (intent.tag) {
  case types::command_tag::hello:
    return commands::hello::run(intent.hello, ctx);
  case types::command_tag::parallel_scan:
    return commands::parallel_scan::run(intent.parallel_scan, ctx);
  case types::command_tag::pathoram:
    return commands::oram::run_pathoram(intent.pathoram, ctx);
  case types::command_tag::zingoram:
    return commands::oram::run_zingoram(intent.zingoram, ctx);
  case types::command_tag::o2th:
    return commands::omap::run_o2th(intent.o2th, ctx);
  case types::command_tag::pmchain:
    return commands::omap::run_pmchain(intent.pmchain, ctx);
  default:
    return types::make_result(types::result_status::unsupported, "command not implemented");
  }
}

}
