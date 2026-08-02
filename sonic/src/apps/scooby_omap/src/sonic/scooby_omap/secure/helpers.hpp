#pragma once

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/secure/session.hpp"
#include "sonic/scooby_omap/secure/types.hpp"
#include "sonic/util/log.hpp"

namespace sn::scooby::omap::secure {

#if defined(SCOOBY_SECURE_COMM)

inline const sn::sgxbridge::secure::aes_gcm_traits::key_type& select_channel_key(
    const plan_config& plan, channel direction
) {
  switch (direction) {
  case channel::client_to_lb:
    return plan.secure_keys.client_to_lb;
  case channel::lb_to_client:
    return plan.secure_keys.lb_to_client;
  case channel::lb_to_suboram:
    return plan.secure_keys.lb_to_suboram;
  case channel::suboram_to_lb:
    return plan.secure_keys.suboram_to_lb;
  }
  sn::util::log::ensuref(false, "secure::select_channel_key received invalid channel");
  return plan.secure_keys.client_to_lb;
}

#endif

inline void configure_session(
    secure_session_t& session, const plan_config& plan, channel direction, std::size_t payload_bytes
) {
#if defined(SCOOBY_SECURE_COMM)
  const auto& key = select_channel_key(plan, direction);
  session.configure(key, payload_bytes);
#else
  (void) plan;
  secure_session_t::key_type key{};
  session.configure(key, payload_bytes);
#endif
}

inline void configure_client_sessions(
    const plan_config& plan, std::size_t payload_bytes, secure_session_t& tx_session, secure_session_t& rx_session
) {
  configure_session(tx_session, plan, channel::client_to_lb, payload_bytes);
  configure_session(rx_session, plan, channel::lb_to_client, payload_bytes);
}

inline void configure_load_balancer_sessions(
    const plan_config& plan, std::size_t batch_payload_bytes, std::size_t bin_payload_bytes,
    secure_session_t& client_rx, secure_session_t& client_tx, secure_session_t& bin_rx, secure_session_t& bin_tx
) {
  configure_session(client_rx, plan, channel::client_to_lb, batch_payload_bytes);
  configure_session(client_tx, plan, channel::lb_to_client, batch_payload_bytes);
  configure_session(bin_rx, plan, channel::suboram_to_lb, bin_payload_bytes);
  configure_session(bin_tx, plan, channel::lb_to_suboram, bin_payload_bytes);
}

inline void configure_suboram_sessions(
    const plan_config& plan, std::size_t bin_payload_bytes, secure_session_t& bin_rx, secure_session_t& response_tx
) {
  configure_session(bin_rx, plan, channel::lb_to_suboram, bin_payload_bytes);
  configure_session(response_tx, plan, channel::suboram_to_lb, bin_payload_bytes);
}

}
