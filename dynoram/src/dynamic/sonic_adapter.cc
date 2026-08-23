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

// Global thread pool for access workers to prevent std::system_error on rapid thread creation
static std::unique_ptr<ThreadPool> g_access_pool = nullptr;
static std::once_flag g_pool_init_flag;


using SonicTraits = sn::oram::zingoram::traits<kSonicBlockBytes, sn::oram::zingoram::epoch_mode::disjoint_epoch, sn::oram::zingoram::storage::slab_store>;
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
    
    // Create 16 eviction threads to handle the massive forest parallelism
    eviction_pool = std::make_unique<sn::threads::pthread_thread_pool>(thread_ctx, 0, "oram-evict");
    eviction_team = std::make_unique<sn::threads::thread_team>(eviction_pool->pool(), 16);

    SonicTraits::options_t opts{};
    opts.block_count = capacity + 1;
    opts.bucket_real_size = 5;
    opts.bucket_dummy_size = 7;
    opts.eviction_rate = 2; // Lock base eviction rate to 2
    
    // Configure SONIC Batch Parallelism to yield num_pathreads = 16
    // num_pathreads = eviction_rate * (2^routing_depth) * evict_batch
    // 16 = 2 * (2^1) * 4
    opts.routing_depth = 1; // 2 parallel subtrees
    opts.evict_batch = 4; 
    opts.access_concurrency = 16;
    opts.disjoint_epoch_window = 16; // Optimized to match thread count and prevent massive padding on small batches

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
    uint64_t safe_k = sn::obliv::ct_select<uint64_t>(k, 0, k < impl_->pos_map.size());
    uint64_t found_leaf = impl_->pos_map[safe_k];
    impl_->pos_map[safe_k] = sn::obliv::ct_select<uint64_t>(new_leaf, impl_->pos_map[safe_k], is_real);
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

