#include "sonic/crypto/prf.hpp"

#include <array>
#include <cstring>
#include <limits>

#include "sonic/crypto/impl/openssl/prf_backend_selector.hpp"

namespace sn::crypto {

namespace {
constexpr std::size_t kIvDomainSize = 12;
constexpr std::size_t kCounterSize = sizeof(std::uint32_t);
constexpr std::uint64_t kMaxBlocksPerDerive = 1ULL << 32;

static_assert(kIvDomainSize + kCounterSize == prf::block_size, "IV layout must span full block");
}

prf::prf() = default;

prf::prf(const key_type& key) { set_key(key); }

prf::~prf() {
  detail::openssl_prf_cleanup(ctx_);
  keyed_ = false;
}

void prf::set_key(const key_type& key) {
  detail::openssl_prf_set_key(ctx_, sn::util::span<const std::uint8_t>(key.bytes.data(), key.bytes.size()));
  keyed_ = true;
}

prf::key_type prf::generate_key(prng& rng) {
  key_type key;
  rng.random_bytes(key.bytes.data(), key.bytes.size());
  return key;
}

void prf::derive(sn::util::span<const std::uint8_t> input, sn::util::span<std::uint8_t> output) {
  if (!keyed_) {
    throw error("prf key not set");
  }
  if (input.size() > max_input_size) {
    throw error("prf input too long");
  }
  if (output.empty()) {
    return;
  }

  const std::uint64_t blocks_needed = (static_cast<std::uint64_t>(output.size()) + block_size - 1) / block_size;
  if (blocks_needed > kMaxBlocksPerDerive) {
    throw error("prf output too long");
  }

  std::array<std::uint8_t, block_size> iv{};
  iv[0] = static_cast<std::uint8_t>(input.size());
  if (!input.empty()) {
    std::memcpy(iv.data() + 1, input.data(), input.size());
  }

  detail::openssl_prf_stream(ctx_, iv.data(), output.data(), output.size());

  detail::secure_zero(iv.data(), iv.size());
}

}
