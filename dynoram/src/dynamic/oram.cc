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

#include "src/utils/crypto.h"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"

namespace dyno::dynamic_stepping_path_oram {

ORam::ORam(int starting_size_power_of_two, size_t val_len)
    : capacity_(1UL << starting_size_power_of_two),
      val_len_(val_len),
      size_(1UL << (starting_size_power_of_two)) {
  auto base_cap = capacity_;
  sub_orams_[0] = std::make_unique<PORam>(base_cap, val_len, true);
  sub_orams_[1] = std::make_unique<PORam>(base_cap * 2, val_len, true);
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

// Returns 0-value of Val if nothing found.
Block ORam::ReadAndRemove(Key k, crypto::Key enc_key) {
  assert(1 <= k && k <= capacity_);
  Block res;
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  auto idx = SubOramIndex(k);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  for (int i = 0; i < 2; ++i) {
    if (i == 0 && (sub_orams_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;

    bool is_real = sn::obliv::ct_eq<uint8_t>(i, idx);
    uint64_t phys_k = PhysicalKey(k, i);
    auto bl = sub_orams_[i]->ReadAndRemove(0, phys_k, enc_key, is_real);
    
    // We must restore the logical key in the result
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

// Returns 0-value of Val if nothing found.
Block ORam::Read(Key k, crypto::Key enc_key) {
  assert(1 <= k && k <= capacity_);
  Block res;
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  auto idx = SubOramIndex(k);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  for (int i = 0; i < 2; ++i) {
    if (i == 0 && (sub_orams_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;

    bool is_real = sn::obliv::ct_eq<uint8_t>(i, idx);
    uint64_t phys_k = PhysicalKey(k, i);
    auto bl = sub_orams_[i]->Read(0, phys_k, enc_key, is_real);
    
    res.key_ = sn::obliv::ct_select<Key>(k, res.key_, is_real && (bl.meta_.key_ != 0));
    if (bl.val_) {
        sn::obliv::ct_select_array(res.val_.get(), bl.val_.get(), res.val_.get(), val_len_, is_real);
    }
  }
  memory_access_count_ += SubORamsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubORamsMemoryBytesMovedTotalSum() - start_bytes;
  if (k <= 20) {
    std::cout << "[DEBUG] ORam::Read k=" << k << " idx=" << (int)idx << " phys_k_S=" << PhysicalKey(k,0) << " phys_k_L=" << PhysicalKey(k,1)
              << " res.key_=" << res.key_;
    if (res.val_) std::cout << " val[0]=" << (int)res.val_.get()[0];
    std::cout << " ptr_S_=" << ptr_S_ << " ptr_L_=" << ptr_L_ << " capacity_=" << capacity_
              << " cap_S=" << (sub_orams_[0] ? sub_orams_[0]->Capacity() : 0) << " cap_L=" << (sub_orams_[1] ? sub_orams_[1]->Capacity() : 0)
              << std::endl;
  }
  return res;
}

void ORam::Insert(Key k, Val v, crypto::Key enc_key) {
  assert(1 <= k && k <= capacity_);
  auto idx = SubOramIndex(k);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  
  for (int i = 0; i < 2; ++i) {
    if (sub_orams_[i] == nullptr)
      continue;

    bool is_real = sn::obliv::ct_eq<uint8_t>(i, idx);
    
    // We must pass a static_path_oram::Block to SonicORamAdapter
    uint64_t phys_k = PhysicalKey(k, i);
    static_path_oram::Block b(0, phys_k);
    if (v) {
        b.val_ = std::make_unique<uint8_t[]>(val_len_);
        std::copy(v.get(), v.get() + val_len_, b.val_.get());
    }
    
    sub_orams_[i]->Insert(std::move(b), enc_key, is_real);
  }
  ++size_;
  memory_access_count_ += SubORamsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubORamsMemoryBytesMovedTotalSum() - start_bytes;
}

uint8_t ORam::SubOramIndex(Key k) {
  assert(1 <= k && k <= capacity_);
  if (capacity_ == 1 || sub_orams_[0] == nullptr)
    return 1;
  
  bool cond1 = sn::obliv::ct_ge<Key>(k, ptr_S_ + 1);
  bool cond2 = sn::obliv::ct_le<Key>(k, ptr_S_ + sub_orams_[0]->Capacity());
  bool in_small = cond1 & cond2;
  
  return sn::obliv::ct_select<uint8_t>(0, 1, in_small);
}

uint64_t ORam::PhysicalKey(Key k, uint8_t sub_oram_idx) {
    if (k == 0) return 0;
    int64_t cap_s = sub_orams_[0] ? sub_orams_[0]->Capacity() : 1;
    int64_t cap_l = sub_orams_[1] ? sub_orams_[1]->Capacity() : 1;
    
    uint64_t p_small = ((k - 1) % cap_s) + 1;
    uint64_t p_large = ((k - 1) % cap_l) + 1;
    
    return sn::obliv::ct_select<uint64_t>(p_small, p_large, sn::obliv::ct_eq<uint8_t>(sub_oram_idx, 0));
}

uint64_t ORam::ReconstructLogicalKeySmallOblivious(uint64_t phys_k) {
    if (phys_k == 0) return 0;
    uint64_t cap_s = sub_orams_[0] ? sub_orams_[0]->Capacity() : 1;
    uint64_t m = (ptr_S_ + cap_s - phys_k) / cap_s;
    uint64_t res = phys_k + m * cap_s;
    return res;
}

uint64_t ORam::ReconstructLogicalKeyLargeOblivious(uint64_t phys_k) {
    if (phys_k == 0) return 0;
    uint64_t cap_l = sub_orams_[1] ? sub_orams_[1]->Capacity() : 1;
    uint64_t m = (ptr_L_ + cap_l - phys_k) / cap_l;
    uint64_t res = phys_k + m * cap_l;
    return res;
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

  size_t B = 1;
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
  for (auto& op : batch) {
    if (op.type == OpType::Insert) original_inserts++;
  }

  // Pre-allocate values so we can obliviously swap them in constant time
  for (auto& op : batch) {
    if (!op.val && val_len_ > 0) {
      op.val = std::make_unique<uint8_t[]>(val_len_);
    }
  }

  struct alignas(8) OblivElem {
    Key key;
    uint32_t seq;
    bool is_dummy;
    uint8_t op_type; // 0=Insert, 1=Search, 2=Delete, 3=Update
  };


  
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

  // Phase 1: O-Sort (Group by Key, then Seq)
  auto comp1 = [](const OblivElem& a, const OblivElem& b) {
    bool key_eq = sn::obliv::ct_eq(a.key, b.key);
    bool key_lt = sn::obliv::ct_lt(a.key, b.key);
    bool seq_lt = sn::obliv::ct_lt(a.seq, b.seq);
    return sn::obliv::ct_select(seq_lt, key_lt, key_eq);
  };
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(elems.data(), B, key_ext, comp1, hook);

  // Phase 2: O-Scan (Collapse)
  for (size_t i = 0; i < B - 1; ++i) {
    bool same_key = sn::obliv::ct_eq(elems[i].key, elems[i+1].key);
    
    bool is_i_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
    bool is_i_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
    bool is_i_update = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Update));
    bool is_next_search = sn::obliv::ct_eq(elems[i+1].op_type, static_cast<uint8_t>(OpType::Search));
    
    bool is_next_update = sn::obliv::ct_eq(elems[i+1].op_type, static_cast<uint8_t>(OpType::Update));
    
    bool transform_to_insert_with_swap = same_key & is_i_insert & is_next_search;
    bool transform_to_insert_no_swap = same_key & is_i_insert & is_next_update;
    bool transform_to_delete = same_key & is_i_delete & is_next_search;
    bool transform_to_update = same_key & is_i_update & is_next_search;
    
    elems[i+1].op_type = sn::obliv::ct_select<uint8_t>(static_cast<uint8_t>(OpType::Search), elems[i+1].op_type, transform_to_insert_with_swap | transform_to_insert_no_swap);
    elems[i+1].op_type = sn::obliv::ct_select<uint8_t>(static_cast<uint8_t>(OpType::Delete), elems[i+1].op_type, transform_to_delete);
    elems[i+1].op_type = sn::obliv::ct_select<uint8_t>(static_cast<uint8_t>(OpType::Update), elems[i+1].op_type, transform_to_update);
    
    bool do_swap = transform_to_insert_with_swap | transform_to_update;
    if (val_len_ > 0) {
        sn::obliv::ct_swap_array(batch[i].val.get(), batch[i+1].val.get(), val_len_, do_swap);
    }
    
    sn::obliv::ct_set_ref(elems[i].is_dummy, true, same_key);
  }

  // Calculate Real Net Growth Obliviously BEFORE Deletes are forced to dummies
  size_t real_I = 0, real_DS = 0, real_DL = 0;
  for (size_t i = 0; i < B; ++i) {
    bool is_real = !elems[i].is_dummy;
    bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
    bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
    uint8_t idx = SubOramIndex(elems[i].key);
    
    real_I += sn::obliv::ct_select<size_t>(1, 0, is_real && is_insert);
    real_DS += sn::obliv::ct_select<size_t>(1, 0, is_real && is_delete && (idx == 0));
    real_DL += sn::obliv::ct_select<size_t>(1, 0, is_real && is_delete && (idx == 1));
  }

  // Phase 3: O-Sort (Group by OpType, then Dummy)
  auto comp2 = [](const OblivElem& a, const OblivElem& b) {
    bool type_eq = sn::obliv::ct_eq(a.op_type, b.op_type);
    bool type_lt = sn::obliv::ct_lt(a.op_type, b.op_type);
    
    bool dummy_eq = sn::obliv::ct_eq(a.is_dummy, b.is_dummy);
    bool dummy_lt = (!a.is_dummy) && b.is_dummy;
    
    bool seq_lt = sn::obliv::ct_lt(a.seq, b.seq);
    
    bool eq_so_far = type_eq & dummy_eq;
    bool lt_so_far = sn::obliv::ct_select(dummy_lt, type_lt, type_eq);
    
    return sn::obliv::ct_select(seq_lt, lt_so_far, eq_so_far);
  };
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(elems.data(), B, key_ext, comp2, hook);

  std::vector<Block> inserts;
  std::vector<SonicORamAdapter::AccessOp> small_ops, large_ops;
  size_t original_accesses = original_B - original_inserts;

  for (size_t i = original_accesses; i < original_B; ++i) {
    Block b;
    bool is_real = !elems[i].is_dummy;
    b.key_ = sn::obliv::ct_select<uint64_t>(batch[i].key, 0, is_real);
    
    if (batch[i].val) {
      b.val_ = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[i].val.get(), batch[i].val.get() + val_len_, b.val_.get());
    }
    inserts.push_back(std::move(b));
  }

  for (size_t i = 0; i < original_accesses; ++i) {
    Key k = batch[i].key;
    bool is_real = !elems[i].is_dummy;
    uint8_t idx = SubOramIndex(k);
    uint8_t op_type = static_cast<uint8_t>(batch[i].type);
    
    bool is_search = sn::obliv::ct_eq(op_type, static_cast<uint8_t>(OpType::Search));
    bool is_delete = sn::obliv::ct_eq(op_type, static_cast<uint8_t>(OpType::Delete));
    bool is_update = sn::obliv::ct_eq(op_type, static_cast<uint8_t>(OpType::Update));
    
    bool is_access = is_search || is_delete || is_update;
    
    uint64_t phys_k_small = PhysicalKey(k, 0);
    SonicORamAdapter::AccessOp op;
    op.key = phys_k_small;
    op.op_type = op_type;
    if (batch[i].val) {
      op.val = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[i].val.get(), batch[i].val.get() + val_len_, op.val.get());
    }
    
    op.is_real = is_real && is_access && (idx == 0);
    small_ops.push_back(std::move(op));

    uint64_t phys_k_large = PhysicalKey(k, 1);
    SonicORamAdapter::AccessOp op_l;
    op_l.key = phys_k_large;
    op_l.op_type = op_type;
    if (batch[i].val) {
      op_l.val = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[i].val.get(), batch[i].val.get() + val_len_, op_l.val.get());
    }
    op_l.is_real = is_real && is_access && (idx == 1);
    large_ops.push_back(std::move(op_l));
  }

  if (sub_orams_[0]) {
      std::cout << "[DEBUG] Phase 2: small sub_oram ReadBatch" << std::endl;
      auto read_results = sub_orams_[0]->ReadBatch(small_ops, enc_key, steady_state);
      std::cout << "[DEBUG] Phase 2: small sub_oram ReadBatch done" << std::endl;
      // Copy read results back to batch for Search operations
      for (size_t i = 0; i < original_accesses; ++i) {
          if (small_ops[i].is_real && sn::obliv::ct_eq(small_ops[i].op_type, static_cast<uint8_t>(OpType::Search))) {
              batch[i].result.key_ = read_results[i].meta_.key_;
              if (read_results[i].val_) {
                  batch[i].result.val_ = std::make_unique<uint8_t[]>(val_len_);
                  std::copy(read_results[i].val_.get(), read_results[i].val_.get() + val_len_, batch[i].result.val_.get());
              }
          }
      }
  }
  if (sub_orams_[1]) {
      std::cout << "[DEBUG] Phase 2: large sub_oram ReadBatch" << std::endl;
      auto read_results = sub_orams_[1]->ReadBatch(large_ops, enc_key, steady_state);
      std::cout << "[DEBUG] Phase 2: large sub_oram ReadBatch done" << std::endl;
      std::cout << "[DEBUG] Post-Phase 2: large_ops processing start" << std::endl;
      for (size_t i = 0; i < original_accesses; ++i) {
          if (large_ops[i].is_real && sn::obliv::ct_eq(large_ops[i].op_type, static_cast<uint8_t>(OpType::Search))) {
              batch[i].result.key_ = read_results[i].meta_.key_;
              if (read_results[i].val_) {
                  batch[i].result.val_ = std::make_unique<uint8_t[]>(val_len_);
                  std::copy(read_results[i].val_.get(), read_results[i].val_.get() + val_len_, batch[i].result.val_.get());
              }
          }
      }
      std::cout << "[DEBUG] Post-Phase 2: large_ops processing end" << std::endl;
  }

  if (sub_orams_[1]) {
    std::cout << "[DEBUG] Pre-Phase 3: sn_inserts populating" << std::endl;
    std::vector<static_path_oram::Block> sn_inserts;
    std::cout << "[DEBUG] Reserving sn_inserts..." << std::endl;
    sn_inserts.reserve(inserts.size());
    std::cout << "[DEBUG] Reserved sn_inserts!" << std::endl;
    int idx = 0;
    for (auto& b : inserts) {
        if (idx % 100 == 0 || idx > 510) {
            std::cout << "  [DEBUG] Loop iter " << idx << " start, key=" << b.key_ << std::endl;
        }
        uint64_t phys_k = PhysicalKey(b.key_, 1);
        static_path_oram::Block new_b(static_cast<static_path_oram::Pos>(0), static_cast<static_path_oram::Key>(phys_k));
        if (b.val_) {
            new_b.val_ = std::move(b.val_);
        }
        sn_inserts.push_back(std::move(new_b));
        idx++;
    }
    std::cout << "[DEBUG] Pre-Phase 3: sn_inserts populated" << std::endl;
    std::cout << "[DEBUG] Phase 3: large sub_oram InsertBatch" << std::endl;
    sub_orams_[1]->InsertBatch(sn_inserts, enc_key, steady_state);
    std::cout << "[DEBUG] Phase 3: large sub_oram InsertBatch done" << std::endl;
  }

  // Phase 4: Oblivious Net Growth & Boundary Checking
  int64_t a = real_I - real_DS - real_DL;
  int64_t x = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  int64_t y = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;

  int64_t target_small = x - a;
  int64_t target_large = y + 2*a;

  int64_t k_transfer = a - real_DS;
  bool scale_up = false, scale_down = false;
  int64_t T = 0;

  if (target_small <= 0) {
    k_transfer = x - real_DS;
    T = x;
    scale_up = true;
  } else if (target_large <= 0) {
    k_transfer = -(y - real_DL + real_I);
    int64_t total_deletes = real_DS + real_DL;
    int64_t tightened_limit = std::min(static_cast<int64_t>(real_I), total_deletes - (y / 2));
    T = y + std::max(static_cast<int64_t>(0), tightened_limit);
    scale_down = true;
  } else {
    int64_t B_size = static_cast<int64_t>(batch.size());
    T = std::max(std::abs(B_size - a), std::abs(a));
  }

  // Phase 4: Oblivious Buffer Swapping & Transfer (via SONIC Global Stashes)
  if (sub_orams_[0] && sub_orams_[1] && T > 0) {
    std::cout << "[DEBUG] Phase 4 Start. T=" << T << " k_transfer=" << k_transfer << std::endl;
    // 1. Pad T to power of 2 for OCompact
    int64_t T_pow2 = 1;
    while (T_pow2 < T) T_pow2 *= 2;

    auto filter_S = [this, k_transfer](Key phys_k) {
        if (k_transfer <= 0) return false;
        return phys_k <= static_cast<Key>(k_transfer);
    };
    
    auto filter_L = [this, k_transfer](Key phys_k) {
        if (k_transfer >= 0) return false;
        Key log_k = ReconstructLogicalKeyLargeOblivious(phys_k);
        int64_t diff_s_new = static_cast<int64_t>(log_k - 1) - static_cast<int64_t>(ptr_S_ + k_transfer);
        return diff_s_new >= 0 && diff_s_new < -k_transfer;
    };

    std::vector<Key> S_extracted = sub_orams_[0]->ObliviousExtractValidKeys(std::max<int64_t>(0, k_transfer), T, filter_S);
    std::vector<Key> L_extracted = sub_orams_[1]->ObliviousExtractValidKeys(std::max<int64_t>(0, -k_transfer), T, filter_L);
    
    std::cout << "[DEBUG] Phase 4 extracted keys" << std::endl;

    // 2. Read into global Buffers
    std::vector<std::pair<Key, bool>> keys_S, keys_L;
    for (int64_t i = 0; i < T; ++i) {
        keys_S.push_back({S_extracted[i], S_extracted[i] != 0});
        keys_L.push_back({L_extracted[i], L_extracted[i] != 0});
    }

    std::cout << "[DEBUG] Phase 4 extracting BufferS" << std::endl;
    std::vector<static_path_oram::Block> BufferS = sub_orams_[0]->ReadAndRemoveBatch(keys_S, enc_key);
    std::cout << "[DEBUG] Phase 4 extracting BufferL" << std::endl;
    std::vector<static_path_oram::Block> BufferL = sub_orams_[1]->ReadAndRemoveBatch(keys_L, enc_key);
    std::cout << "[DEBUG] Phase 4 Buffers extracted" << std::endl;

    // Pad buffers to T_pow2 with pure dummies
    for (uint64_t i = T; i < T_pow2; ++i) {
        BufferS.emplace_back(true);
        if (val_len_ > 0) {
            BufferS.back().val_ = std::make_unique<uint8_t[]>(val_len_);
            std::fill(BufferS.back().val_.get(), BufferS.back().val_.get() + val_len_, 0);
        }
        BufferL.emplace_back(true);
        if (val_len_ > 0) {
            BufferL.back().val_ = std::make_unique<uint8_t[]>(val_len_);
            std::fill(BufferL.back().val_.get(), BufferL.back().val_.get() + val_len_, 0);
        }
    }

    // Translate physical keys back to logical keys BEFORE swapping
    for (uint64_t i = 0; i < T; ++i) {
        if (BufferS[i].meta_.key_ != 0) {
            BufferS[i].meta_.key_ = ReconstructLogicalKeySmallOblivious(BufferS[i].meta_.key_);
        }
        if (BufferL[i].meta_.key_ != 0) {
            BufferL[i].meta_.key_ = ReconstructLogicalKeyLargeOblivious(BufferL[i].meta_.key_);
        }
    }

    // 3. Oblivious Compaction
    OCompact(BufferS, 0, T_pow2, val_len_);
    OCompact(BufferL, 0, T_pow2, val_len_);

    // 4. Double-Oblivious Swapping
    for (uint64_t i = 0; i < T; ++i) {
        bool swap_pos = (k_transfer > 0) && (i < static_cast<uint64_t>(k_transfer));
        bool swap_neg = (k_transfer < 0) && (i < static_cast<uint64_t>(-k_transfer));
        bool should_swap = swap_pos | swap_neg;
        ObliviousSwapBlock(BufferS[i], BufferL[i], should_swap, val_len_);
    }

    // Truncate padded dummies before inserting
    BufferS.erase(BufferS.begin() + T, BufferS.end());
    BufferL.erase(BufferL.begin() + T, BufferL.end());

    // 5. Address Translation (MUST happen before physical key mapping)
    ptr_S_ += std::max(static_cast<int64_t>(0), k_transfer);
    ptr_L_ += std::max(static_cast<int64_t>(0), -k_transfer);
    capacity_ += a;

    // 6. Translate logical keys back to physical keys for destination AFTER swapping
    for (uint64_t i = 0; i < T; ++i) {
        if (BufferS[i].meta_.key_ != 0) BufferS[i].meta_.key_ = PhysicalKey(BufferS[i].meta_.key_, 0);
        if (BufferL[i].meta_.key_ != 0) BufferL[i].meta_.key_ = PhysicalKey(BufferL[i].meta_.key_, 1);
    }

    // 7. Flush to Stashes
    std::cout << "[DEBUG] Phase 4 inserting BufferS" << std::endl;
    sub_orams_[0]->InsertBatch(BufferS, enc_key, true);
    std::cout << "[DEBUG] Phase 4 inserting BufferL" << std::endl;
    sub_orams_[1]->InsertBatch(BufferL, enc_key, true);
    std::cout << "[DEBUG] Phase 4 complete. capacity_=" << capacity_ << " ptr_S_=" << ptr_S_ << " ptr_L_=" << ptr_L_ << std::endl;
  }

  // Phase 5: Cascading Resizing & Secondary Transfer
  if (scale_up) {
    // Phase 5 scale_up: 
    // The old ORAM L (now slightly overstuffed) becomes the new ORAM S.
    // A new double-sized ORAM L is allocated.
    int64_t old_y = sub_orams_[1]->Capacity();
    int64_t excess = std::max<int64_t>(0, static_cast<int64_t>(capacity_) - old_y);
    int64_t k_sec = 2 * excess;

    uint64_t old_ptr_S = ptr_L_; // old_ptr_L becomes the base for new S

    sub_orams_[0] = std::move(sub_orams_[1]);
    sub_orams_[1] = std::make_unique<PORam>(2 * old_y, val_len_, true);
    std::swap(ptr_S_, ptr_L_);
    ptr_S_ += k_sec; // Advance ptr_S_ to maintain the N-j and 2j invariant!
    
    std::vector<std::pair<Key, bool>> keys_to_move;
    for (uint64_t i = 1; i <= static_cast<uint64_t>(k_sec); ++i) {
        Key log_k = old_ptr_S + i;
        Key phys_k = PhysicalKey(log_k, 0); // They are currently in the new S
        keys_to_move.push_back({phys_k, true});
    }
    
    if (!keys_to_move.empty()) {
        std::cout << "[DEBUG] Phase 5 transferring " << keys_to_move.size() << " keys to new L" << std::endl;
        std::cout << "[DEBUG] Phase 5 state: ptr_S_=" << ptr_S_ << " ptr_L_=" << ptr_L_ 
                  << " cap_S=" << sub_orams_[0]->Capacity() << " cap_L=" << sub_orams_[1]->Capacity() 
                  << " old_ptr_S=" << old_ptr_S << std::endl;
        for (size_t dbg = 0; dbg < keys_to_move.size(); ++dbg) {
            std::cout << "[DEBUG] Phase 5 extract key[" << dbg << "]: phys_k=" << keys_to_move[dbg].first << std::endl;
        }
        auto extracted_blocks = sub_orams_[0]->ReadAndRemoveBatch(keys_to_move, enc_key);
        
        for (size_t i = 0; i < extracted_blocks.size(); ++i) {
            auto& b = extracted_blocks[i];
            std::cout << "[DEBUG] Phase 5 extracted[" << i << "]: meta_.key_=" << b.meta_.key_ 
                      << " meta_.pos_=" << b.meta_.pos_;
            if (b.val_) std::cout << " val[0]=" << (int)b.val_.get()[0];
            std::cout << std::endl;
        }
        
        // Translate back to logical, then to L's physical keys
        for (size_t i = 0; i < extracted_blocks.size(); ++i) {
            auto& b = extracted_blocks[i];
            if (b.meta_.key_ != 0) {
                Key log_k = old_ptr_S + i + 1; // Uniquely known
                Key new_phys = PhysicalKey(log_k, 1);
                std::cout << "[DEBUG] Phase 5 remap[" << i << "]: old_phys=" << b.meta_.key_ << " -> log_k=" << log_k << " -> new_phys=" << new_phys << std::endl;
                b.meta_.key_ = new_phys;
            }
        }
        sub_orams_[1]->InsertBatch(extracted_blocks, enc_key, true);
    }
    std::cout << "[DEBUG] Phase 5 scale_up complete. capacity_=" << capacity_ << " ptr_S_=" << ptr_S_ << " ptr_L_=" << ptr_L_ 
              << " cap_S=" << sub_orams_[0]->Capacity() << " cap_L=" << sub_orams_[1]->Capacity() << std::endl;
  } else if (scale_down) {
    int64_t old_x = sub_orams_[0]->Capacity();
    int64_t deficit = std::max<int64_t>(0, old_x - static_cast<int64_t>(capacity_));
    int64_t k_sec = deficit;
    
    std::swap(ptr_S_, ptr_L_);
    ptr_S_ -= k_sec; // Shift pointer backwards to absorb deficit
    
    std::vector<std::pair<Key, bool>> keys_to_move;
    for (uint64_t i = 1; i <= static_cast<uint64_t>(k_sec); ++i) {
        Key log_k = ptr_S_ + i;
        Key phys_k = PhysicalKey(log_k, 1); // They are currently in the new L
        keys_to_move.push_back({phys_k, true});
    }
    
    if (!keys_to_move.empty()) {
        std::cout << "[DEBUG] Phase 5 transferring " << keys_to_move.size() << " keys to new S" << std::endl;
        auto extracted_blocks = sub_orams_[1]->ReadAndRemoveBatch(keys_to_move, enc_key);
        
        // Translate back to logical, then to S's physical keys
        for (size_t i = 0; i < extracted_blocks.size(); ++i) {
            auto& b = extracted_blocks[i];
            if (b.meta_.key_ != 0) {
                Key log_k = ptr_S_ + i + 1; // Uniquely known
                b.meta_.key_ = PhysicalKey(log_k, 0);
            }
        }
        sub_orams_[0]->InsertBatch(extracted_blocks, enc_key, true);
    }
    
    sub_orams_[1] = std::move(sub_orams_[0]);
    sub_orams_[0] = std::make_unique<PORam>(old_x / 2, val_len_, true);
    std::cout << "[DEBUG] Phase 5 scale_down complete" << std::endl;
  }

  if (B > original_B) {
      batch.resize(original_B);
  }
}

} // namespace dyno::dynamic_stepping_path_oram
