#include "oram.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <future>
#include <algorithm>

#include "src/utils/crypto.h"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"

namespace dyno::dynamic_stepping_path_oram {

ORam::ORam(int starting_size_power_of_two, size_t val_len)
    : capacity_(1UL << starting_size_power_of_two),
      val_len_(val_len),
      size_(1UL << (starting_size_power_of_two)) {
  auto base_cap = capacity_ >> 1;
  for (int i = 0; i < 2; ++i)
    sub_orams_[i] = std::make_unique<PORam>(base_cap << i, val_len, true);
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
    auto bl = sub_orams_[i]->ReadAndRemove(0, k, enc_key, is_real);
    
    res.key_ = sn::obliv::ct_select<Key>(bl.meta_.key_, res.key_, is_real);
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
    auto bl = sub_orams_[i]->Read(0, k, enc_key, is_real);
    
    res.key_ = sn::obliv::ct_select<Key>(bl.meta_.key_, res.key_, is_real);
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
  auto idx = SubOramIndex(k);
  auto start_accesses = SubORamsMemoryAccessCountSum();
  auto start_bytes = SubORamsMemoryBytesMovedTotalSum();
  
  for (int i = 0; i < 2; ++i) {
    if (sub_orams_[i] == nullptr)
      continue;

    bool is_real = sn::obliv::ct_eq<uint8_t>(i, idx);
    
    // We must pass a static_path_oram::Block to SonicORamAdapter
    static_path_oram::Block b(0, k);
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
  if (capacity_ == 1)
    return 1;
  
  bool cond1 = sn::obliv::ct_gt<Key>(k, sub_orams_[0]->Capacity());
  bool cond2 = sn::obliv::ct_le<Key>(k, capacity_ - sub_orams_[0]->Capacity());
  bool cond = cond1 | cond2;
  
  return sn::obliv::ct_select<uint8_t>(1, 0, cond);
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

void ORam::ExecuteBatch(std::vector<BatchOperation>& batch, crypto::Key enc_key) {
  size_t B = batch.size();
  if (B == 0) return;

  size_t original_inserts = 0;
  size_t original_reads = 0;
  size_t original_deletes = 0;
  for (auto& op : batch) {
    if (op.type == OpType::Insert) original_inserts++;
    else if (op.type == OpType::Delete) original_deletes++;
    else original_reads++;
  }

  // Pre-allocate values so we can obliviously swap them in constant time
  for (auto& op : batch) {
    if (!op.val && val_len_ > 0) {
      op.val = std::make_unique<uint8_t[]>(val_len_);
    }
  }

  struct OblivElem {
    Key key;
    uint32_t seq;
    bool is_dummy;
    uint8_t op_type; // 0=Insert, 1=Search, 2=Delete
  };

  std::vector<OblivElem> elems(B);
  for (size_t i = 0; i < B; ++i) {
    elems[i].key = batch[i].key;
    elems[i].seq = static_cast<uint32_t>(i);
    elems[i].is_dummy = false;
    if (batch[i].type == OpType::Insert) elems[i].op_type = 0;
    else if (batch[i].type == OpType::Search) elems[i].op_type = 1;
    else elems[i].op_type = 2;
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
    sn::obliv::ct_set_ref(elems[i].is_dummy, true, same_key);
  }
  for (size_t i = 0; i < B; ++i) {
    bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(2));
    sn::obliv::ct_set_ref(elems[i].is_dummy, true, is_delete);
  }

  // Phase 3: O-Sort (Group by OpType, then Dummy)
  auto comp2 = [](const OblivElem& a, const OblivElem& b) {
    bool type_eq = sn::obliv::ct_eq(a.op_type, b.op_type);
    bool type_lt = sn::obliv::ct_lt(a.op_type, b.op_type);
    bool dummy_lt = (!a.is_dummy) && b.is_dummy;
    return sn::obliv::ct_select(dummy_lt, type_lt, type_eq);
  };
  sn::sortshuffle::ser::bitonic::detail::bitonic_sort_impl(elems.data(), B, key_ext, comp2, hook);

  // Calculate Real Net Growth Obliviously
  size_t real_I = 0, real_DS = 0, real_DL = 0;
  for (size_t i = 0; i < B; ++i) {
    bool is_real = !elems[i].is_dummy;
    bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(0));
    bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(2));
    uint8_t idx = SubOramIndex(elems[i].key);
    
    real_I += sn::obliv::ct_select<size_t>(1, 0, is_real && is_insert);
    real_DS += sn::obliv::ct_select<size_t>(1, 0, is_real && is_delete && (idx == 0));
    real_DL += sn::obliv::ct_select<size_t>(1, 0, is_real && is_delete && (idx == 1));
  }

