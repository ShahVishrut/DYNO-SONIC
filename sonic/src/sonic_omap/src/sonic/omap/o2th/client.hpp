#pragma once

#include <type_traits>
#include <utility>

#include "sonic/omap/o2th/access.hpp"
#include "sonic/omap/o2th/build.hpp"

namespace sn::omap::o2th {

// oblivious two-tier hashtable (O2TH) variant for a read-write (RW) key-value (KV) store
// here, the build set is the kv queries, and the query set is the dataset
// because accessing queries against data is symmetric to accessing data against queries
template <typename Key, std::size_t BlockSize> class o2th_rwkv {
public:
  using types = table_types<Key, BlockSize>;
  using state_type = state<Key, BlockSize>;
  using key_type = typename types::key_type;
  using bucket_index = typename types::bucket_index;
  static constexpr std::size_t block_size = types::block_size;
  static constexpr std::size_t block_word_count = types::block_word_count;

  static_assert(std::is_integral_v<key_type>, "o2th_rwkv requires integral key type");
  static_assert(block_size % sizeof(std::uint64_t) == 0, "o2th_rwkv: block_size must be divisible by 8 bytes");

  using data_buffer = typename types::data_buffer;

  using config = typename types::config;

  template <typename T> using maybe_dummy = typename types::template maybe_dummy<T>;

  using op_request = typename types::op_request;
  using request_type = op_request;

  using data_query = typename types::data_query;

  // default mutator that does nothing
  using mutator_none = typename types::mutator_none;

  o2th_rwkv(
      config cfg, sn::threads::thread_team workers, sn::util::log::logger log = sn::util::log::create("omap:o2th")
  ) :
      state_(cfg, std::move(workers), std::move(log)) {
    log_configuration();
  }

  void initialize() { ::sn::omap::o2th::initialize<Key, BlockSize>(state_); }

  [[nodiscard]] std::size_t block_count() const noexcept { return state_.cfg.block_count; }
  [[nodiscard]] std::size_t bucket_size() const noexcept { return state_.cfg.bucket_size; }
  [[nodiscard]] const config& config_ref() const noexcept { return state_.cfg; }

  // construct hashtable over build set
  void build(sn::util::span<maybe_dummy<op_request>> in_data) {
    ::sn::omap::o2th::build<Key, BlockSize>(state_, in_data);
  }

  // match a single key (serial query)
  template <typename Mutator = mutator_none>
  bool access_one(key_type key, std::uint8_t* item_data, Mutator&& mutator = Mutator{}) {
    return ::sn::omap::o2th::access_one<Key, BlockSize>(state_, key, item_data, std::forward<Mutator>(mutator));
  }

  // match multiple keys (parallel query)
  // @param data query items
  // @param pos_buf_l1 buffer for level 1 bucket positions
  // @param pos_buf_l2 buffer for level 2 bucket positions
  // @param mutator optional callable invoked for each scanned bucket slot
  template <typename Mutator = mutator_none>
  void access_batch(
      sn::util::span<data_query> queries, sn::util::span<bucket_index> pos_buf_l1,
      sn::util::span<bucket_index> pos_buf_l2, Mutator&& mutator = Mutator{}
  ) {
    ::sn::omap::o2th::access_batch<Key, BlockSize>(
        state_, queries, pos_buf_l1, pos_buf_l2, std::forward<Mutator>(mutator)
    );
  }

  // after executing queries, retrieve updated build set items
  void retrieve(
      sn::util::span<maybe_dummy<op_request>> out_data, sn::util::span<std::uint8_t> compact_marks,
      sn::util::span<std::size_t> compact_prefix
  ) {
    ::sn::omap::o2th::retrieve<Key, BlockSize>(state_, out_data, compact_marks, compact_prefix);
  }

private:
  void log_configuration() {
    const auto& cfg = state_.cfg;
    const std::size_t worker_count = state_.workers.logical_threads();
    state_.log.inff(
        "o2th::client: block_count=%zu block_bytes=%zu bucket_size=%zu workers=%zu", cfg.block_count, block_size,
        cfg.bucket_size, worker_count
    );
  }

  state_type state_;
};

} // namespace sn::omap::o2th
