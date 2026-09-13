#include "sonic_adapter.h"

#include <iostream>
#include <cstring>
#include <stdexcept>
#include <map>
#include <random>
#include <thread>
#include <future>

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
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace dyno::dynamic_stepping_path_oram {

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) worker.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

#include "sonic/oram/zingoram/analysis.hpp"

// Global thread pool for access workers to prevent std::system_error on rapid thread creation
static std::unique_ptr<ThreadPool> g_access_pool = nullptr;
static std::once_flag g_pool_init_flag;


using SonicTraits = sn::oram::zingoram::traits<kSonicBlockBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::slab_store>;
using SonicClient = sn::oram::zingoram::client<SonicTraits>;

struct SonicORamAdapter::Impl {
  sn::threads::thread_context thread_ctx{sn::threads::thread_policy{.affinity = sn::threads::thread_affinity::inherit}};
  std::unique_ptr<sn::threads::pthread_thread_pool> domain_pool;
  std::unique_ptr<sn::threads::thread_team> eviction_team;
  std::unique_ptr<SonicClient> client;
  SonicClient::access_scratch scratch;

  std::vector<uint64_t> pos_map;
  std::mt19937_64 rng{std::random_device{}()};
  bool with_pos_map;

  Impl(size_t capacity, bool w_pos_map) : with_pos_map(w_pos_map) {
    thread_ctx.bind_current_thread();
    
    domain_pool = std::make_unique<sn::threads::pthread_thread_pool>(thread_ctx, 7, "oram-domain");
    eviction_team = std::make_unique<sn::threads::thread_team>(domain_pool->pool(), 8);

    SonicTraits::options_t opts{};
    opts.block_count = capacity + 1;
    opts.bucket_real_size = 16;
    opts.bucket_dummy_size = 16;
    opts.eviction_rate = static_cast<uint32_t>(sn::oram::zingoram::analysis::max_eviction_rate(16)); 
    opts.routing_depth = 3; 
    opts.evict_batch = 2; 
    opts.access_concurrency = 8;
    
    uint64_t subtree_count = 1ULL << opts.routing_depth;
    uint64_t num_pathreads = opts.eviction_rate * subtree_count * opts.evict_batch;
    uint64_t window = std::max<uint64_t>(1024ULL, num_pathreads) + 131072ULL; // B
    uint64_t remainder = window % num_pathreads;
    if (remainder != 0) window += (num_pathreads - remainder);
    opts.disjoint_epoch_window = window;

    client = std::make_unique<SonicClient>(opts, std::move(*eviction_team));
    client->initialize();
    client->configure_access_scratch(scratch);

    if (with_pos_map) {
        pos_map.resize(capacity + 1, UINT64_MAX);
    }
  }

  uint64_t GenerateLeaf() {
    // Generate a uniformly distributed leaf ID based on the tree height.
    uint64_t lc = 1ULL << client->shape().height;
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

static_path_oram::Block SonicORamAdapter::ReadAndRemove(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real, bool flush) {
  uint64_t cur_leaf = sn::obliv::ct_select<uint64_t>(p - 1, 0, is_real);
  uint64_t new_leaf = impl_->GenerateLeaf();
  
  if (impl_->with_pos_map) {
      uint64_t found_leaf = UINT64_MAX;
      // Oblivious O(N) linear scan to hide memory access
      for (uint64_t pos = 1; pos <= capacity_; ++pos) {
          bool match = sn::obliv::ct_eq<uint64_t>(pos, k);
          found_leaf = sn::obliv::ct_select<uint64_t>(impl_->pos_map[pos], found_leaf, match);
          // Zero out (remove) if real match, otherwise leave unchanged
          impl_->pos_map[pos] = sn::obliv::ct_select<uint64_t>(UINT64_MAX, impl_->pos_map[pos], match & is_real);
      }
      bool has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
      cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
  }

  sn::oram::access_request req;
  // SONIC expects address = -1 for dummies.
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, static_cast<uint64_t>(-1), is_real);
  req.cur_leaf = cur_leaf;
  req.new_leaf = new_leaf;
  req.is_write = false; 
  
  std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
  std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
  req.in = sn::util::span<uint8_t>(in_buf);
  req.out = sn::util::span<uint8_t>(out_buf);

  thread_local SonicClient::access_scratch tl_scratch;
  thread_local size_t tl_scratch_cap = 0;
  if (tl_scratch_cap != capacity_) {
      impl_->client->configure_access_scratch(tl_scratch);
      tl_scratch_cap = capacity_;
  }

  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  impl_->client->access(req, tl_scratch);
  if (flush) impl_->client->flush_epoch();
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  memory_access_count_ += (post_ops - pre_ops);
  memory_bytes_moved_total_ += (post_ops - pre_ops) * kSonicBlockBytes * 2;

  static_path_oram::Block res(true);
  if (val_len_ > 0) {
      res.val_ = std::make_unique<uint8_t[]>(val_len_);
      size_t block_size = static_path_oram::BlockSize(val_len_);
      if (block_size <= kSonicBlockBytes) {
          bytes::FromBytes(out_buf.data(), res.meta_);
          std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                    out_buf.data() + sizeof(static_path_oram::BlockMetadata) + val_len_,
                    res.val_.get());
      }
  }
  
