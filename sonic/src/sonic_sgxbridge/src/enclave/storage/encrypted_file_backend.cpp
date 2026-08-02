#include "sonic/sgxbridge/storage/encrypted_file_backend.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#include <sgx_error.h>
#include <sgx_trts.h>
#include <sgx_trts.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"

namespace {

enum class storage_status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  not_found = 2,
  io_error = 3,
};

storage_status decode_status(std::uint32_t raw) { return static_cast<storage_status>(raw); }

sn::sgxbridge::common::enclave_execution_context& require_context() {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  sn::util::log::ensure(ctx != nullptr && ctx->host_cookie != nullptr, "encrypted_file_backend requires host context");
  return *ctx;
}

void ensure_success(const char* what, sgx_status_t sgx_status, storage_status host_status) {
  sn::util::log::ensuref(sgx_status == SGX_SUCCESS, "%s sgx status=0x%x", what, static_cast<unsigned>(sgx_status));
}

void ensure_success(const char* what, sgx_status_t sgx_status, sgx_status_t ocall_status, storage_status host_status) {
  ensure_success(what, sgx_status, host_status);
  sn::util::log::ensuref(
      ocall_status == SGX_SUCCESS, "%s ocall status=0x%x", what, static_cast<unsigned>(ocall_status)
  );
  sn::util::log::ensuref(
      host_status == storage_status::ok, "%s host status=%u", what, static_cast<unsigned>(host_status)
  );
}

}