static_path_oram::Block SonicORamAdapter::Read(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real, bool flush) {
  uint64_t cur_leaf = sn::obliv::ct_select<uint64_t>(p - 1, 0, is_real);
  uint64_t new_leaf = impl_->GenerateLeaf();

  if (impl_->with_pos_map) {
    uint64_t safe_k = sn::obliv::ct_select<uint64_t>(k, 0, k < impl_->pos_map.size());
    uint64_t found_leaf = impl_->pos_map[safe_k];
    impl_->pos_map[safe_k] = sn::obliv::ct_select<uint64_t>(new_leaf, impl_->pos_map[safe_k], is_real);
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

void SonicORamAdapter::Insert(static_path_oram::Block block, crypto::Key enc_key, bool is_real, bool is_new, bool flush) {
  uint64_t k = block.meta_.key_;
  bool real = is_real & !sn::obliv::ct_eq<uint64_t>(k, 0);
  uint64_t cur_leaf = 0;
  uint64_t write_leaf = 0;
  if (impl_->with_pos_map) {
    bool has_leaf = false;
    uint64_t safe_k = sn::obliv::ct_select<uint64_t>(k, 0, k < impl_->pos_map.size());
    write_leaf = impl_->GenerateLeaf();
    uint64_t found_leaf = impl_->pos_map[safe_k];
    impl_->pos_map[safe_k] = sn::obliv::ct_select<uint64_t>(write_leaf, impl_->pos_map[safe_k], real);
    has_leaf = !sn::obliv::ct_eq<uint64_t>(found_leaf, UINT64_MAX);
    cur_leaf = sn::obliv::ct_select<uint64_t>(found_leaf, impl_->GenerateLeaf(), has_leaf);
    
    // If it was not in the pos_map, it is a brand new bucket.
    if (found_leaf == UINT64_MAX) {
      is_new = true;
    }
  } else {
    uint64_t r_leaf = impl_->GenerateLeaf();
    write_leaf = sn::obliv::ct_select<uint64_t>(block.meta_.pos_ - 1, r_leaf, real);
    cur_leaf = write_leaf;
  }
  
  sn::oram::access_request req;
  req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, real);
  req.cur_leaf = cur_leaf;
  req.new_leaf = write_leaf;
  req.is_write = sn::obliv::ct_select<bool>(true, false, real); 
  
  std::vector<uint8_t> in_buf(128, 0);
  std::vector<uint8_t> out_buf(128, 0);
  
  size_t block_size = static_path_oram::BlockSize(val_len_);
  if (block_size <= 128) {
      block.ToBytes(val_len_, in_buf.data());
  }
  req.in = sn::util::span<uint8_t>(in_buf);
  req.out = sn::util::span<uint8_t>(out_buf);

  thread_local SonicClient::access_scratch tl_scratch;
  thread_local size_t tl_scratch_cap = 0;
  if (tl_scratch_cap != capacity_) {
      impl_->client->configure_access_scratch(tl_scratch);
      tl_scratch_cap = capacity_;
  }

  auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  if (is_new && real) {
    sn::oram::tree::block<128> new_block{};
    new_block.address = k - 1;
    new_block.leaf_ix = write_leaf;
    std::copy(in_buf.begin(), in_buf.end(), new_block.data.begin());
    impl_->client->insert(new_block);
  } else {
    req.is_write = sn::obliv::ct_select<bool>(true, false, real);
    req.in = sn::util::span<uint8_t>(in_buf);
    req.out = sn::util::span<uint8_t>(out_buf);
    impl_->client->access(req, tl_scratch);
  }
  if (flush) impl_->client->flush_epoch();
  auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
  memory_access_count_ += (post_ops - pre_ops);
  memory_bytes_moved_total_ += (post_ops - pre_ops) * 128 * 2;
}

void SonicORamAdapter::FlushEpoch() {
  impl_->client->flush_epoch();
}

std::vector<static_path_oram::Block> SonicORamAdapter::ReadAndRemoveBatch(const std::vector<std::pair<static_path_oram::Key, bool>>& keys_with_real_flags, crypto::Key enc_key) {
  size_t B = keys_with_real_flags.size();
  std::vector<static_path_oram::Block> results;
  results.reserve(B);
  for (size_t i = 0; i < B; ++i) {
    results.emplace_back(true);
  }

  std::vector<uint64_t> batch_cur_leaves(B, UINT64_MAX);
  std::vector<uint64_t> batch_new_leaves(B, 0);
  
  for (size_t j = 0; j < B; ++j) {
      batch_new_leaves[j] = impl_->GenerateLeaf();
  }

  int num_workers = 16;
  if (impl_->with_pos_map) {
      std::vector<std::thread> workers;
      std::vector<std::vector<uint64_t>> thread_local_leaves(num_workers, std::vector<uint64_t>(B, UINT64_MAX));
      
      for (int i = 0; i < num_workers; ++i) {
          workers.emplace_back([this, i, num_workers, B, &keys_with_real_flags, &batch_new_leaves, &thread_local_leaves]() {
              for (size_t pos = 1 + i; pos <= capacity_; pos += num_workers) {
                  uint64_t current_pos = impl_->pos_map[pos];
                  uint64_t next_pos = current_pos;
                  for (size_t j = 0; j < B; ++j) {
                      auto& [k, is_real] = keys_with_real_flags[j];
                      bool match = sn::obliv::ct_eq<uint64_t>(pos, k) & is_real;
                      thread_local_leaves[i][j] = sn::obliv::ct_select<uint64_t>(current_pos, thread_local_leaves[i][j], match);
                      next_pos = sn::obliv::ct_select<uint64_t>(batch_new_leaves[j], next_pos, match);
                  }
                  impl_->pos_map[pos] = next_pos;
              }
          });
      }
      for (auto& w : workers) w.join();
      
      for (size_t j = 0; j < B; ++j) {
          uint64_t global_found = UINT64_MAX;
          for (int i = 0; i < num_workers; ++i) {
              bool match = !sn::obliv::ct_eq<uint64_t>(thread_local_leaves[i][j], UINT64_MAX);
              global_found = sn::obliv::ct_select<uint64_t>(thread_local_leaves[i][j], global_found, match);
          }
          bool has_leaf = !sn::obliv::ct_eq<uint64_t>(global_found, UINT64_MAX);
          batch_cur_leaves[j] = sn::obliv::ct_select<uint64_t>(global_found, impl_->GenerateLeaf(), has_leaf);
      }
  } else {
      for (size_t j = 0; j < B; ++j) {
          batch_cur_leaves[j] = sn::obliv::ct_select<uint64_t>(keys_with_real_flags[j].first - 1, impl_->GenerateLeaf(), keys_with_real_flags[j].second);
      }
  }

  std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(16); });

  std::vector<uint64_t> thread_access_ops(num_workers, 0);
  size_t chunk_size = 16;
  std::mutex ops_mutex;
  std::condition_variable chunk_cv;

  for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
      size_t chunk_end = std::min(B, chunk_start + chunk_size);
      int tasks_pending = num_workers;
      
      for (int i = 0; i < num_workers; ++i) {
          g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, &keys_with_real_flags, &batch_cur_leaves, &batch_new_leaves, &results, &thread_access_ops, &ops_mutex, &tasks_pending, &chunk_cv]() {
              try {
                  thread_local SonicClient::access_scratch tl_scratch;
                  thread_local size_t tl_scratch_cap = 0;
                  if (tl_scratch_cap != capacity_) {
                      impl_->client->configure_access_scratch(tl_scratch);
                      tl_scratch_cap = capacity_;
                  }
                  uint64_t local_ops = 0;
                  for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                      auto& [k, is_real] = keys_with_real_flags[j];
                      sn::oram::access_request req;
                      req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, is_real);
                      req.cur_leaf = batch_cur_leaves[j];
                      req.new_leaf = batch_new_leaves[j];
                      req.is_write = false; 
                      
                      std::vector<uint8_t> in_buf(128, 0);
                      std::vector<uint8_t> out_buf(128, 0);
                      req.in = sn::util::span<uint8_t>(in_buf);
                      req.out = sn::util::span<uint8_t>(out_buf);

                      auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      impl_->client->access(req, tl_scratch);
                      auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      local_ops += (post_ops - pre_ops);

                      static_path_oram::Block res(true);
                      res.val_ = std::make_unique<uint8_t[]>(val_len_);
                      size_t block_size = static_path_oram::BlockSize(val_len_);
                      if (block_size <= 128) {
                          bytes::FromBytes(out_buf.data(), res.meta_);
                          std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                                    out_buf.data() + sizeof(static_path_oram::BlockMetadata) + val_len_,
                                    res.val_.get());
                      }
                      
                      if (!impl_->with_pos_map) {
                          res.meta_.pos_ = batch_new_leaves[j] + 1;
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
                  std::cerr << "[CRITICAL ERROR] Exception in ReadAndRemoveBatch pool thread: " << e.what() << std::endl;
                  std::terminate();
              }
          });
      }
      
      std::unique_lock<std::mutex> lock(ops_mutex);
      chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
      impl_->client->flush_epoch();
  }

  for (int i = 0; i < num_workers; ++i) {
      memory_access_count_ += thread_access_ops[i];
      memory_bytes_moved_total_ += thread_access_ops[i] * 128 * 2;
  }

  return results;
}

