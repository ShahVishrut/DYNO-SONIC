#include "sonic/crypto/hash.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#include <openssl/evp.h>

namespace sn::crypto {

blake2b_hasher::blake2b_hasher(std::size_t digest_size) : digest_size_(digest_size) {
  if (digest_size == 0 || digest_size > max_digest_size) {
    throw error("invalid blake2b digest size");
  }

  detail::check_md(EVP_MD_CTX_reset(ctx_.get()), "EVP_MD_CTX_reset");
  detail::check_md(EVP_DigestInit_ex(ctx_.get(), EVP_blake2b512(), nullptr), "EVP_DigestInit_ex");
}

void blake2b_hasher::update(sn::util::span<const std::uint8_t> data) {
  if (finalized_) {
    throw error("blake2b finalize already called");
  }
  if (data.size() == 0) {
    return;
  }
  detail::check_md(EVP_DigestUpdate(ctx_.get(), data.data(), data.size()), "EVP_DigestUpdate");
}

void blake2b_hasher::finalize(sn::util::span<std::uint8_t> out) {
  if (finalized_) {
    throw error("blake2b finalize already called");
  }
  if (out.size() != digest_size_) {
    throw error("blake2b output size mismatch");
  }

  std::array<std::uint8_t, max_digest_size> buffer{};
  unsigned int actual = 0;
  detail::check_md(EVP_DigestFinal_ex(ctx_.get(), buffer.data(), &actual), "EVP_DigestFinal_ex");
  if (actual != max_digest_size) {
    throw error("blake2b short digest");
  }
  std::memcpy(out.data(), buffer.data(), digest_size_);
  detail::secure_zero(buffer.data(), buffer.size());

  finalized_ = true;
}

void blake2b_hasher::hash(sn::util::span<const std::uint8_t> data, sn::util::span<std::uint8_t> out) {
  blake2b_hasher hasher(out.size());
  hasher.update(data);
  hasher.finalize(out);
}

}