namespace sn::sgxbridge::storage {

encrypted_file_backend::encrypted_file_backend(config cfg) : cfg_(std::move(cfg)) {
  sn::util::log::ensure(!cfg_.data_path.empty(), "encrypted_file_backend: data path must be provided");
  if (cfg_.meta_path.empty()) {
    cfg_.meta_path = cfg_.data_path + ".meta";
  }

  sn::crypto::prng rng{};
  key_ = sn::crypto::ctr_cipher::generate_key(rng);

  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_open(
      &ocall_status, ctx.host_cookie, cfg_.data_path.c_str(), cfg_.meta_path.c_str(), cfg_.create ? 1u : 0u,
      cfg_.truncate ? 1u : 0u, &handle_, &host_status
  );
  ensure_success("storage_open", status, ocall_status, decode_status(host_status));
}

encrypted_file_backend::~encrypted_file_backend() {
  if (handle_ == 0) {
    return;
  }
  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  (void) sonic_sgxbridge_storage_flush(&ocall_status, ctx.host_cookie, handle_, &host_status);
  (void) sonic_sgxbridge_storage_close(&ocall_status, ctx.host_cookie, handle_, &host_status);
  handle_ = 0;
}

void encrypted_file_backend::resize(std::uint64_t pages, std::size_t bytes_per_page) {
  ensure_open();
  page_bytes_ = bytes_per_page;
  sn::util::log::ensure(page_bytes_ > 0, "encrypted backend");
  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_resize(
      &ocall_status, ctx.host_cookie, handle_, pages, bytes_per_page, kNonceSize, &host_status
  );
  ensure_success("storage_resize", status, ocall_status, decode_status(host_status));
}

void encrypted_file_backend::read_page(std::uint64_t page_id, void* dst, std::size_t bytes) {
  ensure_open();
  sn::util::log::ensure(dst != nullptr, "encrypted_file_backend: dst must not be null");
  if (page_bytes_ == 0) {
    page_bytes_ = bytes;
  }
  sn::util::log::ensure(page_bytes_ == bytes, "encrypted_file_backend: page size mismatch");

  auto& t = tls(std::max<std::size_t>(bytes, kNonceSize), bytes);

  read_nonce(page_id, t);
  if (nonce_is_zero(t.nonce_buf)) {
    std::memset(dst, 0, bytes);
    return;
  }

  read_cipher(page_id, bytes, t);

  sn::crypto::ctr_cipher::nonce_type nonce{};
  std::memcpy(nonce.bytes.data(), t.nonce_buf.data(), kNonceSize);
  auto cipher_view = sn::util::span<const std::uint8_t>(t.cipher_buf.data(), bytes);
  auto plain_view = sn::util::span<std::uint8_t>(static_cast<std::uint8_t*>(dst), bytes);
  cipher_.decrypt(key_, nonce, cipher_view, plain_view);
}

void encrypted_file_backend::write_page(std::uint64_t page_id, const void* src, std::size_t bytes) {
  const page_view v{page_id, src, bytes};
  write_pages_impl(sn::util::span<const page_view>(&v, 1));
}

void encrypted_file_backend::write_pages(sn::util::span<const sn::storage::io::backend::page_view> pages) {
  write_pages_impl(pages);
}

void encrypted_file_backend::write_pages_impl(sn::util::span<const page_view> pages) {
  if (pages.empty()) {
    return;
  }
  ensure_open();
  std::size_t max_bytes = 0;
  for (const auto& p : pages) {
    sn::util::log::ensure(p.src != nullptr, "encrypted_file_backend: src must not be null");
    max_bytes = std::max(max_bytes, p.bytes);
    if (page_bytes_ == 0) {
      page_bytes_ = p.bytes;
    }
    sn::util::log::ensure(page_bytes_ == p.bytes, "encrypted_file_backend: page size mismatch");
  }

  const std::size_t page_count = pages.size();
  sn::util::log::ensuref(
      page_count <= std::numeric_limits<std::size_t>::max() / page_bytes_, "encrypted_file_backend: batch size overflow"
  );
  const std::size_t data_bytes_total = page_bytes_ * page_count;
  const std::size_t meta_bytes_total = kNonceSize * page_count;
  const std::size_t host_bytes = data_bytes_total + meta_bytes_total;

  auto& t =
      tls(std::max<std::size_t>(host_bytes, std::max<std::size_t>(max_bytes, kNonceSize)),
          std::max<std::size_t>(max_bytes, kNonceSize));
  if (t.page_ids.size() < page_count) {
    t.page_ids.resize(page_count);
  }

  auto* cipher_base = t.host_buffer.data();
  auto* meta_base = cipher_base + data_bytes_total;
  for (std::size_t idx = 0; idx < page_count; ++idx) {
    const auto& p = pages[idx];
    t.page_ids[idx] = p.page_id;

    regenerate_nonce(t);
    std::memcpy(meta_base + idx * kNonceSize, t.nonce_buf.data(), kNonceSize);

    sn::crypto::ctr_cipher::nonce_type nonce{};
    std::memcpy(nonce.bytes.data(), t.nonce_buf.data(), kNonceSize);

    auto plain_view = sn::util::span<const std::uint8_t>(static_cast<const std::uint8_t*>(p.src), p.bytes);
    auto cipher_view = sn::util::span<std::uint8_t>(cipher_base + idx * page_bytes_, page_bytes_);
    cipher_.encrypt(key_, nonce, plain_view, cipher_view);
  }

  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_write_pages(
      &ocall_status, ctx.host_cookie, handle_, page_count, t.page_ids.data(), cipher_base, meta_base, page_bytes_,
      kNonceSize, &host_status
  );
  ensure_success("storage_write_pages", status, ocall_status, decode_status(host_status));
}

void encrypted_file_backend::flush() {
  ensure_open();
  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_flush(&ocall_status, ctx.host_cookie, handle_, &host_status);
  ensure_success("storage_flush", status, ocall_status, decode_status(host_status));
}

encrypted_file_backend::thread_state& encrypted_file_backend::tls(std::size_t host_bytes, std::size_t cipher_bytes) {
  static sn::threads::mutex map_mtx{};
  static std::unordered_map<std::uint64_t, std::unique_ptr<thread_state>> states;

  const std::uint64_t tid = sn::sgxbridge::enclave::current_thread_uid();

  thread_local thread_state* cached_state = nullptr;
  thread_local std::uint64_t cached_tid = 0;
  if (cached_tid == tid && cached_state != nullptr) {
    auto& ref = *cached_state;
    if (ref.host_capacity >= host_bytes && ref.cipher_buf.size() >= cipher_bytes) {
      return ref;
    }
  }

  thread_state* state = nullptr;
  {
    sn::threads::lock_guard guard(map_mtx);
    auto it = states.find(tid);
    if (it == states.end()) {
      it = states.emplace(tid, std::make_unique<thread_state>()).first;
    }
    state = it->second.get();
  }
  cached_tid = tid;
  cached_state = state;

  auto& ref = *state;
  if (ref.host_capacity < host_bytes) {
    ref.host_buffer.reset();
    ref.host_buffer = sn::sgxbridge::enclave::hostbuf::buffer::allocate(host_bytes);
    ref.host_capacity = ref.host_buffer.size();
  }
  if (ref.cipher_buf.size() < cipher_bytes) {
    ref.cipher_buf.resize(cipher_bytes);
  }
  return ref;
}

void encrypted_file_backend::ensure_open() {
  sn::util::log::ensure(handle_ != 0, "encrypted_file_backend: backend not open");
}

void encrypted_file_backend::read_nonce(std::uint64_t page_id, thread_state& tls_state) {
  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_read_meta(
      &ocall_status, ctx.host_cookie, handle_, page_id, tls_state.host_buffer.data(), kNonceSize, &host_status
  );
  ensure_success("storage_read_meta", status, ocall_status, decode_status(host_status));
  std::memcpy(tls_state.nonce_buf.data(), tls_state.host_buffer.data(), kNonceSize);
}

void encrypted_file_backend::write_nonce(std::uint64_t page_id, thread_state& tls_state) {
  auto& ctx = require_context();
  std::memcpy(tls_state.host_buffer.data(), tls_state.nonce_buf.data(), kNonceSize);
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_write_meta(
      &ocall_status, ctx.host_cookie, handle_, page_id, tls_state.host_buffer.data(), kNonceSize, &host_status
  );
  ensure_success("storage_write_meta", status, ocall_status, decode_status(host_status));
}

void encrypted_file_backend::read_cipher(std::uint64_t page_id, std::size_t bytes, thread_state& tls_state) {
  auto& ctx = require_context();
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_read_data(
      &ocall_status, ctx.host_cookie, handle_, page_id, tls_state.host_buffer.data(), bytes, &host_status
  );
  ensure_success("storage_read_data", status, ocall_status, decode_status(host_status));
  std::memcpy(tls_state.cipher_buf.data(), tls_state.host_buffer.data(), bytes);
}

void encrypted_file_backend::write_cipher(std::uint64_t page_id, std::size_t bytes, thread_state& tls_state) {
  auto& ctx = require_context();
  std::memcpy(tls_state.host_buffer.data(), tls_state.cipher_buf.data(), bytes);
  std::uint32_t host_status = static_cast<std::uint32_t>(storage_status::invalid_arguments);
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  sgx_status_t status = sonic_sgxbridge_storage_write_data(
      &ocall_status, ctx.host_cookie, handle_, page_id, tls_state.host_buffer.data(), bytes, &host_status
  );
  ensure_success("storage_write_data", status, ocall_status, decode_status(host_status));
}

bool encrypted_file_backend::nonce_is_zero(const std::array<std::uint8_t, kNonceSize>& nonce) const noexcept {
  return std::all_of(nonce.begin(), nonce.end(), [](std::uint8_t b) { return b == 0; });
}

void encrypted_file_backend::regenerate_nonce(thread_state& tls_state) {
  for (;;) {
    auto nonce = sn::crypto::ctr_cipher::generate_nonce(tls_state.rng);
    std::memcpy(tls_state.nonce_buf.data(), nonce.bytes.data(), kNonceSize);
    if (!nonce_is_zero(tls_state.nonce_buf)) {
      return;
    }
  }
}

}
