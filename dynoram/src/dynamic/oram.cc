#include "oram.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "src/utils/crypto.h"
#include "sonic/obliv/ops/core_ops.hpp"

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
    sn::obliv::ct_swap(cond, a.meta_.key_, b.meta_.key_);
    sn::obliv::ct_swap(cond, a.meta_.pos_, b.meta_.pos_);
    for (size_t i = 0; i < val_len; ++i) {
        sn::obliv::ct_swap(cond, a.val_.get()[i], b.val_.get()[i]);
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
  // Phase 1: Batch Preprocessing (Collapsed)
  std::map<Key, BatchOperation> collapsed;
  for (auto& op : batch) {
    if (op.type == OpType::Delete) {
      collapsed.erase(op.key); 
    } else {
      collapsed[op.key] = std::move(op);
    }
  }

  // Phase 2: Concurrent Client Execution
  size_t I = 0, DS = 0, DL = 0;
  std::vector<Block> inserts;
  std::vector<std::pair<Key, bool>> small_reads, large_reads;

  for (auto& [k, op] : collapsed) {
    if (op.type == OpType::Insert) {
      ++I;
      Block b;
      b.key_ = k;
      if (op.val) {
        b.val_ = std::make_unique<uint8_t[]>(val_len_);
        std::copy(op.val.get(), op.val.get() + val_len_, b.val_.get());
      }
      inserts.push_back(std::move(b));
    } else {
      uint8_t idx = SubOramIndex(k);
      
      small_reads.push_back({k, idx == 0});
      large_reads.push_back({k, idx == 1});

      if (op.type == OpType::Delete) {
        if (idx == 0) DS++; 
        else DL++;
      }
    }
  }

  // Dispatch reads/deletes concurrently to BOTH tiers to hide location
  if (sub_orams_[0]) sub_orams_[0]->ReadBatch(small_reads, enc_key);
  if (sub_orams_[1]) sub_orams_[1]->ReadBatch(large_reads, enc_key);

  // Directly append inserts to Slarge
  if (sub_orams_[1]) {
    std::vector<static_path_oram::Block> sn_inserts;
    for (auto& b : inserts) sn_inserts.push_back(static_path_oram::Block(0, b.key_));
    sub_orams_[1]->InsertBatch(sn_inserts, enc_key);
  }

  // Phase 3: Oblivious Net Growth & Boundary Checking
  int64_t a = I - DS - DL;
  int64_t x = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  int64_t y = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;

  int64_t target_small = x - a;
  int64_t target_large = y + 2*a;

  int64_t k_transfer = a - DS;
  bool scale_up = false, scale_down = false;
  int64_t T = 0;

  if (target_small <= 0) {
    k_transfer = x - DS;
    T = x;
    scale_up = true;
  } else if (target_large <= 0) {
    k_transfer = -(y - DL + I);
    int64_t total_deletes = DS + DL;
    int64_t tightened_limit = std::min(static_cast<int64_t>(I), total_deletes - (y / 2));
    T = y + std::max(static_cast<int64_t>(0), tightened_limit);
    scale_down = true;
  } else {
    int64_t B = batch.size();
    T = std::max(std::abs(B - a), std::abs(a));
  }

  // Phase 4: Oblivious Buffer Swapping & Transfer (via SONIC Global Stashes)
  if (sub_orams_[0] && sub_orams_[1] && T > 0) {
    // 1. Pad T to power of 2 for OCompact
    uint64_t T_pow2 = 1;
    while (T_pow2 < T) T_pow2 *= 2;

    // 2. Deterministic Batch Extraction into Enclave
    std::vector<std::pair<Key, bool>> S_keys;
    std::vector<std::pair<Key, bool>> L_keys;
    for (uint64_t i = 0; i < T; ++i) {
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
