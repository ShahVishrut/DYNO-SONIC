#pragma once

#include <cstddef>

#include "sonic/sgxbridge/secure/crypto_traits.hpp"
#include "sonic/sgxbridge/secure/session.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::sgxbridge::secure {

template <typename Traits> class message_builder {
public:
  using session_type = session<Traits>;

  message_builder(session_type& sess, sn::util::span<std::uint8_t> host_buffer) :
      session_(sess), host_buffer_(host_buffer) {}

  sn::util::span<std::uint8_t> header_buffer(std::size_t size) {
    header_len_ = size;
    sn::util::log::ensuref(host_buffer_.size() >= size, "secure header", size, host_buffer_.size());
    return host_buffer_.subspan(0, size);
  }

  sn::util::span<std::uint8_t> payload_buffer(std::size_t size) {
    payload_len_ = size;
    if constexpr (Traits::enabled) {
      scratch_view_ = session_.acquire_scratch(size);
      return scratch_view_;
    } else {
      sn::util::log::ensuref(host_buffer_.size() >= header_len_ + size, "secure payload", header_len_, size, host_buffer_.size());
      return host_buffer_.subspan(header_len_, size);
    }
  }

  void finalize() {
    if constexpr (Traits::enabled) {
      const std::size_t total_required = header_len_ + payload_len_ + Traits::overhead;
      sn::util::log::ensuref(host_buffer_.size() >= total_required, "secure finalize", total_required, host_buffer_.size());
      auto aad = host_buffer_.subspan(0, header_len_);
      auto cipher_target = host_buffer_.subspan(header_len_, payload_len_ + Traits::overhead);
      Traits::encrypt(session_.key(), aad, scratch_view_, cipher_target, session_.rng());
    }
  }

  [[nodiscard]] std::size_t total_wire_bytes() const noexcept {
    return header_len_ + payload_len_ + (Traits::enabled ? Traits::overhead : 0);
  }

  static bool decrypt_into(
      session_type& sess, sn::util::span<const std::uint8_t> wire_bytes, std::size_t header_bytes,
      sn::util::span<const std::uint8_t>& header_view, sn::util::span<const std::uint8_t>& payload_view
  ) {
    if (wire_bytes.size() < header_bytes) {
      return false;
    }
    header_view = wire_bytes.subspan(0, header_bytes);
    auto payload_cipher = wire_bytes.subspan(header_bytes, wire_bytes.size() - header_bytes);
    if constexpr (Traits::enabled) {
      if (payload_cipher.size() < Traits::overhead) {
        return false;
      }
      const std::size_t plain_bytes = payload_cipher.size() - Traits::overhead;
      auto scratch = sess.acquire_scratch(plain_bytes);
      auto aad = header_view;
      auto cipher_view = payload_cipher;
      if (!Traits::decrypt(sess.key(), aad, cipher_view, scratch)) {
        return false;
      }
      payload_view = scratch;
    } else {
      payload_view = payload_cipher;
    }
    return true;
  }

private:
  session_type& session_;
  sn::util::span<std::uint8_t> host_buffer_;
  sn::util::span<std::uint8_t> scratch_view_{};
  std::size_t header_len_{0};
  std::size_t payload_len_{0};
};

}
