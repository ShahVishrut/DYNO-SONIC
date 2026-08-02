#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/oram/tree/block.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/oram/tree/path_buffer.hpp"
#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/stash/core/linear_block_storage.hpp"
#include "sonic/oram/stash/core/section_cursor.hpp"
#include "sonic/oram/stash/pathsort/pipeline.hpp"
#include "sonic/util/profiling.hpp"

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/crypto/buffered_prng.hpp"

namespace sn::oram::stash::pathsort {

namespace log = sn::util::log;

template <typename Block> class stash {
public:
  using storage_type = sn::oram::stash::core::linear_block_storage<Block>;
  using section = typename storage_type::section;
  using writer = sn::oram::stash::core::section_writer<Block>;
  using layout = sn::oram::stash::core::section_layout<Block>;

  struct config {
    std::uint64_t deferred_capacity = 0;
    std::uint64_t pathreads_capacity = 0;
    std::uint64_t evictreads_capacity = 0;
    std::uint64_t workspace_capacity = 0;
    std::uint64_t fillers_capacity = 0;
    pipeline::shape shape{};

    [[nodiscard]] std::uint64_t total_capacity() const noexcept {
      return deferred_capacity + pathreads_capacity + evictreads_capacity + workspace_capacity + fillers_capacity;
    }
  };

  static pipeline::shape make_shape(std::uint64_t height, std::uint64_t bucket_size) noexcept {
    return pipeline::shape::make(height, bucket_size);
  }

  stash(config cfg, const sn::oram::tree::topology& topo, sn::oram::uid_generator& uid_gen, sn::util::log::logger log) :
      cfg_(cfg),
      topology_(topo),
      log_(std::move(log).child("pathsort")),
      uid_gen_(uid_gen),
      shape_(cfg.shape),
      storage_(cfg.total_capacity(), uid_gen_, log_.child("storage")),
      scratch_(cfg.total_capacity()),
      retained_staging_(static_cast<std::size_t>(cfg.deferred_capacity)) {

    // configure layout of stash storage
    layout layout_builder(storage_);
    deferred_section_ = layout_builder.take_section(cfg_.deferred_capacity);
    layout_builder.make_writer(pathreads_writer_, cfg_.pathreads_capacity, "pathreads");
    layout_builder.make_writer(evictreads_writer_, cfg_.evictreads_capacity, "evictreads");
    layout_builder.make_writer(workspace_writer_, cfg_.workspace_capacity, "workspace");
    filler_section_ = layout_builder.take_section(cfg_.fillers_capacity);
    layout_builder.finish();
    // the live region covers the non-filler part of the buffer
    const std::uint64_t live_length =
        cfg_.deferred_capacity + cfg_.pathreads_capacity + cfg_.evictreads_capacity + cfg_.workspace_capacity;
    live_section_ = storage_.make_section(0, live_length);

    // fill staging buffer with dummy blocks
    for (auto& block : retained_staging_) {
      block.set_dummy(uid_gen_);
    }

#if defined(ORAM_DEBUG)
    log_.dbgf(
        "stash layout: total_capacity=%d (deferred=%d, pathreads=%d, evictreads=%d, workspace=%d, fillers=%d)",
        cfg_.total_capacity(), cfg_.deferred_capacity, cfg_.pathreads_capacity, cfg_.evictreads_capacity,
        cfg_.workspace_capacity, cfg_.fillers_capacity
    );
#endif
  }

  // add blocks from path
  void absorb_path(sn::oram::tree::path_buffer<Block>& path) {
    sn_prof_zone("pathsort.stash.absorb_path");
    auto blocks_view = path.blocks();
    sn::util::span<const Block> block_span(blocks_view.data(), blocks_view.size());
    pathreads_writer_.append_span(block_span);
  }

  // take out block by address
  Block extract(std::int64_t address) {
    sn_prof_zone("pathsort.stash.extract");
    const auto remove = sn::obliv::choice(true);
    // we need to search the live region
    return storage_.read(address, remove, live_section_);
  }

  // insert a new block
  void insert(const Block& block) {
    sn_prof_zone("pathsort.stash.insert");
    workspace_writer_.append(block);
  }

  std::uint64_t evict_to_path(std::uint64_t leaf_ix, sn::oram::tree::path_buffer<Block>& out_path) {
    sn_prof_zone("pathsort.stash.evict");
    // step 1: run eviction pipeline
    // this will reorganize the storage into [ evicted | retained | garbage ]
    auto storage_view = storage_.span();
    auto live_view = live_section_.span(storage_);
    auto filler_view = filler_section_.span(storage_);
    pipeline::context<Block> ctx{storage_view,           live_view, filler_view, topology_, shape_,
                                 cfg_.deferred_capacity, uid_gen_,  prng_,       log_};

    const auto result = pipeline::run(ctx, scratch_, leaf_ix);

    // step 2: consolidate the retained stash contents and output evicted to path buffer

    // validate output path buffer
    auto out_path_view = out_path.view();
    auto out_path_blocks = out_path_view.blocks();
    sn::util::log::ensure(
        log_, result.evicted_span.size() == out_path_blocks.size(),
        "pathsort::stash::evict_to_path: evicted span size mismatch"
    );

    // get node ids along path
    topology_.path_to_leaf(leaf_ix, out_path_view.node_ids());

    // copy evicted blocks to output path buffer
    // the evicted buffer is in leaf-root order
    // we copy it back to path buffer in root-leaf order
    {
      const std::size_t bucket = static_cast<std::size_t>(shape_.bucket_size);
      const std::size_t height = static_cast<std::size_t>(shape_.height);
      const Block* evicted = result.evicted_span.data();
      Block* dst = out_path_blocks.begin();
      for (std::size_t chunk = 0; chunk <= height; ++chunk) {
        sn::obliv::copy_n(evicted + chunk * bucket, bucket, dst + (height - chunk) * bucket);
      }
    }

#if defined(ORAM_DEBUG)
    log_.dbgf(
        "evict_to_path: leaf=%d evicted_real=%d retained_real=%d", leaf_ix, result.pop.evicted_real,
        result.pop.retained_real
    );
    sn::oram::tree::debug::log_path_buffer(log_, out_path_view);
#endif

    // step 3: reorganize live storage to [ deferred | dummy-fill ]
    // deferred will be directly filled from retained
    auto deferred_span = deferred_section_.span(storage_);
    auto staging_span = retained_staging_span();

    // copy retained blocks to deferred section
    {
#if defined(ORAM_DEBUG)
      // ensure that retained span size matches deferred span
      sn::util::log::ensure(
          log_, result.retained_span.size() == deferred_span.size(),
          "pathsort::evict_to_path: retained span size mismatch"
      );
      // ensure that the deferred span begins at the start of storage
      sn::util::log::ensure(
          log_, deferred_span.data() == storage_view.data(),
          "pathsort::evict_to_path: deferred span not at start of storage"
      );
#endif
      sn::util::log::ensure(
          log_, staging_span.size() == deferred_span.size(),
          "pathsort::evict_to_path: retained staging span size mismatch"
      );

      sn::obliv::copy(result.retained_span.begin(), result.retained_span.end(), staging_span.begin());
      sn::obliv::copy(staging_span.begin(), staging_span.end(), deferred_span.begin());
    }

    // dummy-fill the rest of live storage
    {
      auto live_span = live_section_.span(storage_);
      for (std::size_t ix = deferred_span.size(); ix < live_span.size(); ++ix) {
        live_span[ix].set_dummy(uid_gen_);
      }
    }

    // reset section staging cursors (storage already dummy-filled above)
    pathreads_writer_.reset_cursor();
    evictreads_writer_.reset_cursor();
    workspace_writer_.reset_cursor();

    return 0;
  }

private:
  config cfg_{};
  const sn::oram::tree::topology& topology_;
  sn::util::log::logger log_;
  sn::oram::uid_generator& uid_gen_;
  pipeline::shape shape_{};
  storage_type storage_;
  sn::crypto::buffered_prng<> prng_{};
  pipeline::scratch scratch_;
  std::vector<Block> retained_staging_;

  section deferred_section_{};
  writer pathreads_writer_;
  writer evictreads_writer_;
  writer workspace_writer_;
  section filler_section_{};
  section live_section_{};

  [[nodiscard]] sn::util::span<Block> retained_staging_span() noexcept {
    return sn::util::span<Block>(retained_staging_.data(), retained_staging_.size());
  }
};

} // namespace sn::oram::stash::pathsort
