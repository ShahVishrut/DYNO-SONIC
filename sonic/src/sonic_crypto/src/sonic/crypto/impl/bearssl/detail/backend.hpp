#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <bearssl_aead.h>
#include <bearssl_block.h>
#include <bearssl_hash.h>

namespace sn::crypto::detail {

#if defined(SN_BEARSSL_USE_X86NI)
using bearssl_ctr_keys = br_aes_x86ni_ctr_keys;

inline void bearssl_ctr_init(bearssl_ctr_keys* ctx, const void* key, std::size_t len) {
  br_aes_x86ni_ctr_init(ctx, key, len);
}

inline std::uint32_t bearssl_ctr_run(
    const bearssl_ctr_keys* ctx, const void* iv, std::uint32_t counter, void* data, std::size_t len
) {
  return br_aes_x86ni_ctr_run(ctx, iv, counter, data, len);
}

inline br_ghash bearssl_select_ghash() { return br_ghash_pclmul; }
#elif defined(SN_BEARSSL_USE_CT64)
using bearssl_ctr_keys = br_aes_ct64_ctr_keys;

inline void bearssl_ctr_init(bearssl_ctr_keys* ctx, const void* key, std::size_t len) {
  br_aes_ct64_ctr_init(ctx, key, len);
}

inline std::uint32_t bearssl_ctr_run(
    const bearssl_ctr_keys* ctx, const void* iv, std::uint32_t counter, void* data, std::size_t len
) {
  return br_aes_ct64_ctr_run(ctx, iv, counter, data, len);
}

inline br_ghash bearssl_select_ghash() { return br_ghash_ctmul; }
#else
#error "Unsupported BearSSL backend configuration"
#endif

inline const br_block_ctr_class** bearssl_ctr_vtable(bearssl_ctr_keys& ctx) {
  return const_cast<const br_block_ctr_class**>(reinterpret_cast<const br_block_ctr_class**>(&ctx));
}

struct bearssl_ctr_cipher_state {
  bearssl_ctr_keys keys{};
};

struct bearssl_prf_state {
  bearssl_ctr_keys keys{};
#if defined(SN_BEARSSL_USE_X86NI)
  alignas(16) std::array<std::uint8_t, 16 * 15> round_keys{};
  unsigned rounds = 0;
#endif
};

struct bearssl_gcm_cipher_state {
  bearssl_ctr_keys ctr_keys{};
  br_gcm_context gcm{};
};

struct bearssl_prng_state {
  bearssl_ctr_keys keys{};
  std::array<std::uint8_t, 12> iv{};
  std::uint32_t counter = 0;
  std::array<std::uint8_t, 16> keystream{};
  std::size_t keystream_used = keystream.size();
};

}
