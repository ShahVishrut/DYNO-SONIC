#include "oram.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <future>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <immintrin.h>
#include <random>

#include "src/utils/crypto.h"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include <mutex>
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/omap/o2th/client.hpp"

namespace dyno::dynamic_stepping_path_oram {

static sn::threads::thread_context g_thread_ctx{sn::threads::thread_policy{.affinity = sn::threads::thread_affinity::inherit}};
static std::unique_ptr<sn::threads::pthread_thread_pool> g_oram_pool = nullptr;
static std::once_flag g_oram_pool_init;

ORam::ORam(int starting_size_power_of_two, size_t val_len)
    : capacity_(1UL << starting_size_power_of_two),
      val_len_(val_len),
      size_(1UL << (starting_size_power_of_two)) {
  auto base_cap = capacity_;
  sub_orams_[0] = std::make_unique<PORam>(base_cap, val_len, false);
  sub_orams_[1] = std::make_unique<PORam>(base_cap * 2, val_len, false);
  log_map_[0].resize(base_cap + 1, {0, 0});
  log_map_[1].resize((base_cap * 2) + 1, {0, 0});
}

bool IsPowerOfTwo(size_t x) {
  return !(x & (x - 1));
}

namespace {

void ObliviousSwapBlock(PORamBlock& a, PORamBlock& b, bool cond, size_t val_len) {
    sn::obliv::ct_swap(&a.meta_.key_, &b.meta_.key_, cond);
    sn::obliv::ct_swap(&a.meta_.pos_, &b.meta_.pos_, cond);
    for (size_t i = 0; i < val_len; ++i) {
        sn::obliv::ct_swap(&a.val_.get()[i], &b.val_.get()[i], cond);
    }
}

void ObliviousMergeHalves(std::vector<PORamBlock>& A, size_t start, size_t length, size_t val_len) {
    size_t step = length / 2;
    while (step > 0) {
        for (size_t i = start; i < start + length - step; ++i) {
            bool left_dummy = sn::obliv::ct_eq<uint64_t>(A[i].meta_.key_, 0);
            bool right_real = !sn::obliv::ct_eq<uint64_t>(A[i + step].meta_.key_, 0);
            bool cond = left_dummy & right_real;
            ObliviousSwapBlock(A[i], A[i + step], cond, val_len);
        }
        step /= 2;
    }
}

void OCompact(std::vector<PORamBlock>& A, size_t start, size_t length, size_t val_len) {
    if (length <= 1) return;
    size_t half = length / 2;
    OCompact(A, start, half, val_len);
    OCompact(A, start + half, half, val_len);
    ObliviousMergeHalves(A, start, length, val_len);
}

} // namespace

void ORam::Grow(crypto::Key enc_key) {
  if (capacity_ == 0) {
    sub_orams_[1] = std::make_unique<PORam>(1, val_len_, true);
    ++capacity_;
    return;
  }

  if (IsPowerOfTwo(capacity_)) {
    assert(sub_orams_[1] != nullptr);
    sub_orams_[0] = std::move(sub_orams_[1]);
    sub_orams_[1] = std::make_unique<PORam>(2 * capacity_, val_len_, true);
    
    log_map_[0] = std::move(log_map_[1]);
    log_map_[1].clear();
    log_map_[1].resize((2 * capacity_) + 1, {0, 0});
  }

  assert(sub_orams_[0] != nullptr && sub_orams_[1] != nullptr);
  Key move_idx = (capacity_ % sub_orams_[0]->Capacity()) + 1;
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  auto move_bl = sub_orams_[0]->ReadAndRemove(0, move_idx, enc_key, true);
  
  bool is_real = (move_bl.meta_.key_ != 0);
  sub_orams_[1]->Insert(std::move(move_bl), enc_key, is_real);
  
  memory_access_count_ += SubORamsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubORamsMemoryBytesMovedTotalSum() - start_bytes;
  ++capacity_;
}

Block ORam::ReadAndRemove(Key k, crypto::Key enc_key) {
  assert(1 <= k && k <= capacity_);
  Block res;
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  
  uint64_t cap_S = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  uint64_t cap_L = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;
  
  for (int i = 0; i < 2; ++i) {
    if (i == 0 && (sub_orams_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
      
    uint64_t cap = (i == 0) ? cap_S : cap_L;
    uint64_t phys_k = 0;
    for (uint64_t j = 1; j <= cap; ++j) {
        bool match = sn::obliv::ct_eq(static_cast<uint64_t>(k), log_map_[i][j].logical_key);
        phys_k = sn::obliv::ct_select<uint64_t>(j, phys_k, match);
        log_map_[i][j].logical_key = sn::obliv::ct_select<uint64_t>(0, log_map_[i][j].logical_key, match);
        log_map_[i][j].leaf = sn::obliv::ct_select<uint64_t>(0, log_map_[i][j].leaf, match);
    }
    
    bool is_real = (phys_k != 0);
    phys_k = sn::obliv::ct_select<uint64_t>(phys_k, 1, is_real); // Dummy access to slot 1 if not found
    auto bl = sub_orams_[i]->ReadAndRemove(0, phys_k, enc_key, is_real);
    
    res.key_ = sn::obliv::ct_select<Key>(k, res.key_, is_real && (bl.meta_.key_ != 0));
    if (bl.val_) {
        sn::obliv::ct_select_array(res.val_.get(), bl.val_.get(), res.val_.get(), val_len_, is_real);
    }
  }
  bool found = (res.key_ != 0);
  size_ -= sn::obliv::ct_select<size_t>(1, 0, found);
  memory_access_count_ += SubORamsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubORamsMemoryBytesMovedTotalSum() - start_bytes;
  return res;
}

Block ORam::Read(Key k, crypto::Key enc_key) {
  assert(1 <= k && k <= capacity_);
  Block res;
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  
  uint64_t cap_S = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  uint64_t cap_L = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;
  
  for (int i = 0; i < 2; ++i) {
    if (i == 0 && (sub_orams_[i] == nullptr))
      continue;
      
    uint64_t cap = (i == 0) ? cap_S : cap_L;
    uint64_t phys_k = 0;
    for (uint64_t j = 1; j <= cap; ++j) {
        bool match = sn::obliv::ct_eq(static_cast<uint64_t>(k), log_map_[i][j].logical_key);
        phys_k = sn::obliv::ct_select<uint64_t>(j, phys_k, match);
    }
    
    bool is_real = (phys_k != 0);
    phys_k = sn::obliv::ct_select<uint64_t>(phys_k, 1, is_real);
    auto bl = sub_orams_[i]->Read(0, phys_k, enc_key, is_real);
    
    res.key_ = sn::obliv::ct_select<Key>(k, res.key_, is_real && (bl.meta_.key_ != 0));
    if (bl.val_) {
        sn::obliv::ct_select_array(res.val_.get(), bl.val_.get(), res.val_.get(), val_len_, is_real);
    }
  }
  memory_access_count_ += SubORamsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubORamsMemoryBytesMovedTotalSum() - start_bytes;
  return res;
}

void ORam::Insert(Key k, Val v, crypto::Key enc_key) {
  assert(1 <= k && k <= capacity_);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  
  uint64_t cap_L = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;
  
  uint64_t phys_k = 0;
  for (uint64_t i = 1; i <= cap_L; ++i) {
      bool is_empty = (log_map_[1][i].logical_key == 0);
      bool select_this = is_empty && (phys_k == 0);
      phys_k = sn::obliv::ct_select<uint64_t>(i, phys_k, select_this);
      log_map_[1][i].logical_key = sn::obliv::ct_select<uint64_t>(k, log_map_[1][i].logical_key, select_this);
      log_map_[1][i].leaf = sn::obliv::ct_select<uint64_t>(0, log_map_[1][i].leaf, select_this);
  }
  
  for (int i = 0; i < 2; ++i) {
    if (sub_orams_[i] == nullptr) continue;
    bool is_real = (i == 1);
    uint64_t target_phys_k = sn::obliv::ct_select<uint64_t>(phys_k, 1, is_real);
    
    static_path_oram::Block b(static_cast<uint32_t>(0), static_cast<uint32_t>(target_phys_k));
    if (v) {
        b.val_ = std::make_unique<uint8_t[]>(val_len_);
        std::copy_n(v.get(), val_len_, b.val_.get());
    }
    sub_orams_[i]->Insert(std::move(b), enc_key, is_real);
  }
  
  ++size_;
  memory_access_count_ += SubORamsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubORamsMemoryBytesMovedTotalSum() - start_bytes;
}

uint64_t ORam::SubORamsMemoryAccessCountSum() {
  uint64_t res = 0;
  for (auto &so : sub_orams_) {
    if (so != nullptr) {
      res += so->MemoryAccessCount();
    }
  }
  return res;
}

uint64_t ORam::SubORamsMemoryBytesMovedTotalSum() {
  uint64_t res = 0;
  for (auto &so : sub_orams_) {
    if (so != nullptr) {
      res += so->MemoryBytesMovedTotal();
    }
  }
  return res;
}

void ORam::ExecuteBatch(std::vector<BatchOperation>& batch, crypto::Key enc_key, bool steady_state) {
  size_t original_B = batch.size();
  if (original_B == 0) return;

  size_t B = 64;
  while (B < original_B) B *= 2;
  
  if (B > original_B) {
      for (size_t i = original_B; i < B; ++i) {
          BatchOperation dummy;
          dummy.type = OpType::Search;
          dummy.key = 0;
          batch.push_back(std::move(dummy));
      }
  }

  auto t_start = std::chrono::high_resolution_clock::now();
  auto get_ms = [&t_start]() {
      auto now = std::chrono::high_resolution_clock::now();
      double ms = std::chrono::duration<double, std::milli>(now - t_start).count();
      t_start = now;
      return ms;
  };

  size_t original_inserts = 0;
  size_t original_deletes = 0;
  for (auto& op : batch) {
    if (op.type == OpType::Insert) {
        original_inserts++;
    } else if (op.type == OpType::Delete) {
        original_deletes++;
    }
  }

  // Pre-allocate values so we can obliviously swap them in constant time
  for (auto& op : batch) {
    if (!op.val && val_len_ > 0) {
      op.val = std::make_unique<uint8_t[]>(val_len_);
    }
    // Pre-allocate result buffers here, BEFORE the bitonic sort shuffles the secrets!
    if (!op.result.val_ && val_len_ > 0) {
      op.result.val_ = std::make_unique<uint8_t[]>(val_len_);
      std::fill(op.result.val_.get(), op.result.val_.get() + val_len_, 0);
    }
  }

  struct alignas(8) OblivElem {
    Key key;
    uint32_t seq;
    bool is_dummy;
    uint8_t op_type; // 0=Insert, 1=Search, 2=Delete, 3=Update
  };
  
  uint64_t cap_S = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  uint64_t cap_L = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;
  
  std::vector<OblivElem> elems(B);
  for (size_t i = 0; i < B; ++i) {
    if (i < original_B) {
        elems[i].key = batch[i].key;
        elems[i].seq = static_cast<uint32_t>(i);
        elems[i].is_dummy = false;
        elems[i].op_type = static_cast<uint8_t>(batch[i].type);
    } else {
        elems[i].key = 0;
        elems[i].seq = static_cast<uint32_t>(i);
        elems[i].is_dummy = true;
        elems[i].op_type = 255;
    }
  }

  struct BatchSwapHook {
    std::vector<BatchOperation>& batch_ref;
    size_t val_len;
    OblivElem* elems_base;

    void operator()(OblivElem* a, OblivElem* b, bool cond) const {
      size_t idx_a = static_cast<size_t>(a - elems_base);
      size_t idx_b = static_cast<size_t>(b - elems_base);

      uint8_t type_a = static_cast<uint8_t>(batch_ref[idx_a].type);
      uint8_t type_b = static_cast<uint8_t>(batch_ref[idx_b].type);
      sn::obliv::ct_swap(&type_a, &type_b, cond);
      batch_ref[idx_a].type = static_cast<OpType>(type_a);
      batch_ref[idx_b].type = static_cast<OpType>(type_b);

      sn::obliv::ct_swap(&batch_ref[idx_a].key, &batch_ref[idx_b].key, cond);
      
      if (val_len > 0) {
        sn::obliv::ct_swap_array(batch_ref[idx_a].val.get(), batch_ref[idx_b].val.get(), val_len, cond);
      }
    }
  };

  BatchSwapHook hook{batch, val_len_, elems.data()};
  auto key_ext = [](const OblivElem& e) { return e; };

  std::cout << "[DYNO] Phase 1: O-Sort (Group by Key) starting... B=" << B << std::endl;
  // Phase 1: O-Sort (Group by Key, then Seq)
  auto comp1 = [](const OblivElem& a, const OblivElem& b) {
    bool key_eq = sn::obliv::ct_eq(a.key, b.key);
    bool key_lt = sn::obliv::ct_lt(a.key, b.key);
    bool seq_lt = sn::obliv::ct_lt(a.seq, b.seq);
    return sn::obliv::ct_select(seq_lt, key_lt, key_eq);
  };
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(elems.data(), B, key_ext, comp1, hook);

  // After bitonic sort, batch is co-sorted with elems via the hook.
  // Reset seq to current position so batch[elems[i].seq] == batch[i] stays correct
  // even after the later std::sort reorders elems without touching batch.
  for (size_t i = 0; i < B; ++i) {
      elems[i].seq = static_cast<uint32_t>(i);
  }

  std::cout << "[DYNO] Phase 2: Pre-Phase LogMap Scan starting..." << std::endl;
  // Pre-Phase: Oblivious Routing via LogMap Scan
  // MUST happen after bitonic sort because the hook only co-sorts .type/.key/.val,
  // not .phys_k/.sub_oram_idx. Routing here ensures alignment with the sorted batch.
  for (auto& op : batch) {
      op.sub_oram_idx = -1;
      op.phys_k = 0;
  }
  
  std::cout << "[DYNO] Phase 2: O2TH LogMap Scan starting..." << std::endl;
  
  std::call_once(g_oram_pool_init, [](){ 
      g_thread_ctx.bind_current_thread();
      g_oram_pool = std::make_unique<sn::threads::pthread_thread_pool>(g_thread_ctx, 23, "oram-batch"); 
  });
  sn::threads::thread_team team(g_oram_pool->pool(), 24);

  using O2TH = sn::omap::o2th::o2th_rwkv<uint64_t, 16>;
  O2TH::config cfg;
  cfg.block_count = B;
  cfg.bucket_size = 64;
  sn::util::log::logger log = sn::util::log::create("oram:batch");
  O2TH o2th(cfg, std::move(team), log);
  o2th.initialize();

  std::vector<O2TH::maybe_dummy<O2TH::op_request>> build_set(B);
  for (size_t i = 0; i < B; ++i) {
      bool is_first = (i == 0) || !sn::obliv::ct_eq(elems[i].key, elems[i-1].key);
      bool is_real = is_first & (elems[i].key != 0) & !elems[i].is_dummy;
      
      build_set[i].is_dummy = !is_real;
      build_set[i].value.key = elems[i].key;
      build_set[i].value.is_write = false;
      build_set[i].value.extra_data = static_cast<uint32_t>(i);
      
      std::array<uint8_t, 16> default_payload = {0};
      int8_t default_sub = -1;
      std::memcpy(default_payload.data() + 8, &default_sub, sizeof(int8_t));
      std::memcpy(build_set[i].value.data.data(), default_payload.data(), 16);
  }
  
  o2th.build(sn::util::span<O2TH::maybe_dummy<O2TH::op_request>>(build_set.data(), build_set.size()));

  for (int idx = 0; idx < 2; ++idx) {
      uint64_t cap = (idx == 0) ? cap_S : cap_L;
      if (cap > 0) {
          std::vector<O2TH::data_query> queries(cap);
          for (size_t i = 1; i <= cap; ++i) {
              uint64_t log_k = log_map_[idx][i].logical_key;
              uint64_t cur_leaf = log_map_[idx][i].leaf;
              bool is_real = !sn::obliv::ct_eq<uint64_t>(log_k, 0);
              
              std::array<uint8_t, 16> payload = {0};
              uint64_t packed = (i & 0x00FFFFFFFFFFFFFFULL) | (static_cast<uint64_t>(static_cast<uint8_t>(idx)) << 56);
              std::memcpy(payload.data(), &packed, 8);
              std::memcpy(payload.data() + 8, &cur_leaf, 8);
              
              queries[i-1].assign(sn::obliv::ct_select<uint64_t>(log_k, 0, is_real), payload, true);
          }

          std::vector<O2TH::bucket_index> pos_l1(cap);
          std::vector<O2TH::bucket_index> pos_l2(cap);
          
          o2th.access_batch(sn::util::span<O2TH::data_query>(queries.data(), queries.size()),
                            sn::util::span<O2TH::bucket_index>(pos_l1.data(), pos_l1.size()),
                            sn::util::span<O2TH::bucket_index>(pos_l2.data(), pos_l2.size()));
      }
  }

  std::vector<O2TH::maybe_dummy<O2TH::op_request>> retrieve_data(2 * B);
  std::vector<uint8_t> compact_marks(2 * B);
  std::vector<size_t> compact_prefix(2 * B + 1);
  o2th.retrieve(sn::util::span<O2TH::maybe_dummy<O2TH::op_request>>(retrieve_data.data(), retrieve_data.size()),
                sn::util::span<uint8_t>(compact_marks.data(), compact_marks.size()),
                sn::util::span<size_t>(compact_prefix.data(), compact_prefix.size()));

  struct alignas(8) JoinElement {
      uint32_t sort_key;
      bool is_target;
      uint32_t batch_idx;
      uint64_t phys_k;
      int8_t sub_idx;
      uint64_t cur_leaf;
      uint64_t new_leaf;
  };

  uint64_t cap_S_local = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  uint64_t cap_L_local = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;

  std::vector<JoinElement> join_arr(2 * B);
  for (size_t i = 0; i < B; ++i) {
      bool is_dummy_update = retrieve_data[i].is_dummy;
      join_arr[i].sort_key = sn::obliv::ct_select<uint32_t>(UINT32_MAX, retrieve_data[i].value.extra_data, is_dummy_update);
      join_arr[i].is_target = false;
      join_arr[i].batch_idx = 0;
      
      uint64_t packed = 0;
      uint64_t cur_leaf = 0;
      std::memcpy(&packed, retrieve_data[i].value.data.data(), 8);
      std::memcpy(&cur_leaf, retrieve_data[i].value.data.data() + 8, 8);
      
      uint64_t phys_k = packed & 0x00FFFFFFFFFFFFFFULL;
      int8_t sub_idx = static_cast<int8_t>(packed >> 56);
      
      uint64_t new_leaf = 0;
      if (sub_idx == 0 && sub_orams_[0]) new_leaf = sub_orams_[0]->GenerateRandomLeaf() - 1;
      else if (sub_idx == 1 && sub_orams_[1]) new_leaf = sub_orams_[1]->GenerateRandomLeaf() - 1;
      
      if (!is_dummy_update && sub_idx >= 0 && phys_k > 0) {
          log_map_[sub_idx][phys_k].leaf = new_leaf;
      }

      join_arr[i].phys_k = phys_k;
      join_arr[i].sub_idx = sub_idx;
      join_arr[i].cur_leaf = cur_leaf;
      join_arr[i].new_leaf = new_leaf;

      join_arr[B + i].sort_key = static_cast<uint32_t>(i);
      join_arr[B + i].is_target = true;
      join_arr[B + i].batch_idx = static_cast<uint32_t>(i);
      join_arr[B + i].phys_k = 0;
      join_arr[B + i].sub_idx = -1;
      join_arr[B + i].cur_leaf = 0;
      join_arr[B + i].new_leaf = 0;
  }

  auto comp_join1 = [](const JoinElement& a, const JoinElement& b) {
      bool key_eq = sn::obliv::ct_eq(a.sort_key, b.sort_key);
      bool key_lt = sn::obliv::ct_lt(a.sort_key, b.sort_key);
      bool target_lt = static_cast<uint8_t>(a.is_target) < static_cast<uint8_t>(b.is_target);
      return sn::obliv::ct_select(target_lt, key_lt, key_eq);
  };
  auto ext1 = [](const JoinElement& e) { return e; };
  sn::sortshuffle::ser::bitonic::detail::noop_hook hook1;
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(join_arr.data(), 2 * B, ext1, comp_join1, hook1);

  uint64_t cur_phys_k = 0;
  int8_t cur_sub_idx = -1;
  uint64_t current_cur_leaf = 0;
  uint64_t current_new_leaf = 0;
  for (size_t i = 0; i < 2 * B; ++i) {
      bool is_update = !join_arr[i].is_target;
      cur_phys_k = sn::obliv::ct_select<uint64_t>(join_arr[i].phys_k, cur_phys_k, is_update);
      cur_sub_idx = sn::obliv::ct_select<int8_t>(join_arr[i].sub_idx, cur_sub_idx, is_update);
      current_cur_leaf = sn::obliv::ct_select<uint64_t>(join_arr[i].cur_leaf, current_cur_leaf, is_update);
      current_new_leaf = sn::obliv::ct_select<uint64_t>(join_arr[i].new_leaf, current_new_leaf, is_update);
      
      join_arr[i].phys_k = sn::obliv::ct_select<uint64_t>(cur_phys_k, join_arr[i].phys_k, join_arr[i].is_target);
      join_arr[i].sub_idx = sn::obliv::ct_select<int8_t>(cur_sub_idx, join_arr[i].sub_idx, join_arr[i].is_target);
      join_arr[i].cur_leaf = sn::obliv::ct_select<uint64_t>(current_cur_leaf, join_arr[i].cur_leaf, join_arr[i].is_target);
      join_arr[i].new_leaf = sn::obliv::ct_select<uint64_t>(current_new_leaf, join_arr[i].new_leaf, join_arr[i].is_target);
  }

  auto comp_join2 = [](const JoinElement& a, const JoinElement& b) {
      bool target_a = a.is_target;
      bool target_b = b.is_target;
      bool target_eq = target_a == target_b;
      bool target_gt = target_a && !target_b;
      bool idx_lt = sn::obliv::ct_lt(a.batch_idx, b.batch_idx);
      return sn::obliv::ct_select(idx_lt, target_gt, target_eq);
  };
  auto ext2 = [](const JoinElement& e) { return e; };
  sn::sortshuffle::ser::bitonic::detail::noop_hook hook2;
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(join_arr.data(), 2 * B, ext2, comp_join2, hook2);

  for (size_t i = 0; i < B; ++i) {
      batch[elems[i].seq].phys_k = join_arr[i].phys_k;
      batch[elems[i].seq].sub_oram_idx = join_arr[i].sub_idx;
      batch[elems[i].seq].cur_leaf = join_arr[i].cur_leaf;
      batch[elems[i].seq].new_leaf = join_arr[i].new_leaf;
  }

  uint64_t fw_phys_k = 0;
  int8_t fw_sub_idx = -1;
  uint64_t fw_cur_leaf = 0;
  uint64_t fw_new_leaf = 0;
  for (size_t i = 0; i < B; ++i) {
      bool is_first = (i == 0) || !sn::obliv::ct_eq(elems[i].key, elems[i-1].key);
      fw_phys_k = sn::obliv::ct_select<uint64_t>(batch[elems[i].seq].phys_k, fw_phys_k, is_first);
      fw_sub_idx = sn::obliv::ct_select<int8_t>(batch[elems[i].seq].sub_oram_idx, fw_sub_idx, is_first);
      fw_cur_leaf = sn::obliv::ct_select<uint64_t>(batch[elems[i].seq].cur_leaf, fw_cur_leaf, is_first);
      fw_new_leaf = sn::obliv::ct_select<uint64_t>(batch[elems[i].seq].new_leaf, fw_new_leaf, is_first);
      
      batch[elems[i].seq].phys_k = fw_phys_k;
      batch[elems[i].seq].sub_oram_idx = fw_sub_idx;
      batch[elems[i].seq].cur_leaf = fw_cur_leaf;
      batch[elems[i].seq].new_leaf = fw_new_leaf;
  }

  auto simd_select_48 = [](uint8_t* dest, const uint8_t* a, const uint8_t* b, bool cond) {
#if defined(__AVX2__)
      __m256i mask = _mm256_set1_epi8(cond ? 0xFF : 0x00);
      
      // First 32 bytes (offset 0)
      __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
      __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));
      __m256i r1 = _mm256_blendv_epi8(b1, a1, mask);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest), r1);

