#include "sonic_adapter.h"

#include <iostream>
#include <cstring>
#include <stdexcept>
#include <map>
#include <random>

#include "openssl/rand.h"

#include "sonic/threads/tuning.hpp"

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/oram/storage/slab_store.hpp"
#include "sonic/oram/core/access.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/span.hpp"
#include "src/utils/bytes.h"

namespace dyno::dynamic_stepping_path_oram {

// Fixed block size of 256 bytes for Sonic compatibility.
constexpr size_t kSonicBlockBytes = 256;
using SonicTraits = sn::oram::zingoram::traits<kSonicBlockBytes, sn::oram::zingoram::epoch_mode::default_epoch, sn::oram::zingoram::storage::slab_store>;
using SonicClient = sn::oram::zingoram::client<SonicTraits>;

struct SonicORamAdapter::Impl {
  sn::threads::thread_context thread_ctx{sn::threads::thread_policy{.affinity = sn::threads::thread_affinity::inherit}};
  std::unique_ptr<sn::threads::pthread_thread_pool> eviction_pool;
  std::unique_ptr<sn::threads::thread_team> eviction_team;
  std::unique_ptr<SonicClient> client;
  SonicClient::access_scratch scratch;

  std::vector<uint64_t> pos_map;
  std::mt19937_64 rng{std::random_device{}()};
  bool with_pos_map;

  Impl(size_t capacity, bool w_pos_map) : with_pos_map(w_pos_map) {
    thread_ctx.bind_current_thread();
    
    // E=1 (single eviction thread per subtree)
    eviction_pool = std::make_unique<sn::threads::pthread_thread_pool>(thread_ctx, 0, "oram-evict");
    eviction_team = std::make_unique<sn::threads::thread_team>(eviction_pool->pool(), 1);

    SonicTraits::options_t opts{};
    opts.block_count = capacity + 1; // Allocate 1 extra block for safe dummy accesses
    opts.bucket_real_size = 5;
    opts.bucket_dummy_size = 7;
    opts.eviction_rate = 0; // 0 means auto-compute optimal eviction rate based on bucket size
    opts.routing_depth = 0; // r=0 (forest cut depth)
    opts.evict_batch = 1; // E=1
    opts.access_concurrency = 1;

    client = std::make_unique<SonicClient>(opts, std::move(*eviction_team));
    client->initialize();
    client->configure_access_scratch(scratch);

    if (with_pos_map) {
        pos_map.resize(capacity + 1, UINT64_MAX);
    }
  }

  uint64_t GenerateLeaf() {
    // Generate a uniformly distributed leaf ID based on the tree height.
    uint64_t lc = 1ULL << (client->shape().height - 1);
    std::uniform_int_distribution<uint64_t> dist(0, lc - 1);
    return dist(rng);
  }
};

SonicORamAdapter::SonicORamAdapter(size_t n, size_t val_len, bool with_pos_map)
    : capacity_(n), val_len_(val_len), impl_(std::make_unique<Impl>(n, with_pos_map)) {
}

SonicORamAdapter::SonicORamAdapter(size_t n, size_t val_len, const std::string &file_path,
                                   uint8_t max_levels_in_mem,
                                   bool with_pos_map, bool with_key_gen)
    : capacity_(n), val_len_(val_len), impl_(std::make_unique<Impl>(n, with_pos_map)) {
}

SonicORamAdapter::~SonicORamAdapter() = default;

static_path_oram::Block SonicORamAdapter::ReadAndRemove(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real) {
  uint64_t cur_leaf = sn::obliv::ct_select<uint64_t>(p - 1, 0, is_real);
  uint64_t new_leaf = impl_->GenerateLeaf();
  if (impl_->with_pos_map) {
    uint64_t found_leaf = UINT64_MAX;
    for (size_t i = 1; i <= capacity_; ++i) {
      bool match = sn::obliv::ct_eq<uint64_t>(i, k) & is_real;
      found_leaf = sn::obliv::ct_select<uint64_t>(impl_->pos_map[i], found_leaf, match);
      impl_->pos_map[i] = sn::obliv::ct_select<uint64_t>(new_leaf, impl_->pos_map[i], match);
    }
    bool has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
    cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
  }

  sn::oram::access_request req;
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, is_real);
  req.cur_leaf = cur_leaf;
  req.new_leaf = new_leaf;
  req.is_write = false; 
  
  std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
  std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
  req.in = sn::util::span<uint8_t>(in_buf);
  req.out = sn::util::span<uint8_t>(out_buf);

  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  impl_->client->access(req, impl_->scratch);
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  memory_access_count_ += (post_ops - pre_ops);
  memory_bytes_moved_total_ += (post_ops - pre_ops) * kSonicBlockBytes * 2;

