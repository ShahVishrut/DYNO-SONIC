#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/word_ops.hpp"
#include "sonic/omap/o2th/client.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::suboram {

namespace o2th {

struct config {
  std::size_t block_count = 0;
  std::size_t batch_size = 0;
  std::size_t bucket_size = 64;
};

constexpr std::size_t kHashtableBucketSize = 64;

template <typename Key, std::size_t PayloadBytes> class driver {
public:
  using key_type = Key;
  static constexpr std::size_t payload_bytes = PayloadBytes;
  static_assert(payload_bytes % 8 == 0, "payload size must be multiple of 8 bytes");
  static_assert(std::is_integral_v<key_type>, "o2th_driver: key_type must be integral");

  using o2th_type = sn::omap::o2th::o2th_rwkv<key_type, payload_bytes>;
  using data_query = typename o2th_type::data_query;
  using request_type = typename o2th_type::op_request;

  driver(const config& cfg, sn::threads::thread_team workers) :
      cfg_(cfg),
      build_span_size_(cfg.batch_size),
      retrieve_span_size_(cfg.batch_size * 2),
      table_(
          typename o2th_type::config{.block_count = build_span_size_, .bucket_size = cfg.bucket_size},
          std::move(workers)
      ),
      build_retrieve_buf_(retrieve_span_size_),
      dataset_(cfg.block_count),
      pos_buf_l1_(cfg.block_count),
      pos_buf_l2_(cfg.block_count),
      retrieve_marks_(retrieve_span_size_),
      retrieve_prefix_(retrieve_span_size_ + 1) {

    // validate
    sn::util::log::ensure(cfg.bucket_size == 64, "o2th_driver: bucket_size must be 64");
    sn::util::log::ensure(cfg.block_count > 0, "o2th_driver: block_count must be positive");
    sn::util::log::ensure(cfg.batch_size > 0, "o2th_driver: batch_size must be positive");
    sn::util::log::ensure(cfg.batch_size % cfg.bucket_size == 0, "o2th_driver: batch_size must align with bucket_size");
    sn::util::log::ensure(
        cfg.block_count <= static_cast<std::size_t>(std::numeric_limits<key_type>::max()),
        "o2th_driver: block_count exceeds key space"
    );

    // initialize dataset
    table_.initialize();
    for (std::size_t ix = 0; ix < dataset_.size(); ++ix) {
      auto& item = dataset_[ix];
      item.key = static_cast<key_type>(ix);
      item.data.fill(0);
    }

    // initialize build set span
    for (std::size_t ix = 0; ix < build_span_size_; ++ix) {
      auto& entry = build_retrieve_buf_[ix];
      entry.is_dummy = true;
      entry.value.key = 0;
      entry.value.is_write = false;
      entry.value.data.fill(0);
    }
  }

  [[nodiscard]] std::size_t batch_size() const noexcept { return cfg_.batch_size; }
  [[nodiscard]] std::size_t block_count() const noexcept { return cfg_.block_count; }

  template <typename RoutedSpan> void process_bin(RoutedSpan bin) {
    const std::size_t bin_size = bin.size();
    sn::util::log::ensure(bin_size <= build_span_size_, "o2th_driver: bin larger than batch");

    // span over build set range
    auto build_span = sn::util::span<typename o2th_type::template maybe_dummy<request_type>>(
        build_retrieve_buf_.data(), build_span_size_
    );

    // populate build set from routed bin
    for (std::size_t ix = 0; ix < build_span_size_; ++ix) {
      auto& entry = build_span[ix];

      // dummy
      entry.is_dummy = true;
      entry.value.key = 0;
      entry.value.is_write = false;
      entry.value.extra_data = 0;

      if (ix >= bin_size) {
        // padding
        continue;
      }

      const auto& routed = bin[ix];
      sn::obliv::choice real_slot(!routed.is_dummy);
      sn::obliv::choice write_slot(routed.item.is_write);
      sn::obliv::choice real_write_slot = real_slot && write_slot;
      const key_type routed_key = static_cast<key_type>(routed.item.key);

#if defined(ORAM_DEBUG)
      if (real_slot.unwrap()) {
        sn::util::log::ensure(
            static_cast<std::size_t>(routed.item.key) < cfg_.block_count, "o2th_driver: request key out of range"
        );
      }
#endif

      // conditionally set fields
      entry.is_dummy = routed.is_dummy;
      entry.value.is_write = routed.item.is_write;
      entry.value.key = sn::obliv::ct_select<key_type>(routed_key, entry.value.key, real_slot.unwrap());
      sn::obliv::ct_set_words<payload_bytes>(
          entry.value.data.data(), routed.item.payload.data(), real_write_slot.unwrap()
      );
    }

    // construct hashtable on build set
    table_.build(build_span);

    // scan the dataset, matching it to build set
    table_.access_batch(
        sn::util::span<data_query>(dataset_.data(), dataset_.size()),
        sn::util::span<typename o2th_type::bucket_index>(pos_buf_l1_.data(), pos_buf_l1_.size()),
        sn::util::span<typename o2th_type::bucket_index>(pos_buf_l2_.data(), pos_buf_l2_.size())
    );

    // span over full retrieval range
    auto retrieve_span = sn::util::span<typename o2th_type::template maybe_dummy<request_type>>(
        build_retrieve_buf_.data(), retrieve_span_size_
    );

    // retrieve updated build set items
    table_.retrieve(
        retrieve_span, sn::util::span<std::uint8_t>(retrieve_marks_.data(), retrieve_marks_.size()),
        sn::util::span<std::size_t>(retrieve_prefix_.data(), retrieve_prefix_.size())
    );

    // populate routed bin from fulfilled build set
    for (std::size_t ix = 0; ix < bin_size; ++ix) {
      auto& entry = retrieve_span[ix];
      auto& routed = bin[ix];

      sn::obliv::choice real_slot(!entry.is_dummy);

      // conditionally set fields
      routed.is_dummy = entry.is_dummy;
      routed.item.is_write = entry.value.is_write;
      routed.item.key =
          sn::obliv::ct_select<key_type>(entry.value.key, static_cast<key_type>(routed.item.key), real_slot.unwrap());
      sn::obliv::ct_set_words<payload_bytes>(routed.item.payload.data(), entry.value.data.data(), real_slot.unwrap());
    }
  }

  void maintenance() {}

  [[nodiscard]] sn::util::span<const data_query> queries() const {
    return sn::util::span<const data_query>(dataset_.data(), dataset_.size());
  }

private:
  config cfg_{};
  std::size_t build_span_size_{};
  std::size_t retrieve_span_size_{};
  o2th_type table_;
  std::vector<typename o2th_type::template maybe_dummy<request_type>> build_retrieve_buf_;
  std::vector<data_query> dataset_;
  std::vector<typename o2th_type::bucket_index> pos_buf_l1_;
  std::vector<typename o2th_type::bucket_index> pos_buf_l2_;
  std::vector<std::uint8_t> retrieve_marks_;
  std::vector<std::size_t> retrieve_prefix_;
};

} // namespace o2th

} // namespace sn::omap::suboram