std::vector<static_path_oram::Block> SonicORamAdapter::ReadBatch(const std::vector<std::pair<static_path_oram::Key, bool>>& keys_with_real_flags, crypto::Key enc_key) {
  size_t B = keys_with_real_flags.size();
  std::vector<static_path_oram::Block> results;
  results.reserve(B);
  for (size_t i = 0; i < B; ++i) {
    results.emplace_back(true);
  }

  std::vector<uint64_t> batch_cur_leaves(B, UINT64_MAX);
  std::vector<uint64_t> batch_new_leaves(B, 0);
  
  for (size_t j = 0; j < B; ++j) {
      batch_new_leaves[j] = impl_->GenerateLeaf();
  }

  int num_workers = 16;
  if (impl_->with_pos_map) {
      std::vector<std::thread> workers;
      std::vector<std::vector<uint64_t>> thread_local_leaves(num_workers, std::vector<uint64_t>(B, UINT64_MAX));
      
      for (int i = 0; i < num_workers; ++i) {
          workers.emplace_back([this, i, num_workers, B, &keys_with_real_flags, &batch_new_leaves, &thread_local_leaves]() {
              for (size_t pos = 1 + i; pos <= capacity_; pos += num_workers) {
                  uint64_t current_pos = impl_->pos_map[pos];
                  uint64_t next_pos = current_pos;
                  for (size_t j = 0; j < B; ++j) {
                      auto& [k, is_real] = keys_with_real_flags[j];
                      bool match = sn::obliv::ct_eq<uint64_t>(pos, k) & is_real;
                      thread_local_leaves[i][j] = sn::obliv::ct_select<uint64_t>(current_pos, thread_local_leaves[i][j], match);
                      next_pos = sn::obliv::ct_select<uint64_t>(batch_new_leaves[j], next_pos, match);
                  }
                  impl_->pos_map[pos] = next_pos;
              }
          });
      }
      for (auto& w : workers) w.join();
      
      for (size_t j = 0; j < B; ++j) {
          uint64_t global_found = UINT64_MAX;
          for (int i = 0; i < num_workers; ++i) {
              bool match = !sn::obliv::ct_eq<uint64_t>(thread_local_leaves[i][j], UINT64_MAX);
              global_found = sn::obliv::ct_select<uint64_t>(thread_local_leaves[i][j], global_found, match);
          }
          bool has_leaf = !sn::obliv::ct_eq<uint64_t>(global_found, UINT64_MAX);
          batch_cur_leaves[j] = sn::obliv::ct_select<uint64_t>(global_found, impl_->GenerateLeaf(), has_leaf);
      }
  } else {
      for (size_t j = 0; j < B; ++j) {
          batch_cur_leaves[j] = sn::obliv::ct_select<uint64_t>(keys_with_real_flags[j].first - 1, impl_->GenerateLeaf(), keys_with_real_flags[j].second);
      }
  }

  std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(16); });

  std::vector<uint64_t> thread_access_ops(num_workers, 0);
  size_t chunk_size = 16;
  std::mutex ops_mutex;
  std::condition_variable chunk_cv;

  for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
      size_t chunk_end = std::min(B, chunk_start + chunk_size);
      int tasks_pending = num_workers;
      
      for (int i = 0; i < num_workers; ++i) {
          g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, &keys_with_real_flags, &batch_cur_leaves, &batch_new_leaves, &results, &thread_access_ops, &ops_mutex, &tasks_pending, &chunk_cv]() {
              try {
                  thread_local SonicClient::access_scratch tl_scratch;
                  thread_local size_t tl_scratch_cap = 0;
                  if (tl_scratch_cap != capacity_) {
                      impl_->client->configure_access_scratch(tl_scratch);
                      tl_scratch_cap = capacity_;
                  }
                  uint64_t local_ops = 0;
                  for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                      auto& [k, is_real] = keys_with_real_flags[j];
                      sn::oram::access_request req;
                      req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, is_real);
                      req.cur_leaf = batch_cur_leaves[j];
                      req.new_leaf = batch_new_leaves[j];
                      req.is_write = false; 
                      
                      std::vector<uint8_t> in_buf(128, 0);
                      std::vector<uint8_t> out_buf(128, 0);
                      req.in = sn::util::span<uint8_t>(in_buf);
                      req.out = sn::util::span<uint8_t>(out_buf);

                      auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      impl_->client->access(req, tl_scratch);
                      auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      local_ops += (post_ops - pre_ops);

                      static_path_oram::Block res(true);
                      res.val_ = std::make_unique<uint8_t[]>(val_len_);
                      size_t block_size = static_path_oram::BlockSize(val_len_);
                      if (block_size <= 128) {
                          bytes::FromBytes(out_buf.data(), res.meta_);
                          std::copy(out_buf.data() + sizeof(static_path_oram::BlockMetadata),
                                    out_buf.data() + sizeof(static_path_oram::BlockMetadata) + val_len_,
                                    res.val_.get());
                      }
                      if (!impl_->with_pos_map) {
                          res.meta_.pos_ = batch_new_leaves[j] + 1;
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
                  std::cerr << "[CRITICAL ERROR] Exception in ReadBatch pool thread: " << e.what() << std::endl;
                  std::terminate();
              }
          });
      }
      std::unique_lock<std::mutex> lock(ops_mutex);
      chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
      impl_->client->flush_epoch();
  }
  return results;
}