  if (!impl_->with_pos_map) {
      res.meta_.pos_ = new_leaf + 1;
  }
  return res;
}

static_path_oram::Block SonicORamAdapter::Read(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real, bool flush) {
  uint64_t cur_leaf = sn::obliv::ct_select<uint64_t>(p - 1, 0, is_real);
  uint64_t new_leaf = impl_->GenerateLeaf();

  if (impl_->with_pos_map) {
      uint64_t found_leaf = UINT64_MAX;
      // Oblivious O(N) linear scan to hide memory access
      for (uint64_t pos = 1; pos <= capacity_; ++pos) {
          bool match = sn::obliv::ct_eq<uint64_t>(pos, k);
          found_leaf = sn::obliv::ct_select<uint64_t>(impl_->pos_map[pos], found_leaf, match);
          // Reassign new random leaf if real match
          impl_->pos_map[pos] = sn::obliv::ct_select<uint64_t>(new_leaf, impl_->pos_map[pos], match & is_real);
      }
      bool has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
      cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
  }

  sn::oram::access_request req;
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, static_cast<uint64_t>(-1), is_real);
  req.cur_leaf = cur_leaf;
  req.new_leaf = new_leaf;
  req.is_write = false; 
  
  std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
  std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
  req.in = sn::util::span<uint8_t>(in_buf);
  req.out = sn::util::span<uint8_t>(out_buf);

  thread_local SonicClient::access_scratch tl_scratch;
  thread_local size_t tl_scratch_cap = 0;
  if (tl_scratch_cap != capacity_) {
      impl_->client->configure_access_scratch(tl_scratch);
      tl_scratch_cap = capacity_;
  }

  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  impl_->client->access(req, tl_scratch);
  if (flush) impl_->client->flush_epoch();
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  memory_access_count_ += (post_ops - pre_ops);
  memory_bytes_moved_total_ += (post_ops - pre_ops) * kSonicBlockBytes * 2;

  static_path_oram::Block res(true);
  if (val_len_ > 0) {
      res.val_ = std::make_unique<uint8_t[]>(val_len_);
      size_t block_size = static_path_oram::BlockSize(val_len_);
      if (block_size <= kSonicBlockBytes) {
          bytes::FromBytes(out_buf.data(), res.meta_);
          std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                    out_buf.data() + sizeof(static_path_oram::BlockMetadata) + val_len_,
                    res.val_.get());
      }
  }
  
  if (!impl_->with_pos_map) {
      res.meta_.pos_ = new_leaf + 1; // Convert back to 1-indexed for the caller
  }
  
  // Zero out the result for dummies
  res.meta_.key_ = sn::obliv::ct_select<uint64_t>(res.meta_.key_, 0, is_real);
  res.meta_.pos_ = sn::obliv::ct_select<uint64_t>(res.meta_.pos_, 0, is_real);
  if (val_len_ > 0) {
      std::vector<uint8_t> zeros(val_len_, 0);
      sn::obliv::ct_select_array(res.val_.get(), res.val_.get(), zeros.data(), val_len_, is_real);
  }
  
  return res;
}

