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
  log_map_[0].resize(base_cap + 1, 0);
  log_map_[1].resize((base_cap * 2) + 1, 0);
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
        bool match = sn::obliv::ct_eq(static_cast<uint64_t>(k), log_map_[i][j]);
        phys_k = sn::obliv::ct_select<uint64_t>(j, phys_k, match);
        log_map_[i][j] = sn::obliv::ct_select<uint64_t>(0, log_map_[i][j], match);
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
    if (i == 0 && (sub_orams_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
      
    uint64_t cap = (i == 0) ? cap_S : cap_L;
    uint64_t phys_k = 0;
    for (uint64_t j = 1; j <= cap; ++j) {
        bool match = sn::obliv::ct_eq(static_cast<uint64_t>(k), log_map_[i][j]);
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
      bool is_empty = (log_map_[1][i] == 0);
      bool select_this = is_empty && (phys_k == 0);
      phys_k = sn::obliv::ct_select<uint64_t>(i, phys_k, select_this);
      log_map_[1][i] = sn::obliv::ct_select<uint64_t>(k, log_map_[1][i], select_this);
  }
  
  for (int i = 0; i < 2; ++i) {
    if (sub_orams_[i] == nullptr) continue;
    bool is_real = (i == 1);
    uint64_t target_phys_k = sn::obliv::ct_select<uint64_t>(phys_k, 1, is_real);
    
    static_path_oram::Block b(0, target_phys_k);
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
  
  // Pre-Phase: Oblivious Routing via LogMap Scan
  for (auto& op : batch) {
      op.sub_oram_idx = -1;
      op.phys_k = 0;
  }
  
  uint64_t cap_S = sub_orams_[0] ? sub_orams_[0]->Capacity() : 0;
  uint64_t cap_L = sub_orams_[1] ? sub_orams_[1]->Capacity() : 0;
  
  if (cap_S > 0) {
      for (uint64_t i = 1; i <= cap_S; ++i) {
          uint64_t log_k = log_map_[0][i];
          for (auto& op : batch) {
              bool match = (log_k != 0) && sn::obliv::ct_eq(static_cast<uint64_t>(op.key), log_k);
              op.sub_oram_idx = sn::obliv::ct_select<int8_t>(0, op.sub_oram_idx, match);
              op.phys_k = sn::obliv::ct_select<uint64_t>(i, op.phys_k, match);
          }
      }
  }
  
  if (cap_L > 0) {
      for (uint64_t i = 1; i <= cap_L; ++i) {
          uint64_t log_k = log_map_[1][i];
          for (auto& op : batch) {
              bool match = (log_k != 0) && sn::obliv::ct_eq(static_cast<uint64_t>(op.key), log_k);
              op.sub_oram_idx = sn::obliv::ct_select<int8_t>(1, op.sub_oram_idx, match);
              op.phys_k = sn::obliv::ct_select<uint64_t>(i, op.phys_k, match);
          }
      }
  }


  
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

  // After bitonic sort, batch is co-sorted with elems via the hook.
  // Reset seq to current position so batch[elems[i].seq] == batch[i] stays correct
  // even after the later std::sort reorders elems without touching batch.
  for (size_t i = 0; i < B; ++i) {
      elems[i].seq = static_cast<uint32_t>(i);
  }

  // Phase 2: O-Scan (Collapse) using Two Passes
  // Pass 1: Forward Scan
  // Propagates the latest payload to Search operations, and tracks if the key was deleted.
  std::vector<uint8_t> current_payload(val_len_, 0);
  bool has_payload = false;
  bool is_deleted = false;
  
  for (size_t i = 0; i < B; ++i) {
      bool start_of_key = (i == 0) || !sn::obliv::ct_eq(elems[i].key, elems[i-1].key);
      has_payload = sn::obliv::ct_select(false, has_payload, start_of_key);
      is_deleted = sn::obliv::ct_select(false, is_deleted, start_of_key);
      
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
      bool is_update = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Update));
      bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
      bool is_search = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Search));
      
      bool is_write = is_insert | is_update;
      
      if (val_len_ > 0) {
          sn::obliv::ct_select_array(current_payload.data(), batch[i].val.get(), current_payload.data(), val_len_, is_write);
      }
      has_payload = has_payload | is_write;
      has_payload = sn::obliv::ct_select(false, has_payload, is_delete);
      is_deleted = sn::obliv::ct_select(true, is_deleted, is_delete);
      is_deleted = sn::obliv::ct_select(false, is_deleted, is_insert);
      
      bool forward_to_search = is_search & has_payload & !is_deleted;
      if (val_len_ > 0) {
          if (!batch[i].result.val_) {
              batch[i].result.val_ = std::make_unique<uint8_t[]>(val_len_);
              std::fill(batch[i].result.val_.get(), batch[i].result.val_.get() + val_len_, 0);
          }
          sn::obliv::ct_select_array(batch[i].result.val_.get(), current_payload.data(), batch[i].result.val_.get(), val_len_, forward_to_search);
      }
      // If a search receives a forwarded payload, or if it searches a deleted key, it becomes dummy
      bool search_becomes_dummy = (is_search & has_payload) | (is_search & is_deleted);
      sn::obliv::ct_set_ref(elems[i].is_dummy, true, search_becomes_dummy);
  }

  // Pass 2: Backward Scan
  // Determines exactly ONE real operation (Insert, Update, or Delete) per key,
  // and propagates the final payload backwards to it.
  bool has_future_write = false;
  bool has_future_delete = false;
  std::vector<uint8_t> backward_payload(val_len_, 0);
  
  for (int64_t i = B - 1; i >= 0; --i) {
      bool end_of_key = (i == B - 1) || !sn::obliv::ct_eq(elems[i].key, elems[i+1].key);
      has_future_write = sn::obliv::ct_select(false, has_future_write, end_of_key);
      has_future_delete = sn::obliv::ct_select(false, has_future_delete, end_of_key);
      
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
      bool is_update = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Update));
      bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
      
      // If this is the FIRST write we see going backwards, it captures the payload
      bool captures_payload = (is_insert | is_update) & !has_future_write & !has_future_delete;
      if (val_len_ > 0) {
          sn::obliv::ct_select_array(backward_payload.data(), batch[i].val.get(), backward_payload.data(), val_len_, captures_payload);
      }
      
      // If there is a future write or delete, this operation is overshadowed and becomes dummy
      bool overshadowed = (is_insert | is_update | is_delete) & (has_future_write | has_future_delete);
      
      // Exception: If this is an Insert, and there are future Updates, the Insert MUST remain real 
      // (to be processed by InsertBatch and increment real_I). 
      // It will absorb the backward_payload. The future Updates become dummy.
      bool is_dominant_insert = is_insert & has_future_write & !has_future_delete;
      overshadowed = sn::obliv::ct_select(false, overshadowed, is_dominant_insert);
      
      if (val_len_ > 0) {
          sn::obliv::ct_select_array(batch[i].val.get(), backward_payload.data(), batch[i].val.get(), val_len_, is_dominant_insert);
      }
      
      sn::obliv::ct_set_ref(elems[i].is_dummy, true, overshadowed);
      
      // If this operation was an Insert that remained real, it absorbs all future writes,
      // so we set has_future_write = true (which it already is) but it effectively overshadows PAST writes.
      has_future_write = has_future_write | is_insert | is_update;
      has_future_delete = sn::obliv::ct_select(false, has_future_delete, is_insert); // Insert cancels future delete
      has_future_delete = has_future_delete | is_delete;
  }
  
  // But real_I doesn't increment! So net growth is -1! 
  // But the key was never in the tree, so net growth should be 0!
  // To fix this, we need a forward scan to track if a Delete is deleting an Insert from the SAME batch.
  bool seen_insert = false;
  for (size_t i = 0; i < B; ++i) {
      bool start_of_key = (i == 0) || !sn::obliv::ct_eq(elems[i].key, elems[i-1].key);
      seen_insert = sn::obliv::ct_select(false, seen_insert, start_of_key);
      
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
      bool is_delete = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Delete));
      
      seen_insert = seen_insert | is_insert;
      
      // If we see a Delete and we've seen an Insert for this key in the same batch, the Delete becomes dummy
      bool dummy_delete = is_delete & seen_insert;
      sn::obliv::ct_set_ref(elems[i].is_dummy, true, dummy_delete);
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

  // Allocate empty slots for REAL Inserts in S_large
  for (size_t i = 0; i < B; ++i) {
      uint32_t orig_idx = elems[i].seq;
      bool is_real = !elems[i].is_dummy;
      bool is_insert = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Insert));
      bool needs_slot = is_real && is_insert;
      
      uint64_t assigned_slot = 0;
      for (uint64_t j = 1; j <= cap_L; ++j) {
          bool is_empty = (log_map_[1][j] == 0);
          bool select_this = is_empty && (assigned_slot == 0) && needs_slot;
          assigned_slot = sn::obliv::ct_select<uint64_t>(j, assigned_slot, select_this);
          log_map_[1][j] = sn::obliv::ct_select<uint64_t>(elems[i].key, log_map_[1][j], select_this);
      }
      
      batch[orig_idx].sub_oram_idx = sn::obliv::ct_select<int8_t>(1, batch[orig_idx].sub_oram_idx, needs_slot);
      batch[orig_idx].phys_k = sn::obliv::ct_select<uint64_t>(assigned_slot, batch[orig_idx].phys_k, needs_slot);
  }

  // Phase 3: O-Sort (Group by OpType, then Dummy)
    std::sort(elems.begin(), elems.end(), [](const OblivElem& a, const OblivElem& b) {
        bool a_is_insert = (a.op_type == static_cast<uint8_t>(OpType::Insert) && !a.is_dummy);
        bool b_is_insert = (b.op_type == static_cast<uint8_t>(OpType::Insert) && !b.is_dummy);
        
        if (a_is_insert != b_is_insert) return a_is_insert < b_is_insert; // Real Inserts to the very end
        
        if (a.is_dummy != b.is_dummy) return a.is_dummy < b.is_dummy; // Real Accesses before Dummies
        
        if (a.key != b.key) return a.key < b.key;
        return a.op_type < b.op_type;
    });

  std::vector<Block> inserts;
  std::vector<SonicORamAdapter::AccessOp> small_ops, large_ops;

  // Find the actual partition point: real inserts are at the end after the sort
  size_t original_accesses = B;
  for (size_t i = B; i > 0; --i) {
      bool is_real_insert = !elems[i-1].is_dummy && 
          sn::obliv::ct_eq(elems[i-1].op_type, static_cast<uint8_t>(OpType::Insert));
      if (is_real_insert) {
          original_accesses = i - 1;
      } else {
          break;
      }
  }

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

  if (sub_orams_[0]) {
      std::cout << "[DEBUG] Phase 2: small sub_oram ReadBatch" << std::endl;
      auto read_results = sub_orams_[0]->ReadBatch(small_ops, enc_key, steady_state);
      std::cout << "[DEBUG] Phase 2: small sub_oram ReadBatch done" << std::endl;
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
      std::cout << "[DEBUG] Phase 2: large sub_oram ReadBatch" << std::endl;
      auto read_results = sub_orams_[1]->ReadBatch(large_ops, enc_key, steady_state);
      std::cout << "[DEBUG] Phase 2: large sub_oram ReadBatch done" << std::endl;
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

  // Phase 2.5: Post-ReadBatch backward scan to propagate old values to dummy Searches
  std::vector<uint8_t> old_payload(val_len_, 0);
  bool has_old_payload = false;
  
  for (int64_t i = original_accesses - 1; i >= 0; --i) {
      bool end_of_key = (i == static_cast<int64_t>(original_accesses) - 1) || !sn::obliv::ct_eq(elems[i].key, elems[i+1].key);
      has_old_payload = sn::obliv::ct_select(false, has_old_payload, end_of_key);
      
      uint32_t orig_idx = elems[i].seq;
      bool is_real = !elems[i].is_dummy;
      bool is_search = sn::obliv::ct_eq(elems[i].op_type, static_cast<uint8_t>(OpType::Search));
      
      if (val_len_ > 0) {
          sn::obliv::ct_select_array(old_payload.data(), batch[orig_idx].result.val_.get(), old_payload.data(), val_len_, is_real);
      }
      has_old_payload = has_old_payload | is_real;
      
      bool needs_old_payload = is_search & !is_real & has_old_payload;
      
      if (val_len_ > 0) {
          if (!batch[orig_idx].result.val_) {
              batch[orig_idx].result.val_ = std::make_unique<uint8_t[]>(val_len_);
          }
          sn::obliv::ct_select_array(batch[orig_idx].result.val_.get(), old_payload.data(), batch[orig_idx].result.val_.get(), val_len_, needs_old_payload);
      }
      
      bool is_real_search = is_real & is_search;
      batch[orig_idx].result.key_ = sn::obliv::ct_select<Key>(batch[orig_idx].key, batch[orig_idx].result.key_, is_real_search);
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
        
        // Find the phys_k for this insert using the pre-assigned batch values
        // We must scan the batch array to find the op with this key to get its phys_k
        // Actually, since inserts correspond to the elements at the END of elems array!
        // The loop is over `inserts`, which was populated from `elems` at indices original_accesses to B.
        // Let's just use the orig_idx!
        // Wait, b.key_ is the logic_key, not orig_idx.
        // Let's modify the `inserts` population logic to store phys_k directly!
        uint64_t phys_k = 0;
        for (size_t j = original_accesses; j < B; ++j) {
            uint32_t orig_idx = elems[j].seq;
            bool match = sn::obliv::ct_eq(b.key_, batch[orig_idx].key);
            phys_k = sn::obliv::ct_select(batch[orig_idx].phys_k, phys_k, match);
        }
        
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
  int64_t x = cap_S;
  int64_t y = cap_L;

  int64_t target_small = x - a;
  int64_t target_large = y + 2*a;

  int64_t k_transfer = a;
  bool scale_up = false, scale_down = false;
  int64_t T = 0;

  if (target_small <= 0) {
    k_transfer = x;
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

  // Phase 4: Exact Transfer via LogMap
  if (sub_orams_[0] && sub_orams_[1] && T > 0) {
    std::cout << "[DEBUG] Phase 4 Start. T=" << T << " k_transfer=" << k_transfer << std::endl;
    int64_t T_pow2 = 1;
    while (T_pow2 < T) T_pow2 *= 2;

    auto no_filter = [](Key phys_k) { return true; };
    std::vector<Key> extracted;
    std::vector<std::pair<Key, bool>> keys_to_read;
    
    if (k_transfer > 0) {
        extracted = sub_orams_[0]->ObliviousExtractValidKeys(k_transfer, T, no_filter);
    } else if (k_transfer < 0) {
        extracted = sub_orams_[1]->ObliviousExtractValidKeys(-k_transfer, T, no_filter);
    } else {
        extracted.resize(T, 0);
    }

    for (int64_t i = 0; i < T; ++i) {
        keys_to_read.push_back({extracted[i], extracted[i] != 0});
    }

    std::vector<static_path_oram::Block> Buffer;
    if (k_transfer > 0) Buffer = sub_orams_[0]->ReadAndRemoveBatch(keys_to_read, enc_key);
    else if (k_transfer < 0) Buffer = sub_orams_[1]->ReadAndRemoveBatch(keys_to_read, enc_key);
    
    if (k_transfer != 0) {
        for (int64_t i = T; i < T_pow2; ++i) {
            Buffer.emplace_back(true);
            if (val_len_ > 0) {
                Buffer.back().val_ = std::make_unique<uint8_t[]>(val_len_);
                std::fill(Buffer.back().val_.get(), Buffer.back().val_.get() + val_len_, 0);
            }
        }
        
        uint64_t from_cap = (k_transfer > 0) ? cap_S : cap_L;
        uint64_t to_cap = (k_transfer > 0) ? cap_L : cap_S;
        auto& from_map = (k_transfer > 0) ? log_map_[0] : log_map_[1];
        auto& to_map = (k_transfer > 0) ? log_map_[1] : log_map_[0];
        
        for (int64_t i = 0; i < T; ++i) {
            uint64_t old_phys_k = Buffer[i].meta_.key_;
            bool is_valid = (old_phys_k != 0);
            
            uint64_t log_k = 0;
            for (uint64_t j = 1; j <= from_cap; ++j) {
                bool match = is_valid && sn::obliv::ct_eq(j, old_phys_k);
                log_k = sn::obliv::ct_select(from_map[j], log_k, match);
                from_map[j] = sn::obliv::ct_select<uint64_t>(0, from_map[j], match);
            }
            
            uint64_t new_phys_k = 0;
            for (uint64_t j = 1; j <= to_cap; ++j) {
                bool is_empty = (to_map[j] == 0);
                bool select_this = is_valid && is_empty && (new_phys_k == 0);
                new_phys_k = sn::obliv::ct_select(j, new_phys_k, select_this);
                to_map[j] = sn::obliv::ct_select(log_k, to_map[j], select_this);
            }
            
            Buffer[i].meta_.key_ = new_phys_k;
        }
        
        if (k_transfer > 0) sub_orams_[1]->InsertBatch(Buffer, enc_key, true);
        else sub_orams_[0]->InsertBatch(Buffer, enc_key, true);
    }
    
    capacity_ += a;
  }

  // Phase 5: Simple Structural Scale Up/Down with Secondary Transfers
  if (scale_up) {
    int64_t old_y = sub_orams_[1]->Capacity();
    int64_t excess = std::max<int64_t>(0, static_cast<int64_t>(capacity_) - old_y);
    int64_t k_sec = 2 * excess;
    
    sub_orams_[0] = std::move(sub_orams_[1]);
    sub_orams_[1] = std::make_unique<PORam>(2 * old_y, val_len_, true);
    
    log_map_[0] = std::move(log_map_[1]);
    log_map_[1].clear();
    log_map_[1].resize((2 * old_y) + 1, 0);
    
    if (k_sec > 0) {
        auto no_filter = [](Key phys_k) { return true; };
        std::vector<Key> extracted = sub_orams_[0]->ObliviousExtractValidKeys(k_sec, k_sec, no_filter);
        
        std::vector<std::pair<Key, bool>> keys_to_read;
        for (int64_t i = 0; i < k_sec; ++i) {
            keys_to_read.push_back({extracted[i], extracted[i] != 0});
        }
        
        std::vector<static_path_oram::Block> Buffer = sub_orams_[0]->ReadAndRemoveBatch(keys_to_read, enc_key);
        
        for (int64_t i = 0; i < k_sec; ++i) {
            uint64_t old_phys_k = Buffer[i].meta_.key_;
            bool is_valid = (old_phys_k != 0);
            
            uint64_t log_k = 0;
            for (uint64_t j = 1; j <= old_y; ++j) {
                bool match = is_valid && sn::obliv::ct_eq(j, old_phys_k);
                log_k = sn::obliv::ct_select(log_map_[0][j], log_k, match);
                log_map_[0][j] = sn::obliv::ct_select<uint64_t>(0, log_map_[0][j], match);
            }
            
            uint64_t new_phys_k = 0;
            for (uint64_t j = 1; j <= 2 * old_y; ++j) {
                bool is_empty = (log_map_[1][j] == 0);
                bool select_this = is_valid && is_empty && (new_phys_k == 0);
                new_phys_k = sn::obliv::ct_select(j, new_phys_k, select_this);
                log_map_[1][j] = sn::obliv::ct_select(log_k, log_map_[1][j], select_this);
            }
            
            Buffer[i].meta_.key_ = new_phys_k;
        }
        sub_orams_[1]->InsertBatch(Buffer, enc_key, true);
    }
    std::cout << "[DEBUG] Phase 5 scale_up complete. capacity_=" << capacity_ << std::endl;
  } else if (scale_down) {
    int64_t old_x = sub_orams_[0]->Capacity();
    int64_t deficit = std::max<int64_t>(0, old_x - static_cast<int64_t>(capacity_));
    int64_t k_sec = deficit;
    
    sub_orams_[1] = std::move(sub_orams_[0]);
    sub_orams_[0] = std::make_unique<PORam>(old_x / 2, val_len_, true);
    
    log_map_[1] = std::move(log_map_[0]);
    log_map_[0].clear();
    log_map_[0].resize((old_x / 2) + 1, 0);

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
            for (uint64_t j = 1; j <= old_x; ++j) {
                bool match = is_valid && sn::obliv::ct_eq(j, old_phys_k);
                log_k = sn::obliv::ct_select(log_map_[1][j], log_k, match);
                log_map_[1][j] = sn::obliv::ct_select<uint64_t>(0, log_map_[1][j], match);
            }
            
            uint64_t new_phys_k = 0;
            for (uint64_t j = 1; j <= old_x / 2; ++j) {
                bool is_empty = (log_map_[0][j] == 0);
                bool select_this = is_valid && is_empty && (new_phys_k == 0);
                new_phys_k = sn::obliv::ct_select(j, new_phys_k, select_this);
                log_map_[0][j] = sn::obliv::ct_select(log_k, log_map_[0][j], select_this);
            }
            
            Buffer[i].meta_.key_ = new_phys_k;
        }
        sub_orams_[0]->InsertBatch(Buffer, enc_key, true);
    }
    std::cout << "[DEBUG] Phase 5 scale_down complete. capacity_=" << capacity_ << std::endl;
  }

  if (B > original_B) {
      batch.resize(original_B);
  }
}

} // namespace dyno::dynamic_stepping_path_oram