      // Remaining 16 bytes + 16 bytes of overlap (offset 16)
      __m256i a2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 16));
      __m256i b2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 16));
      __m256i r2 = _mm256_blendv_epi8(b2, a2, mask);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dest + 16), r2);
#else
      sn::obliv::ct_select_array(dest, a, b, 48, cond);
#endif
  };

  std::cout << "[DYNO] Phase 3: O-Scan (Collapse) starting..." << std::endl;
  
  // Pass 1: Forward Scan (Search Resolution & State Tracking)
  std::vector<uint8_t> current_payload(val_len_, 0);
  bool has_payload = false;
  bool is_deleted = false;
  bool seen_insert = false;
  std::vector<bool> seen_insert_arr(B, false);
  
  for (size_t i = 0; i < B; ++i) {
      bool start_of_key = (i == 0) || !sn::obliv::ct_eq(elems[i].key, elems[i-1].key);
      has_payload = sn::obliv::ct_select(false, has_payload, start_of_key);
      is_deleted = sn::obliv::ct_select(false, is_deleted, start_of_key);
      seen_insert = sn::obliv::ct_select(false, seen_insert, start_of_key);
      
      seen_insert_arr[i] = seen_insert;
      
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
      bool is_update = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Update));
      bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
      bool is_search = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Search));
      
      seen_insert = seen_insert | is_insert;
      
      bool is_write = is_insert | is_update;
      if (val_len_ > 0) {
          if (val_len_ == 48) simd_select_48(current_payload.data(), batch[elems[i].seq].val.get(), current_payload.data(), is_write);
          else sn::obliv::ct_select_array(current_payload.data(), batch[elems[i].seq].val.get(), current_payload.data(), val_len_, is_write);
      }
      has_payload = has_payload | is_write;
      
      is_deleted = sn::obliv::ct_select(true, is_deleted, is_delete);
      is_deleted = sn::obliv::ct_select(false, is_deleted, is_insert);
      
      // Forward latest payload to searches
      bool forward_to_search = is_search & has_payload & !is_deleted;
      if (val_len_ > 0) {
          if (val_len_ == 48) simd_select_48(batch[elems[i].seq].result.val_.get(), current_payload.data(), batch[elems[i].seq].result.val_.get(), forward_to_search);
          else sn::obliv::ct_select_array(batch[elems[i].seq].result.val_.get(), current_payload.data(), batch[elems[i].seq].result.val_.get(), val_len_, forward_to_search);
      }
      batch[elems[i].seq].result.key_ = sn::obliv::ct_select<uint64_t>(1, batch[elems[i].seq].result.key_, forward_to_search);
      
      // Searches resolved from previous operations in batch become dummy
      // Updates on deleted keys are invalid no-ops and must also become dummy
      bool search_becomes_dummy = (is_search & has_payload) | (is_search & is_deleted);
      bool update_becomes_dummy = is_update & is_deleted;
      sn::obliv::ct_set_ref(elems[i].is_dummy, true, search_becomes_dummy | update_becomes_dummy);
  }

  // Pass 2: Backward Scan (Semantic Deduplication)
  bool has_future_delete = false;
  bool has_future_access = false;
  bool has_future_insert = false;
  bool has_future_write = false;
  std::vector<uint8_t> backward_payload(val_len_, 0);
  
  for (int64_t i = B - 1; i >= 0; --i) {
      bool end_of_key = (static_cast<size_t>(i) == B - 1) || !sn::obliv::ct_eq(elems[i].key, elems[i+1].key);
      has_future_delete = sn::obliv::ct_select(false, has_future_delete, end_of_key);
      has_future_access = sn::obliv::ct_select(false, has_future_access, end_of_key);
      has_future_insert = sn::obliv::ct_select(false, has_future_insert, end_of_key);
      has_future_write = sn::obliv::ct_select(false, has_future_write, end_of_key);
      
      bool is_already_dummy = elems[i].is_dummy;
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert)) & !is_already_dummy;
      bool is_update = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Update)) & !is_already_dummy;
      bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete)) & !is_already_dummy;
      bool is_search = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Search)) & !is_already_dummy;
      
      bool is_access = is_update | is_delete | is_search;
      bool is_write = is_insert | is_update;
      
      bool captures_payload = is_write & !has_future_write;
      if (val_len_ > 0) {
          if (val_len_ == 48) simd_select_48(backward_payload.data(), batch[elems[i].seq].val.get(), backward_payload.data(), captures_payload);
          else sn::obliv::ct_select_array(backward_payload.data(), batch[elems[i].seq].val.get(), backward_payload.data(), val_len_, captures_payload);
      }
      
      bool survives_insert = is_insert & !has_future_insert & !has_future_delete;
      bool survives_access = is_access & !has_future_access & !seen_insert_arr[i];
      bool is_real = survives_insert | survives_access;
      
      sn::obliv::ct_set_ref(elems[i].is_dummy, true, !is_real);
      
      if (val_len_ > 0) {
          bool receives_payload = is_real & is_write;
          if (val_len_ == 48) simd_select_48(batch[elems[i].seq].val.get(), backward_payload.data(), batch[elems[i].seq].val.get(), receives_payload);
          else sn::obliv::ct_select_array(batch[elems[i].seq].val.get(), backward_payload.data(), batch[elems[i].seq].val.get(), val_len_, receives_payload);
      }
      
      has_future_write = has_future_write | is_write;
      has_future_delete = sn::obliv::ct_select(false, has_future_delete, is_insert);
      has_future_delete = has_future_delete | is_delete;
      
      has_future_access = sn::obliv::ct_select(false, has_future_access, is_insert);
      has_future_access = has_future_access | is_access;
      
      has_future_insert = has_future_insert | is_insert;
  }

  // Calculate Real Net Growth Obliviously BEFORE Deletes are forced to dummies
  size_t real_I = 0, real_DS = 0, real_DL = 0;
  for (size_t i = 0; i < B; ++i) {
    bool is_real = !elems[i].is_dummy;
    bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
    bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
    
    uint32_t orig_idx = elems[i].seq;
    uint8_t idx = batch[orig_idx].sub_oram_idx;
    
    real_I += sn::obliv::ct_select<size_t>(1, 0, is_real && is_insert);
    real_DS += sn::obliv::ct_select<size_t>(1, 0, is_real && is_delete && (idx == 0));
    real_DL += sn::obliv::ct_select<size_t>(1, 0, is_real && is_delete && (idx == 1));
  }

  // Allocate empty slots for REAL Inserts in S_large obliviously
  std::cout << "[DYNO] Phase 3.5: O2TH Empty Slot Allocation starting..." << std::endl;

  std::vector<uint64_t> insert_seq_arr(B, 0);
  uint64_t current_insert_seq = 1;
  for (size_t i = 0; i < B; ++i) {
      bool is_real = !elems[i].is_dummy;
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
      bool needs_slot = is_real && is_insert;
      
      uint32_t orig_idx = elems[i].seq;
      uint64_t initial_leaf = sub_orams_[1] ? sub_orams_[1]->GenerateRandomLeaf() - 1 : 0;
      batch[orig_idx].new_leaf = sn::obliv::ct_select(initial_leaf, batch[orig_idx].new_leaf, needs_slot);
      batch[orig_idx].cur_leaf = sn::obliv::ct_select<uint64_t>(0, batch[orig_idx].cur_leaf, needs_slot);
      
      insert_seq_arr[i] = sn::obliv::ct_select<uint64_t>(current_insert_seq, 0, needs_slot);
      current_insert_seq = sn::obliv::ct_select<uint64_t>(current_insert_seq + 1, current_insert_seq, needs_slot);
  }

  using O2TH_Alloc = sn::omap::o2th::o2th_rwkv<uint64_t, 16>;
  O2TH_Alloc::config cfg_alloc;
  cfg_alloc.block_count = B;
  cfg_alloc.bucket_size = 64;
  sn::util::log::logger log_alloc = sn::util::log::create("oram:alloc");

  sn::threads::thread_team team_alloc(g_oram_pool->pool(), 24);
  O2TH_Alloc o2th_alloc(cfg_alloc, std::move(team_alloc), log_alloc);
  o2th_alloc.initialize();

  std::vector<O2TH_Alloc::maybe_dummy<O2TH_Alloc::op_request>> alloc_build(B);
  for (size_t i = 0; i < B; ++i) {
      uint32_t orig_idx = elems[i].seq;
      bool needs_slot = (insert_seq_arr[i] > 0);

      alloc_build[i].is_dummy = !needs_slot;
      alloc_build[i].value.key = insert_seq_arr[i]; 
      alloc_build[i].value.is_write = true;
      alloc_build[i].value.extra_data = static_cast<uint32_t>(orig_idx); 
      
      std::array<uint8_t, 16> payload = {0};
      uint64_t logical_key = elems[i].key;
      
      uint64_t new_insert_leaf = sub_orams_[1] ? sub_orams_[1]->GenerateRandomLeaf() - 1 : 0;
      batch[orig_idx].new_leaf = sn::obliv::ct_select(new_insert_leaf, batch[orig_idx].new_leaf, needs_slot);
      
      uint64_t leaf_to_write = batch[orig_idx].new_leaf;
      std::memcpy(payload.data(), &logical_key, sizeof(uint64_t));
      std::memcpy(payload.data() + 8, &leaf_to_write, sizeof(uint64_t));
      alloc_build[i].value.data.bytes = payload;
  }

  o2th_alloc.build(sn::util::span<O2TH_Alloc::maybe_dummy<O2TH_Alloc::op_request>>(alloc_build.data(), alloc_build.size()));

  std::vector<O2TH_Alloc::data_query> alloc_queries(cap_L);
  uint64_t empty_seq = 1;
  for (uint64_t j = 1; j <= cap_L; ++j) {
      bool is_empty = (log_map_[1][j].logical_key == 0);
      uint64_t query_key = sn::obliv::ct_select<uint64_t>(empty_seq, 0, is_empty);

      std::array<uint8_t, 16> payload = {0};
      std::memcpy(payload.data(), &j, sizeof(uint64_t)); 

      alloc_queries[j-1].assign(query_key, payload, true);
      empty_seq = sn::obliv::ct_select<uint64_t>(empty_seq + 1, empty_seq, is_empty);
  }

  std::vector<O2TH_Alloc::bucket_index> alloc_pos_l1(cap_L);
  std::vector<O2TH_Alloc::bucket_index> alloc_pos_l2(cap_L);

  if (cap_L > 0) {
      o2th_alloc.access_batch(sn::util::span<O2TH_Alloc::data_query>(alloc_queries.data(), alloc_queries.size()),
                              sn::util::span<O2TH_Alloc::bucket_index>(alloc_pos_l1.data(), alloc_pos_l1.size()),
                              sn::util::span<O2TH_Alloc::bucket_index>(alloc_pos_l2.data(), alloc_pos_l2.size()));
  }

  for (uint64_t j = 1; j <= cap_L; ++j) {
      bool is_empty = (log_map_[1][j].logical_key == 0);
      uint64_t logical_key_received = 0;
      uint64_t leaf_received = 0;
      std::memcpy(&logical_key_received, alloc_queries[j-1].data.data(), sizeof(uint64_t));
      std::memcpy(&leaf_received, alloc_queries[j-1].data.data() + 8, sizeof(uint64_t));
      
      bool received_valid = is_empty && (logical_key_received != 0);
      log_map_[1][j].logical_key = sn::obliv::ct_select<uint64_t>(logical_key_received, log_map_[1][j].logical_key, received_valid);
      log_map_[1][j].leaf = sn::obliv::ct_select<uint64_t>(leaf_received, log_map_[1][j].leaf, received_valid);
  }

  std::vector<O2TH_Alloc::maybe_dummy<O2TH_Alloc::op_request>> alloc_retrieve(2 * B);
  std::vector<uint8_t> alloc_compact_marks(2 * B);
  std::vector<size_t> alloc_compact_prefix(2 * B + 1);

  o2th_alloc.retrieve(sn::util::span<O2TH_Alloc::maybe_dummy<O2TH_Alloc::op_request>>(alloc_retrieve.data(), alloc_retrieve.size()),
                      sn::util::span<uint8_t>(alloc_compact_marks.data(), alloc_compact_marks.size()),
                      sn::util::span<size_t>(alloc_compact_prefix.data(), alloc_compact_prefix.size()));

  struct alignas(8) AllocJoinElement {
      uint32_t sort_key;
      bool is_target;
      uint32_t orig_idx;
      uint64_t assigned_slot;
  };

  std::vector<AllocJoinElement> alloc_join(2 * B);
  for (size_t i = 0; i < B; ++i) {
      bool is_dummy_ret = alloc_retrieve[i].is_dummy;
      alloc_join[i].sort_key = sn::obliv::ct_select<uint32_t>(UINT32_MAX, alloc_retrieve[i].value.extra_data, is_dummy_ret);
      alloc_join[i].is_target = false;
      alloc_join[i].orig_idx = 0;
      
      uint64_t assigned_slot = 0;
      std::memcpy(&assigned_slot, alloc_retrieve[i].value.data.data(), sizeof(uint64_t));
      alloc_join[i].assigned_slot = assigned_slot;
      
      alloc_join[B + i].sort_key = static_cast<uint32_t>(i);
      alloc_join[B + i].is_target = true;
      alloc_join[B + i].orig_idx = static_cast<uint32_t>(i);
      alloc_join[B + i].assigned_slot = 0;
  }

  auto comp_alloc = [](const AllocJoinElement& a, const AllocJoinElement& b) {
      bool key_eq = sn::obliv::ct_eq(a.sort_key, b.sort_key);
      bool key_lt = sn::obliv::ct_lt(a.sort_key, b.sort_key);
      bool target_lt = static_cast<uint8_t>(a.is_target) < static_cast<uint8_t>(b.is_target);
      return sn::obliv::ct_select(target_lt, key_lt, key_eq);
  };
  sn::sortshuffle::ser::bitonic::detail::noop_hook hook_alloc;
  auto key_ext_alloc = [](const AllocJoinElement& e){ return e; };
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(alloc_join.data(), 2 * B, key_ext_alloc, comp_alloc, hook_alloc);

  uint64_t cur_assigned_slot = 0;
  for (size_t i = 0; i < 2 * B; ++i) {
      bool is_update = !alloc_join[i].is_target;
      cur_assigned_slot = sn::obliv::ct_select<uint64_t>(alloc_join[i].assigned_slot, cur_assigned_slot, is_update);
      alloc_join[i].assigned_slot = sn::obliv::ct_select<uint64_t>(cur_assigned_slot, alloc_join[i].assigned_slot, alloc_join[i].is_target);
  }

  auto comp_alloc2 = [](const AllocJoinElement& a, const AllocJoinElement& b) {
      bool target_lt = static_cast<uint8_t>(b.is_target) < static_cast<uint8_t>(a.is_target);
      bool idx_lt = sn::obliv::ct_lt(a.orig_idx, b.orig_idx);
      return sn::obliv::ct_select(idx_lt, target_lt, a.is_target == b.is_target);
  };
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(alloc_join.data(), 2 * B, key_ext_alloc, comp_alloc2, hook_alloc);

  for (size_t i = 0; i < B; ++i) {
      uint32_t orig_idx = alloc_join[i].orig_idx;
      bool received_slot = (alloc_join[i].assigned_slot != 0);
      batch[orig_idx].sub_oram_idx = sn::obliv::ct_select<int8_t>(1, batch[orig_idx].sub_oram_idx, received_slot);
      batch[orig_idx].phys_k = sn::obliv::ct_select<uint64_t>(alloc_join[i].assigned_slot, batch[orig_idx].phys_k, received_slot);
  }

  std::cout << "[DYNO] Phase 4: O-Sort (Group by OpType) starting..." << std::endl;
  // Phase 3: O-Sort (Group by OpType)
  struct NoOpHook {
      void operator()(OblivElem* a, OblivElem* b, bool cond) const {}
  };
  
  auto comp3 = [](const OblivElem& a, const OblivElem& b) {
      bool a_is_ins = sn::obliv::ct_eq(a.op_type, static_cast<uint8_t>(OpType::Insert));
      bool b_is_ins = sn::obliv::ct_eq(b.op_type, static_cast<uint8_t>(OpType::Insert));
      bool ins_eq = sn::obliv::ct_eq(a_is_ins, b_is_ins);
      bool ins_lt = !a_is_ins & b_is_ins;
      bool seq_lt = sn::obliv::ct_lt(a.seq, b.seq);
      return sn::obliv::ct_select(seq_lt, ins_lt, ins_eq);
  };
  
  NoOpHook no_op_hook;
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(elems.data(), B, key_ext, comp3, no_op_hook);

  std::vector<Block> inserts;
  std::vector<SonicORamAdapter::AccessOp> small_ops, large_ops;

  // Static partition point based on public initial distribution
  size_t original_accesses = B - original_inserts;

  for (size_t i = original_accesses; i < B; ++i) {
    Block b;
    bool is_real = !elems[i].is_dummy;
    uint32_t orig_idx = elems[i].seq;
    b.key_ = sn::obliv::ct_select<uint64_t>(batch[orig_idx].key, 0, is_real);
    
    if (batch[orig_idx].val) {
      b.val_ = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[orig_idx].val.get(), batch[orig_idx].val.get() + val_len_, b.val_.get());
    }
    inserts.push_back(std::move(b));
  }

  for (size_t i = 0; i < original_accesses; ++i) {
    uint32_t orig_idx = elems[i].seq;
    bool is_real = !elems[i].is_dummy;
    uint8_t idx = batch[orig_idx].sub_oram_idx;
    uint8_t op_type = static_cast<uint8_t>(batch[orig_idx].type);
    
    bool is_search = sn::obliv::ct_eq(op_type, static_cast<uint8_t>(OpType::Search));
    bool is_delete = sn::obliv::ct_eq(op_type, static_cast<uint8_t>(OpType::Delete));
    bool is_update = sn::obliv::ct_eq(op_type, static_cast<uint8_t>(OpType::Update));
    
    bool is_access = is_search || is_delete || is_update;
    
    SonicORamAdapter::AccessOp op;
    op.key = batch[orig_idx].phys_k;
    op.op_type = op_type;
    if (batch[orig_idx].val) {
      op.val = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[orig_idx].val.get(), batch[orig_idx].val.get() + val_len_, op.val.get());
    }
    op.is_real = is_real && is_access && (idx == 0);
    small_ops.push_back(std::move(op));

    SonicORamAdapter::AccessOp op_l;
    op_l.key = batch[orig_idx].phys_k;
    op_l.op_type = op_type;
    if (batch[orig_idx].val) {
      op_l.val = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[orig_idx].val.get(), batch[orig_idx].val.get() + val_len_, op_l.val.get());
    }
    op_l.is_real = is_real && is_access && (idx == 1);
    large_ops.push_back(std::move(op_l));
  }

  std::cout << "[DYNO] Phase 5: Sub-ORAM ReadBatch starting..." << std::endl;
  if (sub_orams_[0]) {
      auto read_results = sub_orams_[0]->ReadBatch(small_ops, enc_key, steady_state);
      for (size_t i = 0; i < original_accesses; ++i) {
          if (small_ops[i].is_real) {
              uint32_t orig_idx = elems[i].seq;
              batch[orig_idx].result.key_ = read_results[i].meta_.key_;
              if (read_results[i].val_) {
                  batch[orig_idx].result.val_ = std::make_unique<uint8_t[]>(val_len_);
                  std::copy(read_results[i].val_.get(), read_results[i].val_.get() + val_len_, batch[orig_idx].result.val_.get());
              }
          }
      }
  }
  if (sub_orams_[1]) {
      auto read_results = sub_orams_[1]->ReadBatch(large_ops, enc_key, steady_state);
      for (size_t i = 0; i < original_accesses; ++i) {
          if (large_ops[i].is_real) {
              uint32_t orig_idx = elems[i].seq;
              batch[orig_idx].result.key_ = read_results[i].meta_.key_;
              if (read_results[i].val_) {
                  batch[orig_idx].result.val_ = std::make_unique<uint8_t[]>(val_len_);
                  std::copy(read_results[i].val_.get(), read_results[i].val_.get() + val_len_, batch[orig_idx].result.val_.get());
              }
          }
      }
  }

  // Clear log_map entries for successfully executed Deletes
  for (size_t i = 0; i < original_accesses; ++i) {
      uint32_t orig_idx = elems[i].seq;
      bool is_real = !elems[i].is_dummy;
      bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
      bool should_clear = is_real && is_delete;
      
      if (should_clear) {
          uint8_t idx = batch[orig_idx].sub_oram_idx;
          uint64_t phys_k = batch[orig_idx].phys_k;
          if (idx == 0 && phys_k > 0 && phys_k < log_map_[0].size()) {
              log_map_[0][phys_k].logical_key = 0;
              log_map_[0][phys_k].leaf = 0;
          } else if (idx == 1 && phys_k > 0 && phys_k < log_map_[1].size()) {
              log_map_[1][phys_k].logical_key = 0;
              log_map_[1][phys_k].leaf = 0;
          }
      }
  }

  // Phase 2.5: Post-ReadBatch backward scan to propagate old values to dummy Searches
  std::vector<uint8_t> old_payload(val_len_, 0);
  uint64_t old_key = 0;
  bool has_old_payload = false;
  
  for (int64_t i = original_accesses - 1; i >= 0; --i) {
      bool end_of_key = (i == static_cast<int64_t>(original_accesses) - 1) || !sn::obliv::ct_eq(elems[i].key, elems[i+1].key);
      has_old_payload = sn::obliv::ct_select(false, has_old_payload, end_of_key);
      old_key = sn::obliv::ct_select<uint64_t>(0, old_key, end_of_key);
      
      uint32_t orig_idx = elems[i].seq;
      bool is_real = !elems[i].is_dummy;
      bool is_search = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Search));
      
      if (val_len_ > 0) {
          sn::obliv::ct_select_array(old_payload.data(), batch[orig_idx].result.val_.get(), old_payload.data(), val_len_, is_real);
      }
      old_key = sn::obliv::ct_select<uint64_t>(batch[orig_idx].result.key_, old_key, is_real);
      has_old_payload = has_old_payload | is_real;
      
      bool received_forward = sn::obliv::ct_eq<uint64_t>(batch[orig_idx].result.key_, 1);
      bool needs_old_payload = is_search & !is_real & has_old_payload & !received_forward;
      
      if (val_len_ > 0) {
          if (!batch[orig_idx].result.val_) {
              batch[orig_idx].result.val_ = std::make_unique<uint8_t[]>(val_len_);
          }
          sn::obliv::ct_select_array(batch[orig_idx].result.val_.get(), old_payload.data(), batch[orig_idx].result.val_.get(), val_len_, needs_old_payload);
      }
      
      uint64_t final_key = sn::obliv::ct_select<uint64_t>(batch[orig_idx].key, 0, received_forward);
      final_key = sn::obliv::ct_select<uint64_t>(old_key, final_key, needs_old_payload);
      final_key = sn::obliv::ct_select<uint64_t>(batch[orig_idx].result.key_, final_key, is_real & is_search);
      batch[orig_idx].result.key_ = final_key;
  }

  if (sub_orams_[1]) {
    std::vector<static_path_oram::Block> sn_inserts;
    sn_inserts.reserve(inserts.size());
    for (auto& b : inserts) {
        uint64_t phys_k = 0;
        uint64_t leaf = 0;
        for (size_t j = original_accesses; j < B; ++j) {
            uint32_t orig_idx = elems[j].seq;
            bool match = sn::obliv::ct_eq(b.key_, batch[orig_idx].key);
            phys_k = sn::obliv::ct_select(batch[orig_idx].phys_k, phys_k, match);
            leaf = sn::obliv::ct_select(batch[orig_idx].new_leaf, leaf, match);
        }
        
        static_path_oram::Block new_b(static_cast<static_path_oram::Pos>(leaf + 1), static_cast<static_path_oram::Key>(phys_k));
        if (b.val_) {
            new_b.val_ = std::move(b.val_);
        }
        sn_inserts.push_back(std::move(new_b));
    }
    std::cout << "[DYNO] Phase 6: Sub-ORAM InsertBatch starting..." << std::endl;
    sub_orams_[1]->InsertBatch(sn_inserts, enc_key, steady_state, true);
  }

  // Phase 3: Oblivious Bidirectional Transfer Boundaries
  size_t x = GetSubORamValidCount(0);
  size_t y = GetSubORamValidCount(1);
  
  int64_t a = static_cast<int64_t>(real_I) - static_cast<int64_t>(real_DS) - static_cast<int64_t>(real_DL);

  int64_t target_small = static_cast<int64_t>(x) - a;
  int64_t target_large = static_cast<int64_t>(y) + 2*a;

  int64_t k_transfer = a - real_DS;
  bool scale_up = false, scale_down = false;
  int64_t T = 0;

  if (target_small <= 0) {
    k_transfer = x;
    T = x;
    scale_up = true;
  } else if (target_large <= 0) {
    k_transfer = -(static_cast<int64_t>(y) - static_cast<int64_t>(real_DL) + static_cast<int64_t>(real_I));
    int64_t total_deletes = static_cast<int64_t>(real_DS) + static_cast<int64_t>(real_DL);
    int64_t tightened_limit = std::min(static_cast<int64_t>(real_I), total_deletes - static_cast<int64_t>(y / 2));
    T = static_cast<int64_t>(y) + std::max(static_cast<int64_t>(0), tightened_limit);
    scale_down = true;
  } else {
    T = std::max(std::abs(static_cast<int64_t>(original_deletes) - a), std::abs(a));
  }

  // Phase 4: Exact Transfer via LogMap
  if (sub_orams_[0] && sub_orams_[1] && T > 0) {
    int64_t T_pow2 = 1;
    while (T_pow2 < T) T_pow2 *= 2;

    auto extract_keys = [&](int sub_idx, int64_t k_count, int64_t T_count) {
        std::vector<Key> S_keys(T_count, 0);
        uint64_t count = 0;
        uint64_t cap = (sub_idx == 0) ? cap_S : cap_L;
        for (uint64_t i = 1; i <= cap; ++i) {
            bool is_real = (log_map_[sub_idx][i].logical_key != 0);
            bool take = is_real & (count < static_cast<uint64_t>(k_count));
            for (uint64_t j = 0; j < static_cast<uint64_t>(T_count); ++j) {
                bool match = take & (count == j);
                S_keys[j] = sn::obliv::ct_select<Key>(i, S_keys[j], match);
            }
            count = sn::obliv::ct_select(count + 1, count, take);
        }
        return S_keys;
    };
    
    bool transfer_up = sn::obliv::ct_gt(k_transfer, static_cast<int64_t>(0));
    bool transfer_down = sn::obliv::ct_lt(k_transfer, static_cast<int64_t>(0));
    int64_t abs_k_transfer = sn::obliv::ct_select(-k_transfer, k_transfer, transfer_down);
    
    std::vector<Key> extracted_0 = extract_keys(0, abs_k_transfer, T);
    std::vector<Key> extracted_1 = extract_keys(1, abs_k_transfer, T);
    
    std::vector<std::pair<Key, bool>> keys_to_read_0(T), keys_to_read_1(T);
    for (int64_t i = 0; i < T; ++i) {
        keys_to_read_0[i] = {extracted_0[i], transfer_up && extracted_0[i] != 0};
        keys_to_read_1[i] = {extracted_1[i], transfer_down && extracted_1[i] != 0};
    }

    std::vector<static_path_oram::Block> Buffer_0 = sub_orams_[0]->ReadAndRemoveBatch(keys_to_read_0, enc_key);
    std::vector<static_path_oram::Block> Buffer_1 = sub_orams_[1]->ReadAndRemoveBatch(keys_to_read_1, enc_key);
    
    for (int64_t i = T; i < T_pow2; ++i) {
        Buffer_0.emplace_back(true);
        Buffer_1.emplace_back(true);
        if (val_len_ > 0) {
            Buffer_0.back().val_ = std::make_unique<uint8_t[]>(val_len_);
            std::fill(Buffer_0.back().val_.get(), Buffer_0.back().val_.get() + val_len_, 0);
            Buffer_1.back().val_ = std::make_unique<uint8_t[]>(val_len_);
            std::fill(Buffer_1.back().val_.get(), Buffer_1.back().val_.get() + val_len_, 0);
        }
    }

    for (int64_t i = 0; i < T; ++i) {
        uint64_t old_phys_k_0 = Buffer_0[i].meta_.key_;
        bool is_valid_0 = transfer_up && (old_phys_k_0 != 0);
        uint64_t log_k_0 = 0;
        uint64_t leaf_0 = 0;
        for (uint64_t j = 1; j <= cap_S; ++j) {
            bool match = is_valid_0 && sn::obliv::ct_eq(j, old_phys_k_0);
            log_k_0 = sn::obliv::ct_select(log_map_[0][j].logical_key, log_k_0, match);
            leaf_0 = sn::obliv::ct_select(log_map_[0][j].leaf, leaf_0, match);
            log_map_[0][j].logical_key = sn::obliv::ct_select<uint64_t>(0, log_map_[0][j].logical_key, match);
            log_map_[0][j].leaf = sn::obliv::ct_select<uint64_t>(0, log_map_[0][j].leaf, match);
        }
        uint64_t new_phys_k_0 = 0;
        uint64_t new_insert_leaf_1 = sub_orams_[1] ? sub_orams_[1]->GenerateRandomLeaf() - 1 : 0;
        for (uint64_t j = 1; j <= cap_L; ++j) {
            bool is_empty = (log_map_[1][j].logical_key == 0);
            bool select_this = is_valid_0 && is_empty && (new_phys_k_0 == 0);
            new_phys_k_0 = sn::obliv::ct_select(j, new_phys_k_0, select_this);
            log_map_[1][j].logical_key = sn::obliv::ct_select(log_k_0, log_map_[1][j].logical_key, select_this);
            log_map_[1][j].leaf = sn::obliv::ct_select(new_insert_leaf_1, log_map_[1][j].leaf, select_this);
        }
        Buffer_0[i].meta_.key_ = sn::obliv::ct_select(new_phys_k_0, static_cast<uint64_t>(0), is_valid_0);
        Buffer_0[i].meta_.pos_ = sn::obliv::ct_select(new_insert_leaf_1 + 1, static_cast<uint64_t>(0), is_valid_0);

        uint64_t old_phys_k_1 = Buffer_1[i].meta_.key_;
        bool is_valid_1 = transfer_down && (old_phys_k_1 != 0);
        uint64_t log_k_1 = 0;
        uint64_t leaf_1 = 0;
        for (uint64_t j = 1; j <= cap_L; ++j) {
            bool match = is_valid_1 && sn::obliv::ct_eq(j, old_phys_k_1);
            log_k_1 = sn::obliv::ct_select(log_map_[1][j].logical_key, log_k_1, match);
            leaf_1 = sn::obliv::ct_select(log_map_[1][j].leaf, leaf_1, match);
            log_map_[1][j].logical_key = sn::obliv::ct_select<uint64_t>(0, log_map_[1][j].logical_key, match);
            log_map_[1][j].leaf = sn::obliv::ct_select<uint64_t>(0, log_map_[1][j].leaf, match);
        }
        uint64_t new_phys_k_1 = 0;
        uint64_t new_insert_leaf_0 = sub_orams_[0] ? sub_orams_[0]->GenerateRandomLeaf() - 1 : 0;
        for (uint64_t j = 1; j <= cap_S; ++j) {
            bool is_empty = (log_map_[0][j].logical_key == 0);
            bool select_this = is_valid_1 && is_empty && (new_phys_k_1 == 0);
            new_phys_k_1 = sn::obliv::ct_select(j, new_phys_k_1, select_this);
            log_map_[0][j].logical_key = sn::obliv::ct_select(log_k_1, log_map_[0][j].logical_key, select_this);
            log_map_[0][j].leaf = sn::obliv::ct_select(new_insert_leaf_0, log_map_[0][j].leaf, select_this);
        }
        Buffer_1[i].meta_.key_ = sn::obliv::ct_select(new_phys_k_1, static_cast<uint64_t>(0), is_valid_1);
        Buffer_1[i].meta_.pos_ = sn::obliv::ct_select(new_insert_leaf_0 + 1, static_cast<uint64_t>(0), is_valid_1);
    }

    sub_orams_[1]->InsertBatch(Buffer_0, enc_key, true, true);
    sub_orams_[0]->InsertBatch(Buffer_1, enc_key, true, true);
  }

  capacity_ += a;

  // Phase 5: Simple Structural Scale Up/Down with Secondary Transfers
  if (scale_up) {
    int64_t old_y = sub_orams_[1]->Capacity();
    int64_t excess = std::max<int64_t>(0, static_cast<int64_t>(capacity_) - old_y);
    int64_t k_sec = 2 * excess;
    
    sub_orams_[0] = std::move(sub_orams_[1]);
    sub_orams_[1] = std::make_unique<PORam>(2 * old_y, val_len_, true);
    
    log_map_[0] = std::move(log_map_[1]);
    log_map_[1].clear();
    log_map_[1].resize((2 * old_y) + 1, {0, 0});
    
    if (k_sec > 0) {
        auto extract_keys_sec = [&](int sub_idx, int64_t k_count, int64_t T_count) {
            std::vector<Key> S_keys(T_count, 0);
            uint64_t count = 0;
            uint64_t cap = static_cast<uint64_t>(old_y); // the old sub_orams_[1] which is now sub_orams_[0]
            for (uint64_t i = 1; i <= cap; ++i) {
                bool is_real = (log_map_[sub_idx][i].logical_key != 0);
                bool take = is_real & (count < static_cast<uint64_t>(k_count));
                for (uint64_t j = 0; j < static_cast<uint64_t>(T_count); ++j) {
                    bool match = take & (count == j);
                    S_keys[j] = sn::obliv::ct_select<Key>(i, S_keys[j], match);
                }
                count = sn::obliv::ct_select(count + 1, count, take);
            }
            return S_keys;
        };
        std::vector<Key> extracted = extract_keys_sec(0, k_sec, k_sec);
        
        std::vector<std::pair<Key, bool>> keys_to_read;
        for (int64_t i = 0; i < k_sec; ++i) {
            keys_to_read.push_back({extracted[i], extracted[i] != 0});
        }
        
        std::vector<static_path_oram::Block> Buffer = sub_orams_[0]->ReadAndRemoveBatch(keys_to_read, enc_key);
        
        for (int64_t i = 0; i < k_sec; ++i) {
            uint64_t old_phys_k = Buffer[i].meta_.key_;
            bool is_valid = (old_phys_k != 0);
            
            uint64_t log_k = 0;
            uint64_t leaf_k = 0;
            for (uint64_t j = 1; j <= static_cast<uint64_t>(old_y); ++j) {
                bool match = is_valid && sn::obliv::ct_eq(j, old_phys_k);
                log_k = sn::obliv::ct_select(log_map_[0][j].logical_key, log_k, match);
                leaf_k = sn::obliv::ct_select(log_map_[0][j].leaf, leaf_k, match);
                log_map_[0][j].logical_key = sn::obliv::ct_select<uint64_t>(0, log_map_[0][j].logical_key, match);
                log_map_[0][j].leaf = sn::obliv::ct_select<uint64_t>(0, log_map_[0][j].leaf, match);
            }
            
            uint64_t new_phys_k = 0;
            for (uint64_t j = 1; j <= static_cast<uint64_t>(2 * old_y); ++j) {
                bool is_empty = (log_map_[1][j].logical_key == 0);
                bool select_this = is_valid && is_empty && (new_phys_k == 0);
                new_phys_k = sn::obliv::ct_select(j, new_phys_k, select_this);
                log_map_[1][j].logical_key = sn::obliv::ct_select(log_k, log_map_[1][j].logical_key, select_this);
                log_map_[1][j].leaf = sn::obliv::ct_select(leaf_k, log_map_[1][j].leaf, select_this);
            }
            
            Buffer[i].meta_.key_ = new_phys_k;
        }
        sub_orams_[1]->InsertBatch(Buffer, enc_key, true, true); // all_new = true
    }
  } else if (scale_down) {
    int64_t old_x = sub_orams_[0]->Capacity();
    int64_t deficit = std::max<int64_t>(0, old_x - static_cast<int64_t>(capacity_));
    int64_t k_sec = deficit;
    
    sub_orams_[1] = std::move(sub_orams_[0]);
    sub_orams_[0] = std::make_unique<PORam>(old_x / 2, val_len_, true);
    
    log_map_[1] = std::move(log_map_[0]);
    log_map_[0].clear();
    log_map_[0].resize((old_x / 2) + 1, {0, 0});

    if (k_sec > 0) {
        auto no_filter = [](Key phys_k) { return true; };
        std::vector<Key> extracted = sub_orams_[1]->ObliviousExtractValidKeys(k_sec, k_sec, no_filter);
        
        std::vector<std::pair<Key, bool>> keys_to_read;
        for (int64_t i = 0; i < k_sec; ++i) {
            keys_to_read.push_back({extracted[i], extracted[i] != 0});
        }
        
        std::vector<static_path_oram::Block> Buffer = sub_orams_[1]->ReadAndRemoveBatch(keys_to_read, enc_key);
        
        for (int64_t i = 0; i < k_sec; ++i) {
            uint64_t old_phys_k = Buffer[i].meta_.key_;
            bool is_valid = (old_phys_k != 0);
            
            uint64_t log_k = 0;
            uint64_t leaf_k = 0;
            for (uint64_t j = 1; j <= static_cast<uint64_t>(old_x); ++j) {
                bool match = is_valid && sn::obliv::ct_eq(j, old_phys_k);
                log_k = sn::obliv::ct_select(log_map_[1][j].logical_key, log_k, match);
                leaf_k = sn::obliv::ct_select(log_map_[1][j].leaf, leaf_k, match);
                log_map_[1][j].logical_key = sn::obliv::ct_select<uint64_t>(0, log_map_[1][j].logical_key, match);
                log_map_[1][j].leaf = sn::obliv::ct_select<uint64_t>(0, log_map_[1][j].leaf, match);
            }
            
            uint64_t new_phys_k = 0;
            for (uint64_t j = 1; j <= static_cast<uint64_t>(old_x / 2); ++j) {
                bool is_empty = (log_map_[0][j].logical_key == 0);
                bool select_this = is_valid && is_empty && (new_phys_k == 0);
                new_phys_k = sn::obliv::ct_select(j, new_phys_k, select_this);
                log_map_[0][j].logical_key = sn::obliv::ct_select(log_k, log_map_[0][j].logical_key, select_this);
                log_map_[0][j].leaf = sn::obliv::ct_select(leaf_k, log_map_[0][j].leaf, select_this);
            }
            
            Buffer[i].meta_.key_ = new_phys_k;
        }
        sub_orams_[0]->InsertBatch(Buffer, enc_key, true, true);
    }
  }

  if (B > original_B) {
      batch.resize(original_B);
  }

  if (sub_orams_[0]) sub_orams_[0]->FlushEpoch();
  if (sub_orams_[1]) sub_orams_[1]->FlushEpoch();
}

} // namespace dyno::dynamic_stepping_path_oram
