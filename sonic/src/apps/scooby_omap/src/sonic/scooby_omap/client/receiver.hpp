#pragma once

#include "sonic/sgxbridge/dist/api.hpp"

#include "sonic/scooby_omap/client/state.hpp"

namespace sn::scooby::omap::client {

template <std::size_t PayloadBytes> inline void receiver(state<PayloadBytes>& s, sn::sgxbridge::tp::stop_token token) {
  const auto timeout = recv_timeout_ms(50);
  while (!s.stop_requested.load(std::memory_order_relaxed) && !token.stop_requested()) {
    sn::sgxbridge::dist::scoped_message scoped;
    if (!sn::sgxbridge::dist::recv_for(scoped, timeout)) {
      continue;
    }
    inbound_message msg;
    sn::util::log::ensuref(
        decode_inbound_message(std::move(scoped), msg), "scooby-omap client failed to decode message"
    );
    if (header_kind(msg.header) != message_kind::lb_batch_response) {
      s.ctx->logger.wrn("scooby message");
      continue;
    }
    ++s.metrics.messages_received;
    s.metrics.bytes_received += msg.bytes.size();
    msg.enqueued_at = sn::sgxbridge::time::steady_clock::now();
    s.inbox.push(std::move(msg));
  }
}

template <std::size_t PayloadBytes> inline bool start_receiver(state<PayloadBytes>& s) {
  constexpr const char* kLabel = "scooby-omap.client.receiver";
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
