#pragma once

#include <cstdint>

#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/util/log.hpp"

namespace sn::scooby::types {

enum class execution_domain : std::uint32_t {
  native = 0,
  sgx_host = 1,
  sgx_enclave = 2,
};

struct execution_context {
  execution_domain domain{execution_domain::native};
  sn::sgxbridge::tp::provider threadpools{};
  sn::util::log::logger logger{sn::util::log::create("scooby_node")};
  std::uint32_t verbosity{0};
  bool transport_enabled{false};
};

}
