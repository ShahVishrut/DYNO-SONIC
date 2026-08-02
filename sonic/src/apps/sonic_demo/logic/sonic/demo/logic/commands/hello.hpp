#pragma once

#include <string>

#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/types/intents.hpp"
#include "sonic/util/log.hpp"

namespace sn::demo::logic::commands::hello {

inline types::command_result run(const types::hello_intent& intent, execution_context& ctx) {
  const auto name = intent.name.view();
  const std::uint32_t repeat = intent.repeat == 0 ? 1u : intent.repeat;
  const bool enthusiastic = intent.enthusiastic != 0;

  std::string output;
  output.reserve(static_cast<std::size_t>(repeat) * (name.size() + 8));

  auto logger = sn::util::log::create("sonic_demo.hello");

  for (std::uint32_t i = 0; i < repeat; ++i) {
    std::string line{"hello, "};
    line.append(name);
    if (enthusiastic) {
      line.append("!!!");
    }
    logger.inf(line);
    if (!output.empty()) {
      output.push_back('\n');
    }
    output.append(line);
  }

  types::command_result result{};
  if (!result.output.assign(output)) {
    return types::make_result(types::result_status::internal_error, "hello output truncated");
  }

  result.status = types::result_status::ok;
  return result;
}

}