  static_path_oram::Block res(true);
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  size_t block_size = static_path_oram::BlockSize(val_len_);
  if (block_size <= kSonicBlockBytes) {
      bytes::FromBytes(out_buf.data(), res.meta_);
      std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                out_buf.data() + sizeof(static_path_oram::BlockMetadata) + val_len_,
                res.val_.get());
  }
  if (!impl_->with_pos_map) {
      res.meta_.pos_ = new_leaf + 1;
  }
  return res;
}

static_path_oram::Block SonicORamAdapter::Read(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real) {
  uint64_t cur_leaf = sn::obliv::ct_select<uint64_t>(p - 1, 0, is_real);
  uint64_t new_leaf = impl_->GenerateLeaf();

  if (impl_->with_pos_map) {
    uint64_t found_leaf = UINT64_MAX;
    for (size_t i = 1; i <= capacity_; ++i) {
      bool match = sn::obliv::ct_eq<uint64_t>(i, k) & is_real;
      found_leaf = sn::obliv::ct_select<uint64_t>(impl_->pos_map[i], found_leaf, match);
      impl_->pos_map[i] = sn::obliv::ct_select<uint64_t>(new_leaf, impl_->pos_map[i], match);
    }
    bool has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
    cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
  }

  sn::oram::access_request req;
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, is_real);
  req.cur_leaf = cur_leaf;
  req.new_leaf = new_leaf;
  req.is_write = false; 
  
  std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
  std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
  req.in = sn::util::span<uint8_t>(in_buf);
  req.out = sn::util::span<uint8_t>(out_buf);

  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  impl_->client->access(req, impl_->scratch);
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  memory_access_count_ += (post_ops - pre_ops);
  memory_bytes_moved_total_ += (post_ops - pre_ops) * kSonicBlockBytes * 2;

  static_path_oram::Block res(true);
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  size_t block_size = static_path_oram::BlockSize(val_len_);
  if (block_size <= kSonicBlockBytes) {
      bytes::FromBytes(out_buf.data(), res.meta_);
      std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                out_buf.data() + sizeof(static_path_oram::BlockMetadata) + val_len_,
                res.val_.get());
  }
  
  if (!impl_->with_pos_map) {
      res.meta_.pos_ = new_leaf + 1; // Convert back to 1-indexed for the caller
  }
  
  return res;
}

void SonicORamAdapter::Insert(static_path_oram::Block block, crypto::Key enc_key, bool is_real, bool is_new) {
  uint64_t k = block.meta_.key_;
  bool real = is_real & !sn::obliv::ct_eq<uint64_t>(k, 0);
  uint64_t cur_leaf = 0;
  uint64_t write_leaf = 0;
  if (impl_->with_pos_map) {
    bool has_leaf = false;
    uint64_t found_leaf = UINT64_MAX;
    write_leaf = impl_->GenerateLeaf();
    for (size_t i = 1; i <= capacity_; ++i) {
      bool match = sn::obliv::ct_eq<uint64_t>(i, k) & real;
      found_leaf = sn::obliv::ct_select<uint64_t>(impl_->pos_map[i], found_leaf, match);
      impl_->pos_map[i] = sn::obliv::ct_select<uint64_t>(write_leaf, impl_->pos_map[i], match);
    }
    has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
    cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
    
    // If it was not in the pos_map, it is a brand new bucket.
    if (found_leaf == UINT64_MAX) {
      is_new = true;
    }
  } else {
    write_leaf = sn::obliv::ct_select<uint64_t>(block.meta_.pos_ - 1, 0, real);
    cur_leaf = write_leaf;
  }
  
  sn::oram::access_request req;
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, real);
  req.cur_leaf = cur_leaf;
  req.new_leaf = write_leaf;
  req.is_write = sn::obliv::ct_select<bool>(true, false, real); 
  
  std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
  std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
  
  size_t block_size = static_path_oram::BlockSize(val_len_);
  if (block_size <= kSonicBlockBytes) {
      block.ToBytes(val_len_, in_buf.data());
  }
  req.in = sn::util::span<uint8_t>(in_buf);
  req.out = sn::util::span<uint8_t>(out_buf);

  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  if (is_new && real) {
    sn::oram::tree::block<kSonicBlockBytes> new_block{};
    new_block.address = k - 1;
    new_block.leaf_ix = write_leaf;
    std::copy(in_buf.begin(), in_buf.end(), new_block.data.begin());
    impl_->client->insert(new_block);
  } else {
    impl_->client->access(req, impl_->scratch);
  }
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  memory_access_count_ += (post_ops - pre_ops);
  memory_bytes_moved_total_ += (post_ops - pre_ops) * kSonicBlockBytes * 2;
}

uint64_t SonicORamAdapter::GenerateRandomLeaf() const {
  return impl_->GenerateLeaf() + 1; // Return 1-indexed leaf for the caller
}

} // namespace dyno::dynamic_stepping_path_oram
