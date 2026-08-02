#pragma once

#include <string>
#include <string_view>

#include "sonic/threads/tuning.hpp"
#include "sonic/util/ext/args.hpp"

namespace sn::util::cli {

namespace detail {

inline std::optional<sn::threads::thread_affinity> parse_affinity(std::string_view text) {
  if (text == "inherit") {
    return sn::threads::thread_affinity::inherit;
  }
  if (text == "dedicated") {
    return sn::threads::thread_affinity::dedicated;
  }
  return std::nullopt;
}

inline std::optional<sn::threads::thread_smt> parse_smt(std::string_view text) {
  if (text == "avoid") {
    return sn::threads::thread_smt::avoid;
  }
  if (text == "allow") {
    return sn::threads::thread_smt::allow;
  }
  return std::nullopt;
}

}

class thread_option_flags {
public:
  template <typename Parent>
  explicit thread_option_flags(Parent& parent, sn::threads::thread_policy defaults = {}) :
      affinity_(
          parent, "mode", "thread affinity", {"thread-affinity"}, std::string(sn::threads::describe(defaults.affinity))
      ),
      smt_(parent, "mode", "thread smt", {"thread-smt"}, std::string(sn::threads::describe(defaults.smt))) {}

  [[nodiscard]] sn::threads::thread_policy resolve() {
    const auto affinity = detail::parse_affinity(args::get(affinity_));
    if (!affinity.has_value()) {
      throw args::Error("unknown thread-affinity");
    }
    const auto smt = detail::parse_smt(args::get(smt_));
    if (!smt.has_value()) {
      throw args::Error("unknown thread-smt");
    }
    return sn::threads::thread_policy{.affinity = affinity.value(), .smt = smt.value()};
  }

private:
  args::ValueFlag<std::string> affinity_;
  args::ValueFlag<std::string> smt_;
};

}
