#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "sonic/oram/stash/forestzing/stash.hpp"
#include "sonic/oram/storage/slab_store.hpp"
#include "sonic/oram/tree/block.hpp"
#include "sonic/oram/zingoram/bucket.hpp"
#include "sonic/oram/zingoram/options.hpp"
#include "sonic/storage/io/backend.hpp"

namespace sn::oram::zingoram {

enum class epoch_mode : std::uint8_t {
  default_epoch = 0,
  disjoint_epoch = 1,
};

namespace detail {
struct null_cache_backend_factory {
  template <typename Block, typename CachePlan>
  static std::unique_ptr<sn::storage::io::backend> make(const options&, const CachePlan&) {
    return nullptr;
  }
};
} // namespace detail

template <
    std::size_t BlockBytes, epoch_mode Mode = epoch_mode::default_epoch,
    template <typename, typename...> class BlockStoreT = sn::oram::zingoram::storage::slab_store,
    typename CacheBackendFactory = detail::null_cache_backend_factory>
struct traits {
  static constexpr std::size_t block_bytes = BlockBytes;
  static constexpr epoch_mode mode = Mode;
  static constexpr bool disjoint_epoch_mode = Mode == epoch_mode::disjoint_epoch;

  using block_t = sn::oram::tree::block<BlockBytes>;
  using options_t = options;
  using block_store_t = BlockStoreT<block_t>;
  using bucket_t = sn::oram::zingoram::bucket<block_t, block_store_t>;
  using stash_t = sn::oram::stash::forestzing::stash<block_t>;
  using cache_backend_factory = CacheBackendFactory;
};

} // namespace sn::oram::zingoram
