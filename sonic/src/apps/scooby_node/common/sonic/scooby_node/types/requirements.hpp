#pragma once

#include "sonic/scooby_node/types/intents.hpp"

namespace sn::scooby::types {

inline bool requires_transport(command_tag tag) {
  switch (tag) {
  case command_tag::scooby_omap:
    return true;
  case command_tag::none:
  default:
    return false;
  }
}

inline bool requires_transport(const command_intent& intent) { return requires_transport(intent.tag); }

}