void SonicORamAdapter::Insert(static_path_oram::Block block, crypto::Key enc_key, bool is_real, bool is_new, bool flush) {
  uint64_t k = block.meta_.key_;
  bool real = is_real & !sn::obliv::ct_eq<uint64_t>(k, 0);
  uint64_t cur_leaf = 0;
  uint64_t write_leaf = 0;
  
  if (impl_->with_pos_map) {
      uint64_t found_leaf = UINT64_MAX;
      write_leaf = impl_->GenerateLeaf();
      // Oblivious O(N) linear scan to hide memory access
      for (uint64_t pos = 1; pos <= capacity_; ++pos) {
          bool match = sn::obliv::ct_eq<uint64_t>(pos, k);
          found_leaf = sn::obliv::ct_select<uint64_t>(impl_->pos_map[pos], found_leaf, match);
          // Assign write_leaf if real match
          impl_->pos_map[pos] = sn::obliv::ct_select<uint64_t>(write_leaf, impl_->pos_map[pos], match & real);
      }
      bool has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
      cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
      
      // Determine is_new dynamically based on scan result to avoid secret drift
      is_new = !has_leaf;
  } else {
      uint64_t r_leaf = impl_->GenerateLeaf();
      write_leaf = sn::obliv::ct_select<uint64_t>(block.meta_.pos_ - 1, r_leaf, real);
      cur_leaf = write_leaf;
  }

  thread_local std::array<uint8_t, kSonicBlockBytes> in_buf;
  thread_local std::array<uint8_t, kSonicBlockBytes> out_buf;
  std::fill(in_buf.begin(), in_buf.end(), 0);
  std::fill(out_buf.begin(), out_buf.end(), 0);
  size_t block_size = static_path_oram::BlockSize(val_len_);
  if (block_size <= kSonicBlockBytes) {
      block.ToBytes(val_len_, in_buf.data());
  }
  
  // Oblivious dummy mask
  std::vector<uint8_t> zeros(kSonicBlockBytes, 0);
  sn::obliv::ct_select_array(in_buf.data(), in_buf.data(), zeros.data(), kSonicBlockBytes, real);

  thread_local SonicClient::access_scratch tl_scratch;
  thread_local size_t tl_scratch_cap = 0;
  if (tl_scratch_cap != capacity_) {
      impl_->client->configure_access_scratch(tl_scratch);
      tl_scratch_cap = capacity_;
  }

  // To maintain control flow obliviousness, we execute BOTH operations 
  // unconditionally. The real execution gets the valid data, the other gets -1 padding.
  bool execute_insert = is_new & real;
  bool execute_access = !is_new & real;
  
  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  
  // 1. Unconditional Stash Insert (Real or Dummy)
  sn::oram::tree::block<kSonicBlockBytes> new_block{};
  new_block.address = sn::obliv::ct_select<uint64_t>(k - 1, static_cast<uint64_t>(-1), execute_insert);
  new_block.leaf_ix = sn::obliv::ct_select<uint64_t>(write_leaf, 0, real);
  std::copy(in_buf.begin(), in_buf.end(), new_block.data.begin());
  impl_->client->insert(new_block);

  // 2. Unconditional Tree Access (Real or Dummy)
  sn::oram::access_request req;
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, static_cast<uint64_t>(-1), execute_access);
  req.cur_leaf = sn::obliv::ct_select<uint64_t>(cur_leaf, 0, execute_access);
  req.new_leaf = sn::obliv::ct_select<uint64_t>(write_leaf, 0, execute_access);
  req.is_write = sn::obliv::ct_select<bool>(true, false, execute_access);
  req.in = sn::util::span<uint8_t>(in_buf.data(), in_buf.size());
  req.out = sn::util::span<uint8_t>(out_buf.data(), out_buf.size());
  impl_->client->access(req, tl_scratch);
  
  if (flush) impl_->client->flush_epoch();
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  
  // We divide by 2 to keep metrics proportional since we intentionally executed both operations
  memory_access_count_ += ((post_ops - pre_ops) / 2);
  memory_bytes_moved_total_ += ((post_ops - pre_ops) / 2) * kSonicBlockBytes * 2;
}

void SonicORamAdapter::FlushEpoch() {
  std::cout << "[DYNO_DEBUG] SonicORamAdapter::FlushEpoch() called. capacity_=" << capacity_ 
            << ", disjoint_window=" << impl_->client->options().disjoint_epoch_window
            << ", leaf_count=" << impl_->client->shape().leaf_count << std::endl;
  impl_->client->flush_epoch();
}

