#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sonic/crypto/prf.hpp"
#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/omap/o2th/types.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/formatter.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::o2th {

template <typename Key, std::size_t BlockSize> struct state {
  using types = table_types<Key, BlockSize>;
  using key_type = typename types::key_type;
  using bucket_index = typename types::bucket_index;
  using config = typename types::config;
  template <typename T> using maybe_dummy = typename types::template maybe_dummy<T>;
  using op_request = typename types::op_request;
  using data_query = typename types::data_query;
  using mutator_none = typename types::mutator_none;
  using block_flag = typename types::block_flag;
  using op_block = typename types::op_block;
  using data_buffer = typename types::data_buffer;

  // per-worker state for parallel operations
  struct worker_state {
    sn::crypto::buffered_prng<> rng{};
    sn::crypto::prf prf_l1{};
    sn::crypto::prf prf_l2{};
  };

  state(config cfg_in, sn::threads::thread_team workers_in, sn::util::log::logger log_in) :
      log(std::move(log_in)), cfg(cfg_in), workers(std::move(workers_in)) {}

  [[nodiscard]] std::size_t worker_state_count() const noexcept { return worker_states.size(); }

  [[nodiscard]] worker_state& worker_state_for_index(std::size_t index) noexcept {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(index < worker_states.size(), "o2th_rwkv: worker index out of range");
#endif
    return worker_states[index];
  }

  [[nodiscard]] worker_state& serial_worker_state() noexcept {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(!worker_states.empty(), "o2th_rwkv: worker states not initialized");
#endif
    return worker_states.front();
  }

  void initialize_worker_states() {
    const std::size_t slots = workers.logical_threads();
    worker_states.clear();
    worker_states.resize(slots);
#if defined(ORAM_DEBUG)
    log.dbgf("o2th.initialize_worker_states: worker_states=%zu (threads=%zu)", slots, workers.logical_threads());
#endif
  }

  template <typename Fn> void for_each_worker_state(Fn&& fn) {
    for (auto& worker : worker_states) {
      fn(worker);
    }
  }

  // return a span covering only the primary (non-overflow) section of a level
  [[nodiscard]] sn::util::span<op_block> level_primary_span(std::vector<op_block>& blocks) noexcept {
    return sn::util::span<op_block>(blocks.data(), bucket_block_count);
  }

  // return a fixed-sized span for a specific bucket
  [[nodiscard]] sn::util::span<op_block> bucket_view(op_block* blocks_ptr, bucket_index bucket) const noexcept {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(bucket < bucket_count, "o2th_rwkv: bucket index out of range");
#endif
    const std::size_t span = cfg.bucket_size;
    const std::size_t offset = static_cast<std::size_t>(bucket) * span;
    return sn::util::span<op_block>(blocks_ptr + static_cast<std::ptrdiff_t>(offset), span);
  }

#if defined(ORAM_DEBUG)
  [[nodiscard]] bool debug_should_log(sn::util::log::level lvl) const noexcept {
    return static_cast<int>(log.verbosity()) >= static_cast<int>(lvl);
  }

  static std::string debug_flags_to_string(std::uint8_t flags) {
    auto append_tag = [&flags](std::string& dest, block_flag flag, std::string_view name) {
      if ((flags & types::flag_mask(flag)) == 0) {
        return;
      }
      if (!dest.empty()) {
        dest.append(" | ");
      }
      dest.append(name);
    };

    std::string out;
    append_tag(out, block_flag::real, "real");
    append_tag(out, block_flag::dummy, "dummy");
    append_tag(out, block_flag::filler, "filler");
    append_tag(out, block_flag::excess, "excess");
    append_tag(out, block_flag::op_read, "op:read");
    append_tag(out, block_flag::op_write, "op:write");
    if (out.empty()) {
      out = "none";
    }
    return out;
  }

  void debug_dump_blocks(const std::vector<op_block>& blocks, std::string_view stage) const {
    if (!debug_should_log(sn::util::log::level::pedantic)) {
      return;
    }
    std::ostringstream oss;
    const std::string stage_str(stage);
    const std::size_t main_count = bucket_block_count;
    const std::size_t overflow_count = (blocks.size() > main_count) ? (blocks.size() - main_count) : 0;
    oss << pfm::format(
        "%s block dump:\n"
        "  total=%zu primary=%zu overflow=%zu bucket_size=%zu\n",
        stage_str, blocks.size(), main_count, overflow_count, cfg.bucket_size
    );

    if (main_count > 0) {
      oss << "  buckets:\n";
      for (std::size_t ix = 0; ix < main_count; ++ix) {
        if ((cfg.bucket_size != 0) && (ix % cfg.bucket_size == 0)) {
          const std::size_t bucket = ix / cfg.bucket_size;
          oss << pfm::format("    bucket[%03zu]\n", bucket);
        }
        const auto& block = blocks[ix];
        const auto flags_str = debug_flags_to_string(block.flags);
        oss << pfm::format(
            "      slot[%04zu] key=%lld tags=(%u,%u) flags={%s}\n", ix, static_cast<long long>(block.key),
            static_cast<unsigned>(block.tag_l1), static_cast<unsigned>(block.tag_l2), flags_str
        );
      }
    }

    if (overflow_count > 0) {
      oss << "  overflow:\n";
      for (std::size_t ix = 0; ix < overflow_count; ++ix) {
        const auto& block = blocks[main_count + ix];
        const auto flags_str = debug_flags_to_string(block.flags);
        oss << pfm::format(
            "    spill[%04zu] key=%lld tags=(%u,%u) flags={%s}\n", main_count + ix, static_cast<long long>(block.key),
            static_cast<unsigned>(block.tag_l1), static_cast<unsigned>(block.tag_l2), flags_str
        );
      }
    }

    if (blocks.empty() && cfg.bucket_size == 0) {
      oss << "  (empty)\n";
    }

    const auto dump = oss.str();
    log.pedf("%s", dump);
  }
#endif

  mutable sn::util::log::logger log;
  config cfg;
  sn::threads::thread_team workers;

  sn::crypto::buffered_prng<> rng{};
  sn::crypto::prf::key_type epoch_prf_key_l1{};
  sn::crypto::prf::key_type epoch_prf_key_l2{};

  // worker buffers
  std::vector<worker_state> worker_states{};
  // level 1 blocks
  std::vector<op_block> level1_blocks{};
  // level 2 blocks
  std::vector<op_block> level2_blocks{};
  // compact marks
  std::vector<std::uint8_t> compact_marks{};
  // compact prefix sum buffer
  std::vector<std::size_t> compact_prefix{};

  // bucket count for addressable buckets in the block vectors above
  std::size_t bucket_count = 0;
  // number of blocks in the block vectors that correspond to buckets
  std::size_t bucket_block_count = 0;
  // number of blocks in the block vectors that correspond to filler blocks
  std::size_t filler_block_count = 0;
};

} // namespace sn::omap::o2th
