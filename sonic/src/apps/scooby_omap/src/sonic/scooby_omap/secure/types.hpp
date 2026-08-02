#pragma once

#include "sonic/sgxbridge/secure/crypto_traits.hpp"

namespace sn::scooby::omap::secure {

enum class channel {
  client_to_lb,
  lb_to_client,
  lb_to_suboram,
  suboram_to_lb,
};

#if defined(SCOOBY_SECURE_COMM)

struct key_schedule {
  sn::sgxbridge::secure::aes_gcm_traits::key_type client_to_lb{};
  sn::sgxbridge::secure::aes_gcm_traits::key_type lb_to_client{};
  sn::sgxbridge::secure::aes_gcm_traits::key_type lb_to_suboram{};
  sn::sgxbridge::secure::aes_gcm_traits::key_type suboram_to_lb{};
};

#endif

}