std::vector<static_path_oram::Block> SonicORamAdapter::ReadAndRemoveBatch(const std::vector<AccessOp>& ops, crypto::Key enc_key, bool steady_state) {
  size_t B = ops.size();
  std::vector<static_path_oram::Block> results;
  results.reserve(B);
  for (size_t i = 0; i < B; ++i) results.emplace_back(true);

  std::vector<uint64_t> batch_cur_leaves(B, 0);
  std::vector<uint64_t> batch_new_leaves(B, 0);
  
  for (size_t j = 0; j < B; ++j) {
      batch_cur_leaves[j] = ops[j].cur_leaf;
      batch_new_leaves[j] = impl_->GenerateLeaf();
  }

  int num_workers = 24;
  std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(24); });

  std::vector<uint64_t> thread_access_ops(num_workers, 0);
  size_t chunk_size = 768;
  std::mutex ops_mutex;
  std::condition_variable chunk_cv;

  for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
      size_t chunk_end = std::min(B, chunk_start + chunk_size);
      int tasks_pending = num_workers;
      
      for (int i = 0; i < num_workers; ++i) {
          g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, &ops, &batch_cur_leaves, &batch_new_leaves, &results, &thread_access_ops, &ops_mutex, &tasks_pending, &chunk_cv]() {
              try {
                  thread_local SonicClient::access_scratch tl_scratch;
                  thread_local size_t tl_scratch_cap = 0;
                  if (tl_scratch_cap != capacity_) {
                      impl_->client->configure_access_scratch(tl_scratch);
                      tl_scratch_cap = capacity_;
                  }
                  uint64_t local_ops = 0;
                  for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                      const auto& op = ops[j];
                      sn::oram::access_request req;
                      req.address = sn::obliv::ct_select<uint64_t>(op.key - 1, UINT64_MAX, op.is_real);
                      req.cur_leaf = batch_cur_leaves[j];
                      req.new_leaf = batch_new_leaves[j];
                      req.is_write = false; 
                      
                      std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
                      std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
                      req.in = sn::util::span<uint8_t>(in_buf);
                      req.out = sn::util::span<uint8_t>(out_buf);

                      auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      impl_->client->access(req, tl_scratch);
                      auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      local_ops += (post_ops - pre_ops);

                      static_path_oram::Block res(true);
                      if (val_len_ > 0) {
                          res.val_ = std::make_unique<uint8_t[]>(val_len_);
                          size_t safe_val_len = kSonicBlockBytes > sizeof(static_path_oram::BlockMetadata) ? 
                                                std::min(val_len_, kSonicBlockBytes - sizeof(static_path_oram::BlockMetadata)) : 0;
                          if (safe_val_len > 0) {
                              bytes::FromBytes(out_buf.data(), res.meta_);
                              std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                                        out_buf.data() + sizeof(static_path_oram::BlockMetadata) + safe_val_len,
                                        res.val_.get());
                          }
                      }
                      
                      if (!impl_->with_pos_map) {
                          res.meta_.pos_ = batch_new_leaves[j] + 1;
                      }
                      
                      res.meta_.key_ = sn::obliv::ct_select<uint64_t>(res.meta_.key_, 0, op.is_real);
                      res.meta_.pos_ = sn::obliv::ct_select<uint64_t>(res.meta_.pos_, 0, op.is_real);
                      if (val_len_ > 0) {
                          std::vector<uint8_t> zeros(val_len_, 0);
                          sn::obliv::ct_select_array(res.val_.get(), res.val_.get(), zeros.data(), val_len_, op.is_real);
                      }
                      results[j] = std::move(res);
                  }
                  {
                      std::unique_lock<std::mutex> lock(ops_mutex);
                      thread_access_ops[i] += local_ops;
                      tasks_pending--;
                      if (tasks_pending == 0) chunk_cv.notify_one();
                  }
              } catch (const std::exception& e) {
                  std::cerr << "[CRITICAL ERROR] ReadAndRemoveBatch thread: " << e.what() << std::endl;
                  std::terminate();
              }
          });
      }
      std::unique_lock<std::mutex> lock(ops_mutex);
      chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
  }

  for (int i = 0; i < num_workers; ++i) {
      memory_access_count_ += thread_access_ops[i];
      memory_bytes_moved_total_ += thread_access_ops[i] * kSonicBlockBytes * 2;
  }
  return results;
}

std::vector<static_path_oram::Key> SonicORamAdapter::ObliviousExtractValidKeys(size_t k, size_t T, std::function<bool(static_path_oram::Key)> filter) {
  std::vector<static_path_oram::Key> S_keys(T, 0); 
  uint64_t count = 0;
  for (uint64_t i = 1; i <= capacity_; ++i) { 
      bool is_real = (impl_->pos_map[i] != UINT64_MAX);
      if (is_real && filter && !filter(i)) is_real = false; // Apply filter
      bool take = is_real & (count < k); 
      for (uint64_t j = 0; j < T; ++j) { 
          bool match = take & (count == j);
          S_keys[j] = sn::obliv::ct_select<static_path_oram::Key>(i, S_keys[j], match); 
      }
      count = sn::obliv::ct_select(count + 1, count, take);
  }
  return S_keys;
}

std::vector<static_path_oram::Key> SonicORamAdapter::GetAllValidKeys() const {
  std::vector<static_path_oram::Key> keys;
  for (uint64_t i = 1; i <= capacity_; ++i) {
      if (impl_->pos_map[i] != UINT64_MAX) {
          keys.push_back(i);
      }
  }
  return keys;
}

