#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "sonic/util/log.hpp"

#include "sonic/oram/pathoram/analysis.hpp"
#include "sonic/oram/pathoram/bucket.hpp"
#include "sonic/oram/pathoram/options.hpp"
#if defined(ORAM_DEBUG)
#include "sonic/oram/tree/assigned_block_map.hpp"
#endif
#include "sonic/oram/tree/bucket_heap.hpp"
#include "sonic/oram/stash/pathsort/stash.hpp"
#include "sonic/oram/tree/path_buffer.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/oram/core/uid_generator.hpp"

namespace sn::oram::pathoram {

namespace log = sn::util::log;

template <typename Traits> class state {
public:
  using options_t = typename Traits::options_t;
  using block_t = typename Traits::block_t;
  using bucket_t = typename Traits::bucket_t;
  using stash_t = typename Traits::stash_t;
  using stash_config = typename stash_t::config;
  static constexpr std::size_t bucket_size = Traits::bucket_size;

  struct geometry {
    std::uint64_t height = 0;
    std::uint64_t leaf_count = 1;
    std::uint64_t node_count = 1;
    std::uint64_t path_block_count = 0;
  };

  struct access_scratch {
    void configure(const geometry& shape) { buffer.configure(shape.height, bucket_size); }

    sn::oram::tree::path_buffer<block_t> buffer;
  };

  struct eviction_ctx {
    sn::oram::tree::path_buffer<block_t> buffer;

    eviction_ctx() = default;
    eviction_ctx(const geometry& shape) : buffer(shape.height, bucket_size) {}
  };

  state(options_t opts, sn::util::log::logger logger);

  void initialize();

  void configure_access_scratch(access_scratch& scratch) const { scratch.configure(shape_); }

  const geometry& shape() const noexcept { return shape_; }
  const options_t& options() const noexcept { return options_; }

  sn::util::log::logger& log() noexcept { return logger_; }
  const sn::util::log::logger& log() const noexcept { return logger_; }

  sn::oram::tree::topology& topology() noexcept { return topology_; }
  const sn::oram::tree::topology& topology() const noexcept { return topology_; }

  sn::oram::tree::bucket_heap<bucket_t>& storage() noexcept { return storage_; }
  const sn::oram::tree::bucket_heap<bucket_t>& storage() const noexcept { return storage_; }

  stash_t& stash() noexcept { return stash_; }
  const stash_t& stash() const noexcept { return stash_; }

  sn::oram::uid_generator& uid_gen() noexcept { return uid_gen_; }
  const sn::oram::uid_generator& uid_gen() const noexcept { return uid_gen_; }

#if defined(ORAM_DEBUG)
  sn::oram::tree::assigned_block_map& assigned_blocks() noexcept { return assigned_blocks_; }
  const sn::oram::tree::assigned_block_map& assigned_blocks() const noexcept { return assigned_blocks_; }
#endif

  eviction_ctx& eviction_buffers() noexcept { return eviction_ctx_; }
  const eviction_ctx& eviction_buffers() const noexcept { return eviction_ctx_; }

private:
  geometry derive_geometry(const options_t& opts);
  stash_config derive_stash_config(const options_t& opts, const geometry& shape);

  geometry shape_{};
  options_t options_{};
  sn::oram::uid_generator uid_gen_{};
  sn::oram::tree::topology topology_;
  sn::oram::tree::bucket_heap<bucket_t> storage_;
  sn::util::log::logger logger_;
  stash_t stash_;
  eviction_ctx eviction_ctx_;
#if defined(ORAM_DEBUG)
  sn::oram::tree::assigned_block_map assigned_blocks_;
#endif
};

template <typename Traits>
state<Traits>::state(options_t opts, sn::util::log::logger logger) :
    shape_(derive_geometry(opts)),
    options_(std::move(opts)),
    uid_gen_(),
    topology_(shape_.height, 2),
    storage_(),
    logger_(std::move(logger)),
    stash_(derive_stash_config(options_, shape_), topology_, uid_gen_, logger_.child("stash")),
    eviction_ctx_(shape_) {
#if defined(ORAM_DEBUG)
  assigned_blocks_.configure(options_.block_count);
#endif
}

template <typename Traits> void state<Traits>::initialize() {
  uid_gen_.reset();

  storage_.initialize(shape_.node_count, uid_gen_);
#if defined(ORAM_DEBUG)
  assigned_blocks_.reset();
#endif
}

template <typename Traits> typename state<Traits>::geometry state<Traits>::derive_geometry(const options_t& opts) {
  log::ensure(opts.block_count > 0, "pathoram::state: block_count must be positive");
  log::ensure(opts.evict_batch > 0, "pathoram::state: evict_batch must be positive");

  // create oram tree with height based on analysis
  geometry result{};
  const std::uint64_t height = pathoram_tree_height(opts.block_count);
  result.height = height;
  // leaf and node counts
  const std::uint64_t leaf_count = 1ULL << height;
  result.leaf_count = leaf_count;
  result.node_count = (leaf_count << 1U) - 1U;
  // number of blocks along a root-to-leaf path
  result.path_block_count = (height + 1U) * bucket_size;
  return result;
}

template <typename Traits>
typename state<Traits>::stash_config state<Traits>::derive_stash_config(const options_t& opts, const geometry& shape) {
  stash_config cfg{};

  // stash bound
  cfg.deferred_capacity = 80;

  // shape
  cfg.shape = stash_t::make_shape(shape.height, bucket_size);

  const std::uint64_t path_blocks = cfg.shape.path_block_count;
  // each access reads a path, with k accesses between evictions
  cfg.pathreads_capacity = path_blocks * opts.evict_batch;
  // eviction writes back exactly one path per batch
  cfg.evictreads_capacity = path_blocks;
  // each access modifies one block
  cfg.workspace_capacity = opts.evict_batch;
  // filler slots used only during eviction sorts
  cfg.fillers_capacity = path_blocks;

  return cfg;
}

} // namespace sn::oram::pathoram
