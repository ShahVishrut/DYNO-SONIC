#include "sonic/crypto/prng.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/impl/bearssl/detail/backend.hpp"
#include "sonic/crypto/impl/bearssl/detail/utils.hpp"
#include "sonic/util/memcpy.hpp"

namespace sn::crypto {

namespace {
constexpr std::size_t kMaxBatchBytes = detail::max_chunk_size;

std::size_t clamp_step(std::size_t remaining, std::size_t bytes_available_this_key) {
  return std::min<std::size_t>({remaining, bytes_available_this_key, kMaxBatchBytes});
}

}

prng::prng() {
  allow_auto_reseed_ = true;
  u64_cache_used_ = block_size;
  const auto seed = make_seed(seeder_);
  reseed(seed);
}

prng::prng(const seed_material& seed) {
  allow_auto_reseed_ = false;
  u64_cache_used_ = block_size;
  reseed(seed);
}

prng::~prng() {
  detail::secure_zero(seed_.key.data(), seed_.key.size());
  detail::secure_zero(seed_.iv.data(), seed_.iv.size());
  detail::secure_zero(ctx_.iv.data(), ctx_.iv.size());
  ctx_.counter = 0;
  detail::secure_zero(ctx_.keystream.data(), ctx_.keystream.size());
  ctx_.keystream_used = ctx_.keystream.size();
  detail::secure_zero(u64_cache_.data(), u64_cache_.size());
  detail::secure_zero(&ctx_.keys, sizeof(ctx_.keys));
}

void prng::reseed(const seed_material& seed) {
  seed_ = seed;
  detail::bearssl_ctr_init(&ctx_.keys, seed_.key.data(), seed_.key.size());
  sn::mem::copy_bytes(ctx_.iv.data(), seed_.iv.data(), ctx_.iv.size());
  ctx_.counter = detail::load_be32(seed_.iv.data() + ctx_.iv.size());
  blocks_generated_ = 0;
  u64_cache_used_ = block_size;
  detail::secure_zero(u64_cache_.data(), u64_cache_.size());
  ctx_.keystream_used = ctx_.keystream.size();
  detail::secure_zero(ctx_.keystream.data(), ctx_.keystream.size());
}

prng::seed_material prng::make_seed(random_device& rd) {
  seed_material seed;
  rd.fill(seed.key.data(), seed.key.size());
  rd.fill(seed.iv.data(), seed.iv.size());
  return seed;
}

prng::seed_material prng::make_seed() {
  random_device rd;
  return make_seed(rd);
}

void prng::random_bytes(sn::util::span<std::uint8_t> out) { random_bytes(out.data(), out.size()); }

void prng::random_bytes(std::uint8_t* dst, std::size_t len) {
  if (len == 0) {
    return;
  }

  if (!allow_auto_reseed_) {
    const std::uint64_t blocks_remaining = max_blocks_per_key - blocks_generated_;
    const std::uint64_t bytes_remaining = blocks_remaining * block_size;
    if (static_cast<std::uint64_t>(len) > bytes_remaining) {
      throw error("prng block budget exceeded for deterministic stream");
    }
  }

  std::size_t remaining = len;
  std::uint8_t* cursor = dst;

  while (remaining > 0) {
    if (ctx_.keystream_used < block_size) {
      const std::size_t available = block_size - ctx_.keystream_used;
      const std::size_t take = std::min<std::size_t>(remaining, available);
      sn::mem::copy_bytes(cursor, ctx_.keystream.data() + ctx_.keystream_used, take);
      ctx_.keystream_used += take;
      cursor += take;
      remaining -= take;
      continue;
    }

    if (allow_auto_reseed_ && blocks_generated_ >= max_blocks_per_key) {
      reseed(make_seed(seeder_));
      continue;
    } else if (!allow_auto_reseed_ && blocks_generated_ >= max_blocks_per_key) {
      throw error("prng block budget exceeded for deterministic stream");
    }

    const std::uint64_t blocks_available = max_blocks_per_key - blocks_generated_;
    const std::uint64_t raw_bytes_available = blocks_available * block_size;
    const std::size_t bytes_available_this_key = static_cast<std::size_t>(std::min<std::uint64_t>(
        raw_bytes_available, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
    ));

    if (bytes_available_this_key == 0) {
      if (allow_auto_reseed_) {
        reseed(make_seed(seeder_));
        continue;
      }
      throw error("prng block budget exhausted");
    }

    const std::size_t chunk = clamp_step(remaining, bytes_available_this_key);
    const std::size_t aligned = chunk & ~(block_size - 1);

    if (aligned >= block_size) {
      sn::mem::fill_bytes(cursor, 0, aligned);
      ctx_.counter = detail::bearssl_ctr_run(&ctx_.keys, ctx_.iv.data(), ctx_.counter, cursor, aligned);
      cursor += aligned;
      remaining -= aligned;
      blocks_generated_ += static_cast<std::uint64_t>(aligned / block_size);
      continue;
    }

    sn::mem::fill_bytes(ctx_.keystream.data(), 0, block_size);
    ctx_.counter = detail::bearssl_ctr_run(&ctx_.keys, ctx_.iv.data(), ctx_.counter, ctx_.keystream.data(), block_size);
    blocks_generated_ += 1;
    ctx_.keystream_used = 0;
  }
}

std::uint64_t prng::random_u64() {
  if (u64_cache_used_ + sizeof(std::uint64_t) <= u64_cache_.size()) {
    const auto value = detail::load_trivial<std::uint64_t>(u64_cache_.data() + u64_cache_used_);
    u64_cache_used_ += sizeof(std::uint64_t);
    return value;
  }

  random_bytes(u64_cache_.data(), u64_cache_.size());
  u64_cache_used_ = sizeof(std::uint64_t);
  return detail::load_trivial<std::uint64_t>(u64_cache_.data());
}

std::uint64_t prng::random_u64(std::uint64_t begin, std::uint64_t end) {
  return detail::sample_range_with_u64(begin, end, [this]() { return random_u64(); });
}

}
