#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "sonic/crypto/prng.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/ops/word_ops.hpp"
#include "sonic/omap/o2th/state.hpp"
#include "sonic/util/formatter.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::o2th {

// resets all blocks in a vector to their default (dummy) state
// used during initialization to ensure clean starting state
template <typename Key, std::size_t BlockSize>
inline void reset_storage(std::vector<typename table_types<Key, BlockSize>::op_block>& blocks) noexcept {
  for (auto& block : blocks) {
    block.reset();
  }
}

// serialize a key into bytes for prf input (to its raw byte representation)
template <typename Key, std::size_t BlockSize>
[[nodiscard]] inline std::array<std::uint8_t, sizeof(Key)> serialize_key(Key key) noexcept {
  static_assert(std::is_trivially_copyable_v<Key>, "serialize_key requires trivially copyable key types");
  std::array<std::uint8_t, sizeof(Key)> bytes{};
  sn::obliv::memcpy(bytes.data(), &key, bytes.size());
  return bytes;
}

// refresh per-worker prfs and rng with epoch material
template <typename Key, std::size_t BlockSize>
inline void rekey_worker_states(
    typename state<Key, BlockSize>::worker_state& worker, const sn::crypto::prf::key_type& key_l1,
    const sn::crypto::prf::key_type& key_l2, sn::crypto::prng::seed_material seed
) {
  worker.prf_l1.set_key(key_l1);
  worker.prf_l2.set_key(key_l2);
  worker.rng.reseed(seed);
}

// derive a bucket index from a key using a prf
// each prf maps keys to a random positions in [0, bucket_count)
template <std::size_t Level, typename Key, std::size_t BlockSize>
inline typename table_types<Key, BlockSize>::bucket_index derive_bucket_index(
    typename state<Key, BlockSize>::worker_state& worker, Key key, std::size_t bucket_count
) noexcept {
  const auto input = serialize_key<Key, BlockSize>(key);
  std::array<std::uint8_t, sn::crypto::prf::block_size> output{};

  // compute prf(key) for the level
  if constexpr (Level == 1) {
    worker.prf_l1.derive(sn::util::span<const std::uint8_t>(input.data(), input.size()), output);
  } else {
    worker.prf_l2.derive(sn::util::span<const std::uint8_t>(input.data(), input.size()), output);
  }

  // convert raw prf output to bucket index
  std::uint32_t raw = 0;
  sn::obliv::memcpy(&raw, output.data(), sizeof(raw));
  return static_cast<typename table_types<Key, BlockSize>::bucket_index>(raw % bucket_count);
}

// generate fresh prf keys for the current epoch; must be called each build
template <typename Key, std::size_t BlockSize> inline void reseed_epoch(state<Key, BlockSize>& st) {
  sn_prof_zone("o2th.reseed_epoch");
  st.epoch_prf_key_l1 = sn::crypto::prf::generate_key(st.rng.engine());
  st.epoch_prf_key_l2 = sn::crypto::prf::generate_key(st.rng.engine());
#if defined(ORAM_DEBUG)
  st.log.dbgf(
      "o2th.reseed_epoch: refreshing prf keys for %zu worker states", static_cast<std::size_t>(st.worker_states.size())
  );
#endif
  // for all buffer sets, re-key prf
  st.for_each_worker_state([&st](typename state<Key, BlockSize>::worker_state& worker) {
    rekey_worker_states<Key, BlockSize>(
        worker, st.epoch_prf_key_l1, st.epoch_prf_key_l2, sn::crypto::prng::make_seed()
    );
  });
}

// export an op_block into an output item (constant-time)
// the mark output is set to 1 for real blocks, 0 for dummy blocks (used for compaction)
template <typename Key, std::size_t BlockSize>
inline void export_block(
    const typename table_types<Key, BlockSize>::op_block& block,
    typename table_types<Key, BlockSize>::template maybe_dummy<typename table_types<Key, BlockSize>::op_request>& out,
    std::uint8_t& mark
) noexcept {
  const sn::obliv::choice block_is_real = block.is_real();
  out.is_dummy = !block_is_real.unwrap();
  out.value.is_write = block.is_op_write().unwrap();
  out.value.key = block.key;
  out.value.extra_data = block.extra_data;
  sn::obliv::ct_set_words<BlockSize>(out.value.data.data(), block.data.data(), block_is_real.unwrap());
  mark = static_cast<std::uint8_t>(block_is_real.unwrap());
}

template <typename Key, std::size_t BlockSize> inline void initialize(state<Key, BlockSize>& st) {
  sn_prof_zone("o2th.initialize");
  sn::util::log::ensure(st.cfg.bucket_size > 0, "o2th_rwkv: bucket_size must be positive");
  sn::util::log::ensure(st.cfg.block_count > 0, "o2th_rwkv: block_count must be positive");
  // ensure count is a multiple of bucket size
  sn::util::log::ensure(
      st.cfg.block_count % st.cfg.bucket_size == 0, "o2th_rwkv: block_count must be divisible by bucket_size"
  );

  // compute bucket count
  st.bucket_count = st.cfg.block_count / st.cfg.bucket_size;
  constexpr std::size_t bucket_index_limit =
      static_cast<std::size_t>(std::numeric_limits<typename table_types<Key, BlockSize>::bucket_index>::max());
  sn::util::log::ensure(
      st.bucket_count <= bucket_index_limit, "o2th_rwkv: bucket_count exceeds 32-bit bucket index capacity"
  );
#if defined(ORAM_DEBUG)
  st.log.trcf(
      "o2th.initialize: block_count=%zu, bucket_size=%zu, bucket_count=%zu", st.cfg.block_count, st.cfg.bucket_size,
      st.bucket_count
  );
  st.log.dbgf("o2th.initialize: op_block_size=%zu bytes", sizeof(typename state<Key, BlockSize>::op_block));
#endif
  // compute block counts
  st.bucket_block_count = st.bucket_count * st.cfg.bucket_size;
  st.filler_block_count = st.bucket_block_count;
  const std::size_t level_capacity = st.bucket_block_count + st.filler_block_count;
  sn::util::log::ensure(
      level_capacity == st.bucket_count * st.cfg.bucket_size * 2, "o2th_rwkv: inconsistent level capacity"
  );
#if defined(ORAM_DEBUG)
  const std::size_t per_level_bytes = level_capacity * sizeof(typename state<Key, BlockSize>::op_block);
  const std::uint64_t total_bytes = static_cast<std::uint64_t>(per_level_bytes) * 2ULL;
  auto total_bytes_str = sn::util::format::format_bytes(total_bytes);
  st.log.dbgf(
      "o2th.initialize: allocating buffers per_level=%zu blocks (%zu bytes) -> total=%s", level_capacity,
      per_level_bytes, total_bytes_str
  );
#endif

  // allocate block buffers
  st.level1_blocks.resize(level_capacity);
  st.level2_blocks.resize(level_capacity);
  reset_storage<Key, BlockSize>(st.level1_blocks);
  reset_storage<Key, BlockSize>(st.level2_blocks);

  // misc buffers
  st.compact_marks.resize(level_capacity);
  st.compact_prefix.resize(level_capacity + 1);
  // worker state buffers
  st.initialize_worker_states();

#if defined(ORAM_DEBUG)
  st.log.dbgf(
      "o2th.initialize: worker_states=%zu (threads=%zu)", st.worker_state_count(), st.workers.logical_threads()
  );
#endif

  reseed_epoch<Key, BlockSize>(st);
}

} // namespace sn::omap::o2th
