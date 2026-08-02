#pragma once

#include "sonic/scooby_omap/suboram/state.hpp"

namespace sn::scooby::omap::suboram {

template <std::size_t PayloadBytes> inline void receiver(state<PayloadBytes>& s, sn::sgxbridge::tp::stop_token token) {
  const auto timeout = recv_timeout_ms(50);
  while (!s.stop_requested.load(std::memory_order_relaxed) && !token.stop_requested()) {
    sn::sgxbridge::dist::scoped_message scoped;
    if (!sn::sgxbridge::dist::recv_for(scoped, timeout)) {
      continue;
    }
    inbound_message msg;
    if (!decode_inbound_message(std::move(scoped), msg)) {
      s.ctx->logger.wrn("scooby-omap suboram dropped malformed message");
      continue;
    }
    if (header_kind(msg.header) != message_kind::lb_bin_dispatch) {
      s.ctx->logger.wrnf(
          "scooby-omap suboram[%u] ignoring unexpected message kind=%u", s.plan.role_index,
          static_cast<unsigned>(header_kind(msg.header))
      );
      continue;
    }
    ++s.metrics.messages_received;
    s.metrics.bytes_received += msg.bytes.size();
    msg.enqueued_at = sn::sgxbridge::time::steady_clock::now();
    s.inbox.push(std::move(msg));
  }
}

template <std::size_t PayloadBytes> inline bool start_receiver(state<PayloadBytes>& s) {
  constexpr const char* kLabel = "scooby-omap.suboram.receiver";
  auto request = sn::sgxbridge::tp::make_request(1, 0, sn::sgxbridge::tp::queue_policy::block_when_full, kLabel);
  return s.receiver.start(
      s.ctx->threadpools, request, s.ctx->logger, s.stop_requested, 1,
      [&](std::size_t, sn::sgxbridge::tp::stop_token token) { receiver(s, token); }
  );
}

template <std::size_t PayloadBytes> inline void stop_receiver(state<PayloadBytes>& s) {
  s.receiver.stop([&] { s.inbox.close(); });
}

}
