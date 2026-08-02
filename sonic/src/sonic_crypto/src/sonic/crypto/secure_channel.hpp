#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "sonic/crypto/cipher.hpp"
#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/crypto/error.hpp"
#include "sonic/crypto/hkdf.hpp"
#include "sonic/crypto/random.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

namespace detail_secure_channel {

inline constexpr std::array<std::uint8_t, 4> kHelloMagic{{'S', 'C', 'H', '1'}};
inline constexpr std::string_view kInfoPrefix{"sonic.secure_channel/v1"};
inline constexpr std::string_view kLabelKey0{"K/0"};
inline constexpr std::string_view kLabelKey1{"K/1"};
inline constexpr std::string_view kLabelNonce0{"N/0"};
inline constexpr std::string_view kLabelNonce1{"N/1"};
inline constexpr std::string_view kLabelCid{"CID"};

inline constexpr std::size_t kNoncePrefixSize = 4;
inline constexpr std::size_t kCidSize = 16;
inline constexpr std::size_t kSaltSize = 16;
inline constexpr std::size_t kHelloSize = 4 + kSaltSize;
inline constexpr std::size_t kLengthFieldSize = 4;
inline constexpr std::size_t kAadSize = kCidSize + sizeof(std::uint64_t) + kLengthFieldSize;

inline void store_be32(std::uint8_t* dst, std::uint32_t value) {
  dst[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
  dst[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  dst[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  dst[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline std::uint32_t load_be32(const std::uint8_t* src) {
  return (static_cast<std::uint32_t>(src[0]) << 24) | (static_cast<std::uint32_t>(src[1]) << 16) |
         (static_cast<std::uint32_t>(src[2]) << 8) | static_cast<std::uint32_t>(src[3]);
}

inline void store_be64(std::uint8_t* dst, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    dst[7 - i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
  }
}

inline int compare_bytes(sn::util::span<const std::uint8_t> lhs, sn::util::span<const std::uint8_t> rhs) {
  const std::size_t common = std::min(lhs.size(), rhs.size());
  if (common > 0) {
    const int cmp = std::memcmp(lhs.data(), rhs.data(), common);
    if (cmp != 0) {
      return cmp;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return lhs.size() < rhs.size() ? -1 : 1;
}

inline void prepare_frame(
    const std::array<std::uint8_t, kNoncePrefixSize>& prefix, const std::array<std::uint8_t, kCidSize>& cid,
    std::uint64_t sequence, const std::array<std::uint8_t, kLengthFieldSize>& length_be,
    gcm_cipher::nonce_type& nonce_out, std::array<std::uint8_t, kAadSize>& aad_out
) {
  std::memcpy(nonce_out.bytes.data(), prefix.data(), prefix.size());
  store_be64(nonce_out.bytes.data() + kNoncePrefixSize, sequence);

  std::memcpy(aad_out.data(), cid.data(), cid.size());
  store_be64(aad_out.data() + kCidSize, sequence);
  std::memcpy(aad_out.data() + kCidSize + sizeof(std::uint64_t), length_be.data(), length_be.size());
}

}

template <typename Transport> class secure_channel {
public:
  static constexpr std::size_t tag_size = gcm_cipher::tag_size;
  static constexpr std::size_t nonce_size = gcm_cipher::nonce_size;
  static constexpr std::size_t nonce_prefix_size = detail_secure_channel::kNoncePrefixSize;
  static constexpr std::size_t key_size = gcm_cipher::key_size;
  static constexpr std::size_t hello_size = detail_secure_channel::kHelloSize;
  static constexpr std::size_t salt_size = detail_secure_channel::kSaltSize;
  static constexpr std::size_t cid_size = detail_secure_channel::kCidSize;
  static constexpr std::size_t default_max_frame = 1u << 20;

  struct options {
    std::size_t max_frame_bytes = default_max_frame;
  };

  secure_channel(Transport& transport, sn::util::span<const std::uint8_t> psk, const options& opt = {}) :
      transport_(transport), max_frame_bytes_(opt.max_frame_bytes == 0 ? default_max_frame : opt.max_frame_bytes) {
    if (psk.size() < 16) {
      throw error("secure channel psk too short");
    }

    handshake_result handshake = perform_handshake();
    material_ = derive_schedule(handshake, psk);
    detail::secure_zero(handshake.salts.data(), handshake.salts.size());
    tx_buffer_.resize(max_frame_bytes_);
    rx_buffer_.resize(max_frame_bytes_);
    seq_tx_ = 0;
    seq_rx_ = 0;
  }

  void send(sn::util::span<const std::uint8_t> plaintext) {
    const std::size_t len = plaintext.size();
    if (len > max_frame_bytes_) {
      throw error("secure channel frame too large");
    }
    if (seq_tx_ == std::numeric_limits<std::uint64_t>::max()) {
      throw error("secure channel tx overflow");
    }

    const auto length_be = encode_length(len);
    write_all(length_be.data(), length_be.size());

    gcm_cipher::nonce_type nonce{};
    std::array<std::uint8_t, detail_secure_channel::kAadSize> aad{};
    detail_secure_channel::prepare_frame(
        material_.nonce_prefix_tx, material_.channel_id, seq_tx_, length_be, nonce, aad
    );

    gcm_cipher::tag_type tag{};
    cipher_.encrypt(
        material_.key_tx, nonce, sn::util::span<const std::uint8_t>(aad), plaintext,
        sn::util::span<std::uint8_t>(tx_buffer_.data(), len), tag
    );

    write_all(tx_buffer_.data(), len);
    write_all(tag.bytes.data(), tag.bytes.size());
    ++seq_tx_;
  }

  std::size_t recv(sn::util::span<std::uint8_t> out_buffer) {
    if (seq_rx_ == std::numeric_limits<std::uint64_t>::max()) {
      throw error("secure channel rx overflow");
    }

    std::array<std::uint8_t, detail_secure_channel::kLengthFieldSize> length_be{};
    read_exact(length_be.data(), length_be.size());
    const std::uint32_t len = detail_secure_channel::load_be32(length_be.data());

    if (len > max_frame_bytes_) {
      throw error("secure channel frame too large");
    }
    if (len > out_buffer.size()) {
      throw error("secure channel output buffer too small");
    }

    if (len != 0) {
      read_exact(rx_buffer_.data(), len);
    }

    gcm_cipher::tag_type tag{};
    read_exact(tag.bytes.data(), tag.bytes.size());

    gcm_cipher::nonce_type nonce{};
    std::array<std::uint8_t, detail_secure_channel::kAadSize> aad{};
    detail_secure_channel::prepare_frame(
        material_.nonce_prefix_rx, material_.channel_id, seq_rx_, length_be, nonce, aad
    );

    const bool ok = cipher_.decrypt(
        material_.key_rx, nonce, sn::util::span<const std::uint8_t>(aad),
        sn::util::span<const std::uint8_t>(rx_buffer_.data(), len), out_buffer.subspan(0, len), tag
    );

    if (!ok) {
      detail::secure_zero(out_buffer.data(), len);
      throw error("secure channel tag mismatch");
    }

    ++seq_rx_;
    return len;
  }

private:

  struct handshake_result {
    std::array<std::uint8_t, 2 * salt_size> salts{};
    bool local_is_first = false;
  };

  struct schedule_material {
    gcm_cipher::key_type key_tx{};
    gcm_cipher::key_type key_rx{};
    std::array<std::uint8_t, nonce_prefix_size> nonce_prefix_tx{};
    std::array<std::uint8_t, nonce_prefix_size> nonce_prefix_rx{};
    std::array<std::uint8_t, cid_size> channel_id{};
  };

  static std::array<std::uint8_t, detail_secure_channel::kLengthFieldSize> encode_length(std::size_t len) {
    if (len > std::numeric_limits<std::uint32_t>::max()) {
      throw error("secure channel length overflow");
    }
    std::array<std::uint8_t, detail_secure_channel::kLengthFieldSize> out{};
    detail_secure_channel::store_be32(out.data(), static_cast<std::uint32_t>(len));
    return out;
  }

  handshake_result perform_handshake() {
    handshake_result result{};

    std::array<std::uint8_t, hello_size> local_hello{};
    std::memcpy(
        local_hello.data(), detail_secure_channel::kHelloMagic.data(), detail_secure_channel::kHelloMagic.size()
    );
    random_device rng;
    rng.fill(sn::util::span<std::uint8_t>(local_hello.data() + detail_secure_channel::kHelloMagic.size(), salt_size));
    write_all(local_hello.data(), local_hello.size());

    std::array<std::uint8_t, hello_size> peer_hello{};
    read_exact(peer_hello.data(), peer_hello.size());
    if (std::memcmp(
            peer_hello.data(), detail_secure_channel::kHelloMagic.data(), detail_secure_channel::kHelloMagic.size()
        ) != 0) {
      throw error("secure channel bad hello");
    }

    const sn::util::span<const std::uint8_t> local_salt(
        local_hello.data() + detail_secure_channel::kHelloMagic.size(), salt_size
    );
    const sn::util::span<const std::uint8_t> peer_salt(
        peer_hello.data() + detail_secure_channel::kHelloMagic.size(), salt_size
    );

    const int cmp = detail_secure_channel::compare_bytes(local_salt, peer_salt);
    if (cmp <= 0) {
      std::memcpy(result.salts.data(), local_salt.data(), salt_size);
      std::memcpy(result.salts.data() + salt_size, peer_salt.data(), salt_size);
      result.local_is_first = true;
    } else {
      std::memcpy(result.salts.data(), peer_salt.data(), salt_size);
      std::memcpy(result.salts.data() + salt_size, local_salt.data(), salt_size);
      result.local_is_first = false;
    }

    detail::secure_zero(local_hello.data(), local_hello.size());
    detail::secure_zero(peer_hello.data(), peer_hello.size());
    return result;
  }

  schedule_material derive_schedule(const handshake_result& handshake, sn::util::span<const std::uint8_t> psk) {
    schedule_material schedule{};

    std::array<std::uint8_t, hkdf_sha256::hash_len> prk{};
    hkdf_sha256::extract(sn::util::span<const std::uint8_t>(handshake.salts), psk, sn::util::span<std::uint8_t>(prk));

    std::array<std::uint8_t, detail_secure_channel::kInfoPrefix.size() + 2 * salt_size> base_info{};
    std::memcpy(base_info.data(), detail_secure_channel::kInfoPrefix.data(), detail_secure_channel::kInfoPrefix.size());
    std::memcpy(
        base_info.data() + detail_secure_channel::kInfoPrefix.size(), handshake.salts.data(), handshake.salts.size()
    );

    std::array<std::uint8_t, base_info.size() + 4> info_buffer{};
    auto expand = [&](std::string_view label, sn::util::span<std::uint8_t> out) {
      std::memcpy(info_buffer.data(), base_info.data(), base_info.size());
      std::memcpy(info_buffer.data() + base_info.size(), label.data(), label.size());
      hkdf_sha256::expand(
          sn::util::span<const std::uint8_t>(prk),
          sn::util::span<const std::uint8_t>(info_buffer.data(), base_info.size() + label.size()), out
      );
    };

    std::array<std::uint8_t, key_size> key0{};
    std::array<std::uint8_t, key_size> key1{};
    std::array<std::uint8_t, nonce_prefix_size> nonce0{};
    std::array<std::uint8_t, nonce_prefix_size> nonce1{};

    expand(detail_secure_channel::kLabelKey0, sn::util::span<std::uint8_t>(key0));
    expand(detail_secure_channel::kLabelKey1, sn::util::span<std::uint8_t>(key1));
    expand(detail_secure_channel::kLabelNonce0, sn::util::span<std::uint8_t>(nonce0));
    expand(detail_secure_channel::kLabelNonce1, sn::util::span<std::uint8_t>(nonce1));
    expand(detail_secure_channel::kLabelCid, sn::util::span<std::uint8_t>(schedule.channel_id));

    if (handshake.local_is_first) {
      std::memcpy(schedule.key_tx.bytes.data(), key0.data(), key0.size());
      std::memcpy(schedule.key_rx.bytes.data(), key1.data(), key1.size());
      schedule.nonce_prefix_tx = nonce0;
      schedule.nonce_prefix_rx = nonce1;
    } else {
      std::memcpy(schedule.key_tx.bytes.data(), key1.data(), key1.size());
      std::memcpy(schedule.key_rx.bytes.data(), key0.data(), key0.size());
      schedule.nonce_prefix_tx = nonce1;
      schedule.nonce_prefix_rx = nonce0;
    }

    detail::secure_zero(key0.data(), key0.size());
    detail::secure_zero(key1.data(), key1.size());
    detail::secure_zero(nonce0.data(), nonce0.size());
    detail::secure_zero(nonce1.data(), nonce1.size());
    detail::secure_zero(prk.data(), prk.size());
    detail::secure_zero(base_info.data(), base_info.size());
    detail::secure_zero(info_buffer.data(), info_buffer.size());
    return schedule;
  }

  void write_all(const std::uint8_t* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
      const std::size_t n = transport_.write(data + written, len - written);
      if (n == 0) {
        throw error("secure channel write failure");
      }
      written += n;
    }
  }

  void read_exact(std::uint8_t* data, std::size_t len) {
    std::size_t read = 0;
    while (read < len) {
      const std::size_t n = transport_.read(data + read, len - read);
      if (n == 0) {
        throw error("secure channel read failure");
      }
      read += n;
    }
  }

private:
  Transport& transport_;
  std::size_t max_frame_bytes_;
  gcm_cipher cipher_{};

  schedule_material material_{};
  std::uint64_t seq_tx_ = 0;
  std::uint64_t seq_rx_ = 0;

  std::vector<std::uint8_t> tx_buffer_;
  std::vector<std::uint8_t> rx_buffer_;
};

}