void SonicORamAdapter::InsertBatch(std::vector<static_path_oram::Block>& blocks, crypto::Key enc_key) {
  size_t B = blocks.size();
  std::vector<uint64_t> batch_cur_leaves(B, 0);
  std::vector<uint64_t> batch_new_leaves(B, 0);
  std::vector<bool> batch_is_new(B, false);

  int num_workers = 16;
  if (impl_->with_pos_map) {
      std::vector<std::thread> workers;
      std::vector<std::vector<uint64_t>> thread_local_leaves(num_workers, std::vector<uint64_t>(B, UINT64_MAX));
      
      for (size_t j = 0; j < B; ++j) {
          batch_new_leaves[j] = impl_->GenerateLeaf();
      }

      for (int i = 0; i < num_workers; ++i) {
          workers.emplace_back([this, i, num_workers, B, &blocks, &batch_new_leaves, &thread_local_leaves]() {
              for (size_t pos = 1 + i; pos <= capacity_; pos += num_workers) {
                  uint64_t current_pos = impl_->pos_map[pos];
                  uint64_t next_pos = current_pos;
                  for (size_t j = 0; j < B; ++j) {
                      uint64_t k = blocks[j].meta_.key_;
                      bool real = (!sn::obliv::ct_eq<uint64_t>(k, 0));
                      bool match = sn::obliv::ct_eq<uint64_t>(pos, k) & real;
                      
                      thread_local_leaves[i][j] = sn::obliv::ct_select<uint64_t>(current_pos, thread_local_leaves[i][j], match);
                      next_pos = sn::obliv::ct_select<uint64_t>(batch_new_leaves[j], next_pos, match);
                  }
                  impl_->pos_map[pos] = next_pos;
              }
          });
      }
      for (auto& w : workers) w.join();
      
      for (size_t j = 0; j < B; ++j) {
          uint64_t global_found = UINT64_MAX;
          for (int i = 0; i < num_workers; ++i) {
              bool match = !sn::obliv::ct_eq<uint64_t>(thread_local_leaves[i][j], UINT64_MAX);
              global_found = sn::obliv::ct_select<uint64_t>(thread_local_leaves[i][j], global_found, match);
          }
          batch_is_new[j] = sn::obliv::ct_eq<uint64_t>(global_found, UINT64_MAX);
          bool has_leaf = !batch_is_new[j];
          batch_cur_leaves[j] = sn::obliv::ct_select<uint64_t>(global_found, impl_->GenerateLeaf(), has_leaf);
      }
  } else {
      for (size_t j = 0; j < B; ++j) {
          uint64_t k = blocks[j].meta_.key_;
          bool real = (!sn::obliv::ct_eq<uint64_t>(k, 0));
          batch_new_leaves[j] = sn::obliv::ct_select<uint64_t>(blocks[j].meta_.pos_ - 1, impl_->GenerateLeaf(), real);
          batch_cur_leaves[j] = batch_new_leaves[j];
      }
  }

  std::call_once(g_pool_init_flag, [](){ g_access_pool = std::make_unique<ThreadPool>(16); });

  std::vector<uint64_t> thread_access_ops(num_workers, 0);
  size_t chunk_size = 16;
  std::mutex ops_mutex;
  std::condition_variable chunk_cv;

  for (size_t chunk_start = 0; chunk_start < B; chunk_start += chunk_size) {
      size_t chunk_end = std::min(B, chunk_start + chunk_size);
      int tasks_pending = num_workers;
      
      for (int i = 0; i < num_workers; ++i) {
          g_access_pool->enqueue([this, i, num_workers, chunk_start, chunk_end, &blocks, &batch_cur_leaves, &batch_new_leaves, &batch_is_new, &thread_access_ops, &ops_mutex, &tasks_pending, &chunk_cv]() {
              try {
                  thread_local SonicClient::access_scratch tl_scratch;
                  thread_local size_t tl_scratch_cap = 0;
                  if (tl_scratch_cap != capacity_) {
                      impl_->client->configure_access_scratch(tl_scratch);
                      tl_scratch_cap = capacity_;
                  }
                  uint64_t local_ops = 0;
                  for (size_t j = chunk_start + i; j < chunk_end; j += num_workers) {
                      uint64_t k = blocks[j].meta_.key_;
                      bool real = (!sn::obliv::ct_eq<uint64_t>(k, 0));
                      
                      std::vector<uint8_t> in_buf(kSonicBlockBytes, 0);
                      std::vector<uint8_t> out_buf(kSonicBlockBytes, 0);
                      size_t block_size = static_path_oram::BlockSize(val_len_);
                      if (block_size <= kSonicBlockBytes) {
                          blocks[j].ToBytes(val_len_, in_buf.data());
                      }
                      
                      auto pre_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      if (batch_is_new[j] && real) {
                          sn::oram::tree::block<kSonicBlockBytes> new_block{};
                          new_block.address = k - 1;
                          new_block.leaf_ix = batch_new_leaves[j];
                          std::copy(in_buf.begin(), in_buf.end(), new_block.data.begin());
                          impl_->client->insert(new_block);
                      } else {
                          sn::oram::access_request req;
                          req.address = sn::obliv::ct_select<uint64_t>(k - 1, capacity_, real);
                          req.cur_leaf = batch_cur_leaves[j];
                          req.new_leaf = batch_new_leaves[j];
                          req.is_write = sn::obliv::ct_select<bool>(true, false, real);
                          req.in = sn::util::span<uint8_t>(in_buf);
                          req.out = sn::util::span<uint8_t>(out_buf);
                          impl_->client->access(req, tl_scratch);
                      }
                      auto post_ops = impl_->client->state_ref().metrics_snapshot().access_ops;
                      local_ops += (post_ops - pre_ops);
                  }
                  {
                      std::unique_lock<std::mutex> lock(ops_mutex);
                      thread_access_ops[i] += local_ops;
                      tasks_pending--;
                      if (tasks_pending == 0) chunk_cv.notify_one();
                  }
              } catch (const std::exception& e) {
                  std::cerr << "[CRITICAL ERROR] Exception in InsertBatch pool thread: " << e.what() << std::endl;
                  std::terminate();
              }
          });
      }
      std::unique_lock<std::mutex> lock(ops_mutex);
      chunk_cv.wait(lock, [&tasks_pending]{ return tasks_pending == 0; });
      impl_->client->flush_epoch();
  }

  for (int i = 0; i < num_workers; ++i) {
      memory_access_count_ += thread_access_ops[i];
      memory_bytes_moved_total_ += thread_access_ops[i] * kSonicBlockBytes * 2;
  }


}

uint64_t SonicORamAdapter::GenerateRandomLeaf() const {
  return impl_->GenerateLeaf() + 1; // Return 1-indexed leaf for the caller
}

} // namespace dyno::dynamic_stepping_path_oram
