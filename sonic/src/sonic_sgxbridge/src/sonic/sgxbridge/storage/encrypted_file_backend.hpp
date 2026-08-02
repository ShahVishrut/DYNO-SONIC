#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "sonic/crypto/cipher.hpp"
#include "sonic/sgxbridge/enclave/host_buffer_portal.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/storage/io/backend.hpp"
#include "sonic/threads/sync.hpp"

namespace sn::sgxbridge::storage {

class encrypted_file_backend : public sn::storage::io::backend {
public:
  using page_view = sn::storage::io::backend::page_view;

  struct config {
    std::string data_path;
    std::string meta_path;
    bool create{true};
    bool truncate{true};
  };

  explicit encrypted_file_backend(config cfg);
  ~encrypted_file_backend() override;

  encrypted_file_backend(const encrypted_file_backend&) = delete;
  encrypted_file_backend& operator=(const encrypted_file_backend&) = delete;

  void read_page(std::uint64_t page_id, void* dst, std::size_t bytes) override;
  void write_page(std::uint64_t page_id, const void* src, std::size_t bytes) override;
  void write_pages(sn::util::span<const sn::storage::io::backend::page_view> pages) override;
  void flush() override;
  void resize(std::uint64_t pages, std::size_t bytes_per_page) override;

private:
  static constexpr std::size_t kNonceSize = sn::crypto::ctr_cipher::nonce_size;

  void write_pages_impl(sn::util::span<const page_view> pages);
  struct thread_state {
    static constexpr std::uint64_t kMagic = 0x4556425f53475854ULL;
    std::uint64_t magic{kMagic};
    sn::crypto::prng rng{};
    sn::sgxbridge::enclave::hostbuf::buffer host_buffer{};
    std::size_t host_capacity{0};
    std::vector<std::uint8_t> cipher_buf{};
    std::vector<std::uint64_t> page_ids{};
    std::array<std::uint8_t, kNonceSize> nonce_buf{};
  };

  void ensure_open();

  void read_nonce(std::uint64_t page_id, thread_state& tls_state);
  void write_nonce(std::uint64_t page_id, thread_state& tls_state);
  void read_cipher(std::uint64_t page_id, std::size_t bytes, thread_state& tls_state);
  void write_cipher(std::uint64_t page_id, std::size_t bytes, thread_state& tls_state);

  bool nonce_is_zero(const std::array<std::uint8_t, kNonceSize>& nonce) const noexcept;
  void regenerate_nonce(thread_state& tls_state);

  config cfg_{};
  std::uint64_t handle_{0};
  sn::crypto::ctr_cipher::key_type key_{};
  sn::crypto::ctr_cipher cipher_{};
  std::size_t page_bytes_{0};

  thread_state& tls(std::size_t host_bytes, std::size_t cipher_bytes);
};

}