std::vector<static_path_oram::Block> SonicORamAdapter::ReadBatch(const std::vector<AccessOp>& ops, crypto::Key enc_key, bool steady_state) {
  size_t B = ops.size();
  std::vector<static_path_oram::Block> results;
  results.reserve(B);
  for (size_t i = 0; i < B; ++i) {
    results.emplace_back(true);
  }

  std::vector<uint64_t> batch_cur_leaves(B, 0);
  std::vector<uint64_t> batch_new_leaves(B, 0);
  
  for (size_t j = 0; j < B; ++j) {
      batch_cur_leaves[j] = ops[j].cur_leaf;
      batch_new_leaves[j] = ops[j].new_leaf;
  }

  int num_workers = 24;

  std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(24); });

  std::vector<uint64_t> thread_access_ops(num_workers, 0);
  size_t chunk_size = 768; // 3. Increased chunk size 
  std::mutex ops_mutex;
  std::condition_variable chunk_cv;

  for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
      size_t chunk_end = std::min(B, chunk_start + chunk_size);
      int tasks_pending = num_workers;
      
      for (int i = 0; i < num_workers; ++i) {
          // Fixed the print statement typo
          if (chunk_start % 7680 == 0 && i == 0) std::cout << "    ... ReadBatch processing chunk " << chunk_start << " / " << B << std::endl;
          
          g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, &ops, &batch_cur_leaves, &batch_new_leaves, &results, &thread_access_ops, &ops_mutex, &tasks_pending, &chunk_cv]() {
              try {
                  thread_local SonicClient::access_scratch tl_scratch;
                  thread_local size_t tl_scratch_cap = 0;
                  if (tl_scratch_cap != capacity_) {
                      impl_->client->configure_access_scratch(tl_scratch);
                      tl_scratch_cap = capacity_;
                  }
                  uint64_t local_ops = 0;
                  
                  for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                      const auto& op = ops[j];
                      sn::oram::access_request req;
                      req.address = sn::obliv::ct_select<uint64_t>(op.key - 1, UINT64_MAX, op.is_real);
                      req.cur_leaf = batch_cur_leaves[j];
                      req.new_leaf = batch_new_leaves[j];
                      bool is_update = sn::obliv::ct_eq<uint8_t>(op.op_type, 1);
                      bool is_delete = sn::obliv::ct_eq<uint8_t>(op.op_type, 2);
                      req.is_write = is_update | is_delete;
                      
                      thread_local std::array<uint8_t, kSonicBlockBytes> in_buf;
                      thread_local std::array<uint8_t, kSonicBlockBytes> out_buf;
                      std::fill(in_buf.begin(), in_buf.end(), 0);
                      std::fill(out_buf.begin(), out_buf.end(), 0);
                      
                      if (req.is_write) {
                          static_path_oram::BlockMetadata meta;
                          meta.key_ = sn::obliv::ct_select<uint64_t>(op.key, 0, is_update);
                          meta.pos_ = sn::obliv::ct_select<uint64_t>(batch_new_leaves[j] + 1, 0, is_update);
                          auto meta_bytes = bytes::ToBytes(meta);
                          std::copy(meta_bytes.begin(), meta_bytes.end(), in_buf.data());
                          if (is_update && op.val) {
                              std::copy(op.val.get(), op.val.get() + val_len_, in_buf.data() + sizeof(static_path_oram::BlockMetadata));
                          }
                      }
                      
                      req.in = sn::util::span<uint8_t>(in_buf.data(), in_buf.size());
                      req.out = sn::util::span<uint8_t>(out_buf.data(), out_buf.size());

                      auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      
                      if (req.cur_leaf > capacity_) {
                          std::cout << "[DYNO_CRITICAL] ReadBatch invalid cur_leaf: " << req.cur_leaf 
                                    << " (orig ops[j].cur_leaf: " << op.cur_leaf << ")\n";
                      }
                      
                      // 4. REMOVED MUTEX: Native lock-free concurrency via SONIC's access_gate!
                      impl_->client->access(req, tl_scratch);
                      
                      auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      local_ops += (post_ops - pre_ops);

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
                          res.meta_.pos_ = batch_new_leaves[j] + 1;
                      }
                      
                      // Zero out the result for dummies to prevent data corruption
                      res.meta_.key_ = sn::obliv::ct_select<uint64_t>(res.meta_.key_, 0, op.is_real);
                      
                      // 5. REMOVED UNUSED VARIABLE "pos" causing compiler warning
                      res.meta_.pos_ = sn::obliv::ct_select<uint64_t>(res.meta_.pos_, 0, op.is_real);
                      if (val_len_ > 0) {
                          std::vector<uint8_t> zeros(val_len_, 0);
                          sn::obliv::ct_select_array(res.val_.get(), res.val_.get(), zeros.data(), val_len_, op.is_real);
                      }
                      
                      results[j] = std::move(res);
                  }
                  
                  // Only lock ONCE per thread per chunk to update pending tasks
                  {
                      std::unique_lock<std::mutex> lock(ops_mutex);
                      thread_access_ops[i] += local_ops;
                      tasks_pending--;
                      if (tasks_pending == 0) chunk_cv.notify_one();
                  }
              } catch (const std::exception& e) {
                  std::cerr << "[CRITICAL ERROR] Exception in ReadBatch pool thread: " << e.what() << std::endl;
                  std::terminate();
              }
          });
      }
      std::unique_lock<std::mutex> lock(ops_mutex);
      chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
  }

  for (int i = 0; i < num_workers; ++i) {
      memory_access_count_ += thread_access_ops[i];
      memory_bytes_moved_total_ += thread_access_ops[i] * kSonicBlockBytes * 2;
  }

  return results;
}

