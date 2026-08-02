#pragma once

#include <cstdint>

#include "sonic/demo/types/intents.hpp"
#include "sonic/threads/tuning.hpp"

namespace sn::demo::cli {

struct parse_result {
  bool success{false};
  bool show_help{false};
  std::uint32_t verbosity{0};
  sn::threads::thread_policy thread_policy{};
  types::command_intent intent{};
};

parse_result parse_command_line(int argc, const char** argv);
void apply_logging_preferences(const parse_result& result);

}
