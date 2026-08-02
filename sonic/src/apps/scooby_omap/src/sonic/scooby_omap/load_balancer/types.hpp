#pragma once

#include <cstddef>

#include "sonic/omap/lbrouter/types.hpp"

#include "sonic/scooby_omap/config/types.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes> using router_types = sn::omap::lbrouter::router_types<key_type, PayloadBytes>;

}
