#include "sonic/crypto/prf.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/impl/bearssl/detail/utils.hpp"
#include "sonic/crypto/impl/bearssl/prf_backend.hpp"
#include "sonic/util/memcpy.hpp"

namespace sn::crypto {

namespace {
constexpr std::size_t kIvDomainSize = 12;
constexpr std::uint64_t kMaxBlocksPerDerive = 1ULL << 32;

static_assert(kIvDomainSize + sizeof(std::uint32_t) == prf::block_size, "IV layout must span full block");
}

prf::prf() = default;

prf::prf(const key_type& key) { set_key(key); }

prf::~prf() {
  detail::bearssl_prf_cleanup(ctx_);
  keyed_ = false;
}

void prf::set_key(const key_type& key) {
  detail::bearssl_prf_set_key(ctx_, sn::util::span<const std::uint8_t>(key.bytes.data(), key.bytes.size()));
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
  if (output.size() == 0) {
    return;
  }

  const std::uint64_t blocks_needed = (static_cast<std::uint64_t>(output.size()) + block_size - 1) / block_size;
  if (blocks_needed > kMaxBlocksPerDerive) {
    throw error("prf output too long");
  }

  std::array<std::uint8_t, block_size> iv{};
  iv[0] = static_cast<std::uint8_t>(input.size());
  if (!input.empty()) {
    sn::mem::copy_bytes(iv.data() + 1, input.data(), input.size());
  }

  detail::bearssl_prf_stream(ctx_, iv.data(), output.data(), output.size());

  detail::secure_zero(iv.data(), iv.size());
}

}