void SonicORamAdapter::InsertBatch(std::vector<static_path_oram::Block>& blocks, crypto::Key enc_key, bool steady_state, bool all_new) {
  size_t B = blocks.size();
  
  if (all_new) {
      size_t chunk_size = 768; 
      for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
          size_t chunk_end = std::min(B, chunk_start + chunk_size);
          for (size_t j = chunk_start; j < chunk_end; ++j) {
              uint64_t k = blocks[j].meta_.key_;
              bool is_real = (k != 0);
              
              // PURE BITWISE MASKING: No macros, no branches, no underflow traps.
              uint64_t real_mask = is_real ? 0xFFFFFFFFFFFFFFFFULL : 0x0000000000000000ULL;
              
              uint32_t raw_pos = blocks[j].meta_.pos_;
              uint64_t safe_pos = (raw_pos > 0) ? raw_pos : 1; 
              uint64_t leaf = safe_pos - 1;
              
              thread_local std::array<uint8_t, kSonicBlockBytes> in_buf;
              std::fill(in_buf.begin(), in_buf.end(), 0);
              size_t block_size = static_path_oram::BlockSize(val_len_);
              if (block_size <= kSonicBlockBytes) {
                  auto meta_bytes = bytes::ToBytes(blocks[j].meta_);
                  std::copy(meta_bytes.begin(), meta_bytes.end(), in_buf.begin());
                  if (val_len_ > 0 && blocks[j].val_) {
                      std::copy(blocks[j].val_.get(), blocks[j].val_.get() + val_len_, in_buf.data() + sizeof(static_path_oram::BlockMetadata));
                  }
              }
              
              std::vector<uint8_t> zeros(kSonicBlockBytes, 0);
              sn::obliv::ct_select_array(in_buf.data(), in_buf.data(), zeros.data(), kSonicBlockBytes, is_real);
              
              sn::oram::tree::block<kSonicBlockBytes> new_block{};
              
              // If real, use (k-1) and (leaf). If dummy, mathematically force address to -1 and leaf_ix to 0.
              new_block.address = ((k - 1) & real_mask) | (static_cast<uint64_t>(-1) & ~real_mask);
              new_block.leaf_ix = (leaf & real_mask) | (0 & ~real_mask);
              
              std::copy(in_buf.begin(), in_buf.end(), new_block.data.begin());
              impl_->client->insert(new_block);
          }
      }
      return;
  }
}
//   std::vector<uint64_t> batch_cur_leaves(B, 0);
//   std::vector<uint64_t> batch_new_leaves(B, 0);
//   std::vector<bool> batch_is_new(B, false);

//   int num_workers = 16;
//   if (impl_->with_pos_map) {
//       std::vector<std::thread> workers;
//       std::vector<std::vector<uint64_t>> thread_local_leaves(num_workers, std::vector<uint64_t>(B, UINT64_MAX));
      
//       for (size_t j = 0; j < B; ++j) {
//           batch_new_leaves[j] = impl_->GenerateLeaf();
//       }

//       for (int i = 0; i < num_workers; ++i) {
//           workers.emplace_back([this, i, num_workers, B, &blocks, &batch_new_leaves, &thread_local_leaves]() {
//               for (size_t pos = 1 + i; pos <= capacity_; pos += num_workers) {
//                   uint64_t current_pos = impl_->pos_map[pos];
//                   uint64_t next_pos = current_pos;
//                   for (size_t j = 0; j < B; ++j) {
//                       uint64_t k = blocks[j].meta_.key_;
//                       bool real = (!sn::obliv::ct_eq<uint64_t>(k, 0));
//                       bool match = sn::obliv::ct_eq<uint64_t>(pos, k) & real;
                      
//                       thread_local_leaves[i][j] = sn::obliv::ct_select<uint64_t>(current_pos, thread_local_leaves[i][j], match);
//                       next_pos = sn::obliv::ct_select<uint64_t>(batch_new_leaves[j], next_pos, match);
//                   }
//                   impl_->pos_map[pos] = next_pos;
//               }
//           });
//       }
//       for (auto& w : workers) w.join();
      
//       for (size_t j = 0; j < B; ++j) {
//           uint64_t global_found = UINT64_MAX;
//           for (int i = 0; i < num_workers; ++i) {
//               bool match = !sn::obliv::ct_eq<uint64_t>(thread_local_leaves[i][j], UINT64_MAX);
//               global_found = sn::obliv::ct_select<uint64_t>(thread_local_leaves[i][j], global_found, match);
//           }
//           batch_is_new[j] = sn::obliv::ct_eq<uint64_t>(global_found, UINT64_MAX);
//           bool has_leaf = !batch_is_new[j];
//           batch_cur_leaves[j] = sn::obliv::ct_select<uint64_t>(global_found, impl_->GenerateLeaf(), has_leaf);
//       }
//   } else {
//       for (size_t j = 0; j < B; ++j) {
//           uint64_t k = blocks[j].meta_.key_;
//           bool real = (!sn::obliv::ct_eq<uint64_t>(k, 0));
//           batch_new_leaves[j] = sn::obliv::ct_select<uint64_t>(k - 1, impl_->GenerateLeaf(), real);
//           batch_cur_leaves[j] = batch_new_leaves[j];
//       }
//   }

//   std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(16); });

//   std::vector<uint64_t> thread_access_ops(num_workers, 0);
//   size_t chunk_size = 16;
//   std::mutex ops_mutex;
//   std::condition_variable chunk_cv;

//   size_t batch_count = 0;
//   for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
//       size_t chunk_end = std::min(B, chunk_start + chunk_size);
//       int tasks_pending = num_workers;
      