  // Dispatch exactly to Public Bounds
  std::vector<Block> inserts;
  std::vector<std::pair<Key, bool>> small_reads, large_reads;

  for (size_t i = 0; i < original_inserts; ++i) {
    Block b;
    b.key_ = batch[i].key;
    if (batch[i].val) {
      b.val_ = std::make_unique<uint8_t[]>(val_len_);
      std::copy(batch[i].val.get(), batch[i].val.get() + val_len_, b.val_.get());
    }
    inserts.push_back(std::move(b));
  }

  for (size_t i = original_inserts; i < B; ++i) {
    Key k = batch[i].key;
    bool is_real = !elems[i].is_dummy;
    uint8_t idx = SubOramIndex(k);
    small_reads.push_back({k, is_real && (idx == 0)});
    large_reads.push_back({k, is_real && (idx == 1)});
  }

  if (sub_orams_[0]) sub_orams_[0]->ReadBatch(small_reads, enc_key);
  if (sub_orams_[1]) sub_orams_[1]->ReadBatch(large_reads, enc_key);

  if (sub_orams_[1]) {
    std::vector<static_path_oram::Block> sn_inserts;
    for (auto& b : inserts) sn_inserts.push_back(static_path_oram::Block(0, b.key_));
    sub_orams_[1]->InsertBatch(sn_inserts, enc_key);
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
    // 1. Pad T to power of 2 for OCompact
    int64_t T_pow2 = 1;
    while (T_pow2 < T) T_pow2 *= 2;

    // 2. Deterministic Batch Extraction into Enclave
    std::vector<std::pair<Key, bool>> S_keys;
    std::vector<std::pair<Key, bool>> L_keys;
    for (int64_t i = 0; i < T; ++i) {
        S_keys.push_back({(ptr_S_ + i) % sub_orams_[0]->Capacity() + 1, true});
        L_keys.push_back({(ptr_L_ + i) % sub_orams_[1]->Capacity() + 1, true});
    }

    auto BufferS = sub_orams_[0]->ReadAndRemoveBatch(S_keys, enc_key);
    auto BufferL = sub_orams_[1]->ReadAndRemoveBatch(L_keys, enc_key);

    // Pad buffers to T_pow2 with pure dummies
    for (uint64_t i = T; i < T_pow2; ++i) {
        BufferS.emplace_back(true);
        BufferL.emplace_back(true);
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

    // 5. Flush to Stashes
    sub_orams_[0]->InsertBatch(BufferS, enc_key);
    sub_orams_[1]->InsertBatch(BufferL, enc_key);

    // 6. Address Translation & SONIC Native Cleanup
    ptr_S_ += std::max(static_cast<int64_t>(0), k_transfer);
    ptr_L_ += std::max(static_cast<int64_t>(0), -k_transfer);
    capacity_ += k_transfer;
  }

  // Phase 5: Cascading Resizing & Secondary Transfer
  if (scale_up) {
    sub_orams_[0] = std::move(sub_orams_[1]);
    sub_orams_[1] = std::make_unique<PORam>(2 * capacity_, val_len_, true);
  } else if (scale_down) {
    sub_orams_[1] = std::move(sub_orams_[0]);
    sub_orams_[0] = std::make_unique<PORam>(capacity_ / 2, val_len_, true);
  }
}

} // namespace dyno::dynamic_stepping_path_oram