//       for (int i = 0; i < num_workers; ++i) {
//           g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, &blocks, &batch_cur_leaves, &batch_new_leaves, &batch_is_new, &thread_access_ops, &ops_mutex, &tasks_pending, &chunk_cv]() {
//               try {
//                   thread_local SonicClient::access_scratch tl_scratch;
//                   thread_local size_t tl_scratch_cap = 0;
//                   if (tl_scratch_cap != capacity_) {
//                       impl_->client->configure_access_scratch(tl_scratch);
//                       tl_scratch_cap = capacity_;
//                   }
//                   uint64_t local_ops = 0;
//                   for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
//                       uint64_t k = blocks[j].meta_.key_;
//                       bool real = (!sn::obliv::ct_eq<uint64_t>(k, 0));
                      
//                       std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
//                       std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
                      
//                       size_t block_size = static_path_oram::BlockSize(val_len_);
//                       if (block_size <= kSonicBlockBytes) {
//                           auto meta_bytes = bytes::ToBytes(blocks[j].meta_);
//                           std::copy(meta_bytes.begin(), meta_bytes.end(), in_buf.begin());
//                           if (blocks[j].val_) {
//                               std::copy(blocks[j].val_.get(), blocks[j].val_.get() + val_len_, in_buf.data() + sizeof(static_path_oram::BlockMetadata));
//                           }
//                       }
                      
//                       auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
//                       if (batch_is_new[j] && real) {
//                           sn::oram::tree::block<kSonicBlockBytes> new_block{};
//                           new_block.address = k - 1;
//                           new_block.leaf_ix = batch_new_leaves[j];
//                           std::copy(in_buf.begin(), in_buf.end(), new_block.data.begin());
//                           {
//                               std::lock_guard<std::mutex> client_lock(ops_mutex);
//                               impl_->client->insert(new_block);
//                           }
//                       } else {
//                           // Clear pos_map if this is a real read-and-remove
//                           if (real && k > 0 && k <= capacity_) {
//                               impl_->pos_map[k] = UINT64_MAX;
//                           }
//                           sn::oram::access_request req;
//                           req.address = sn::obliv::ct_select<uint64_t>(k - 1, UINT64_MAX, real);
//                           req.cur_leaf = batch_cur_leaves[j];
//                           req.new_leaf = batch_new_leaves[j];
//                           req.is_write = sn::obliv::ct_select<bool>(true, false, real);
//                           req.in = sn::util::span<uint8_t>(in_buf);
//                           req.out = sn::util::span<uint8_t>(out_buf);
                          
//                           {
//                               std::lock_guard<std::mutex> client_lock(ops_mutex);
//                               impl_->client->access(req, tl_scratch);
//                           }
//                       }
//                       auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
//                       local_ops += (post_ops - pre_ops);
//                   }
//                   {
//                       std::unique_lock<std::mutex> lock(ops_mutex);
//                       thread_access_ops[i] += local_ops;
//                       tasks_pending--;
//                       if (tasks_pending == 0) chunk_cv.notify_one();
//                   }
//               } catch (const std::exception& e) {
//                   std::cerr << "[CRITICAL ERROR] Exception in InsertBatch pool thread: " << e.what() << std::endl;
//                   std::terminate();
//               }
//           });
//       }
//       std::unique_lock<std::mutex> lock(ops_mutex);
//       chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
      
//       batch_count += chunk_size;
//       if (batch_count + chunk_size > 1000) {
//           impl_->client->flush_epoch();
//           batch_count = 0;
//       }
//   }

//   if (batch_count > 0) {
//       impl_->client->flush_epoch();
//   }

//   for (int i = 0; i < num_workers; ++i) {
//       memory_access_count_ += thread_access_ops[i];
//       memory_bytes_moved_total_ += thread_access_ops[i] * kSonicBlockBytes * 2;
//   }

uint64_t SonicORamAdapter::GenerateRandomLeaf() const {
  return impl_->GenerateLeaf() + 1; // Return 1-indexed leaf for the caller
}

double SonicORamAdapter::RawSonicBenchmark(int work_type, size_t batch_size) {
    int num_workers = 24;
    size_t chunk_size = 384; 
    
    std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(24); });
    
    std::mutex ops_mutex;
    std::condition_variable chunk_cv;

    // Pad batch_size to be a multiple of chunk_size to prevent deadlocks in flush_epoch()
    if (batch_size % chunk_size != 0) {
        batch_size = ((batch_size / chunk_size) + 1) * chunk_size;
    }

    double total_ms = 0;

    for (size_t chunk_start = 0; chunk_start < batch_size; chunk_start += chunk_size) {
        size_t chunk_end = std::min(batch_size, chunk_start + chunk_size);
        int tasks_pending = num_workers;
        
        auto start_time = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < num_workers; ++i) {
            g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, work_type, &ops_mutex, &tasks_pending, &chunk_cv]() {
                thread_local SonicClient::access_scratch tl_scratch;
                impl_->client->configure_access_scratch(tl_scratch);
                
                std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
                std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);

                uint64_t num_leaves = 1ULL << impl_->client->shape().height;

                for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                    uint64_t fake_address = j % capacity_; 
                    uint64_t fake_cur_leaf = impl_->pos_map[fake_address + 1];
                    uint64_t fake_new_leaf = ((fake_address + 1) * 7331) % num_leaves;

                    if (fake_cur_leaf == UINT64_MAX) {
                        impl_->pos_map[fake_address + 1] = fake_new_leaf;
                        sn::oram::tree::block<kSonicBlockBytes> new_block{};
                        new_block.address = fake_address;
                        new_block.leaf_ix = fake_new_leaf;
                        impl_->client->insert(new_block);
                    } else {
                        impl_->pos_map[fake_address + 1] = fake_new_leaf;
                        sn::oram::access_request req;
                        req.address = fake_address;
                        req.cur_leaf = fake_cur_leaf;
                        req.new_leaf = fake_new_leaf;
                        req.is_write = (work_type == 0); 
                        req.in = sn::util::span<uint8_t>(in_buf);
                        req.out = sn::util::span<uint8_t>(out_buf);
                        impl_->client->access(req, tl_scratch);
                    }
                }
                
                {
                    std::unique_lock<std::mutex> lock(ops_mutex);
                    tasks_pending--;
                    if (tasks_pending == 0) chunk_cv.notify_one();
                }
            });
        }
        
        std::unique_lock<std::mutex> lock(ops_mutex);
        chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
        
        auto end_time = std::chrono::high_resolution_clock::now();
        total_ms += std::chrono::duration<double, std::milli>(end_time - start_time).count();

        impl_->client->flush_epoch();
    }

    return total_ms;
}



double SonicORamAdapter::SpinlockSonicBenchmark(int work_type, size_t batch_size, bool steady_state) {
    int num_workers = 24;
    size_t chunk_size = 384;
    
    if (batch_size % chunk_size != 0) {
        batch_size = ((batch_size / chunk_size) + 1) * chunk_size;
    }

    double total_ms = 0;
    
    // We use a barrier for num_workers + 1 (the main thread orchestrates)
    sn::threads::barrier sync_point(num_workers + 1);

    std::vector<std::thread> workers;
    for (int i = 0; i < num_workers; ++i) {
        workers.emplace_back([this, i, num_workers, chunk_size, batch_size, work_type, &sync_point]() {
            thread_local SonicClient::access_scratch tl_scratch;
            impl_->client->configure_access_scratch(tl_scratch);
            
            std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
            std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);

            uint64_t num_leaves = 1ULL << impl_->client->shape().height;

            for (size_t chunk_start = 0; chunk_start < batch_size; chunk_start += chunk_size) {
                size_t chunk_end = std::min(batch_size, chunk_start + chunk_size);
                
                // Wait for main thread to signal start of this chunk's timer
                sync_point.arrive_and_wait();

                for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                    uint64_t fake_address = j % capacity_; 
                    uint64_t fake_cur_leaf = impl_->pos_map[fake_address + 1];
                    uint64_t fake_new_leaf = ((fake_address + 1) * 7331) % num_leaves;

                    if (fake_cur_leaf == UINT64_MAX) {
                        impl_->pos_map[fake_address + 1] = fake_new_leaf;
                        sn::oram::tree::block<kSonicBlockBytes> new_block{};
                        new_block.address = fake_address;
                        new_block.leaf_ix = fake_new_leaf;
                        impl_->client->insert(new_block);
                    } else {
                        impl_->pos_map[fake_address + 1] = fake_new_leaf;
                        sn::oram::access_request req;
                        req.address = fake_address;
                        req.cur_leaf = fake_cur_leaf;
                        req.new_leaf = fake_new_leaf;
                        req.is_write = false;
                        req.in = sn::util::span<uint8_t>(in_buf);
                        req.out = sn::util::span<uint8_t>(out_buf);
                        impl_->client->access(req, tl_scratch);
                    }
                }
                
                // Signal main thread we are done with accesses
                sync_point.arrive_and_wait();
                
                // Wait for main thread to finish flush_epoch
                sync_point.arrive_and_wait();
            }
        });
    }

    for (size_t chunk_start = 0; chunk_start < batch_size; chunk_start += chunk_size) {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Release workers to start accesses
        sync_point.arrive_and_wait();
        
        // Wait for workers to finish accesses
        sync_point.arrive_and_wait();
        
        if (!steady_state) {
            auto end_time = std::chrono::high_resolution_clock::now();
            total_ms += std::chrono::duration<double, std::milli>(end_time - start_time).count();
        }

        // Perform evictions
        impl_->client->flush_epoch();
        
        if (steady_state) {
            auto end_time = std::chrono::high_resolution_clock::now();
            total_ms += std::chrono::duration<double, std::milli>(end_time - start_time).count();
        }
        
        // Release workers for next chunk
        sync_point.arrive_and_wait();
    }

    for (auto& t : workers) {
        t.join();
    }

    return total_ms;
}

} // namespace dyno::dynamic_stepping_path_oram
