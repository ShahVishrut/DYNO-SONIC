#include "oheap.h"

#include <cassert>
#include <cstddef>
#include <memory>

#include "src/static/oheap.h"
#include "src/utils/crypto.h"
#include "sonic/obliv/ops/core_ops.hpp"
#include <iostream>

namespace dyno::dynamic_stepping_path_oheap {

OHeap::OHeap(int starting_size_power_of_two, size_t val_len)
    : capacity_(1UL << starting_size_power_of_two),
      val_len_(val_len),
      size_(1UL << starting_size_power_of_two) {
  auto base_cap = capacity_ >> 1;
  for (int i = 0; i < 2; ++i) {
    sub_oheaps_[i] = std::make_unique<POHeap>(base_cap << i, val_len_);
  }
}

void OHeap::FillWithDummies(crypto::Key enc_key) {
  for (int i = 0; i < 2; ++i) {
    if (sub_oheaps_[i]) {
      sub_oheaps_[i]->FillWithDummies(enc_key);
    }
  }
}

namespace {
bool IsPowerOfTwo(size_t x) {
  return !(x & (x - 1));
}
} // namespace

void OHeap::Grow(crypto::Key enc_key) {
  if (capacity_ == 0) {
    sub_oheaps_[1] = std::make_unique<POHeap>(1, val_len_);
    ++capacity_;
    return;
  }

  if (IsPowerOfTwo(capacity_)) {
    assert(sub_oheaps_[1] != nullptr);
    sub_oheaps_[0] = std::move(sub_oheaps_[1]);
    sub_oheaps_[1] = std::make_unique<POHeap>(2 * capacity_, val_len_);
    sub_oheaps_[1]->FillWithDummies(enc_key);
  }

  auto start_accesses = SubOHeapsMemoryAccessCountSum();
  auto start_bytes = SubOHeapsMemoryBytesMovedTotalSum();
  assert(sub_oheaps_[0] != nullptr && sub_oheaps_[1] != nullptr);
  auto move_bl = sub_oheaps_[0]->ExtractMin(enc_key);
  bool is_dummy = (move_bl.meta_.pos_ == 0);
  sub_oheaps_[1]->Insert(move_bl.meta_.key_, std::move(move_bl.val_),
                         enc_key, is_dummy);
  ++capacity_;
  memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ +=
      SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;
}

void OHeap::Shrink(crypto::Key enc_key) {
  if (capacity_ == 0)
    return;

  assert(capacity_ > size_);

  if (capacity_ == 1) {
    for (auto &so : sub_oheaps_) {
      so.reset();
    }
    capacity_ = 0;
    return;
  }

  assert(sub_oheaps_[0] != nullptr && sub_oheaps_[1] != nullptr);
  auto start_accesses = SubOHeapsMemoryAccessCountSum();
  auto start_bytes = SubOHeapsMemoryBytesMovedTotalSum();
  for (int count = 0; count < 2; ++count) {
    Block move_bl(true);
    bool is_dummy_extract = !(sub_oheaps_[0]->Size() < sub_oheaps_[0]->Capacity());
    move_bl = sub_oheaps_[1]->ExtractMin(enc_key, is_dummy_extract);

    bool is_dummy_insert = (move_bl.meta_.pos_ == 0);
    sub_oheaps_[0]->Insert(move_bl.meta_.key_, std::move(move_bl.val_),
                           enc_key, is_dummy_insert);
  }
  --capacity_;
  memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ +=
      SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;

  if (IsPowerOfTwo(capacity_)) {
    sub_oheaps_[1] = std::move(sub_oheaps_[0]);
    if (capacity_ > 1) {
      sub_oheaps_[0] = std::make_unique<POHeap>(capacity_ / 2, val_len_);
    } else {
      sub_oheaps_[0] = nullptr;
    }
  }
}

void OHeap::Insert(Key k, Val v, crypto::Key enc_key, bool pad) {
  assert(size_ < capacity_);
  auto start_accesses = SubOHeapsMemoryAccessCountSum();
  auto start_bytes = SubOHeapsMemoryBytesMovedTotalSum();
  sub_oheaps_[1]->Insert(k, std::move(v), enc_key);
  ++size_;
  if (!pad) {
    memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
    memory_bytes_moved_total_ +=
        SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;
    return;
  }

  for (auto &so : sub_oheaps_) {
    if (so != nullptr)
      so->DummyAccess(enc_key);
  }
  memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ +=
      SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;
}

Block OHeap::FindMin(crypto::Key enc_key, bool pad) {
  if (!size_)
    return Block(true);

  auto start_accesses = SubOHeapsMemoryAccessCountSum();
  auto start_bytes = SubOHeapsMemoryBytesMovedTotalSum();
  Block res(true);
  for (int i = 0; i < 2; ++i) {
    // Skip the first sub structure if 1. it's null (size==1) or it's known to
    // be empty (IsPowerOfTwo(capacity_)).
    if (i == 0 && (sub_oheaps_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
    auto so_bl = sub_oheaps_[i]->FindMin(enc_key, pad);
    
    bool is_valid = (so_bl.meta_.pos_ != 0);
    bool is_dummy = (res.meta_.pos_ == 0);
    bool is_lt = sn::obliv::ct_lt<static_path_oheap::Key>(so_bl.meta_.key_, res.meta_.key_);
    bool do_update = is_valid & (is_dummy | is_lt);
    
    res.meta_.pos_ = sn::obliv::ct_select<static_path_oheap::Pos>(so_bl.meta_.pos_, res.meta_.pos_, do_update);
    res.meta_.key_ = sn::obliv::ct_select<static_path_oheap::Key>(so_bl.meta_.key_, res.meta_.key_, do_update);
    if (val_len_ > 0) {
      if (!res.val_) res.val_ = std::make_unique<uint8_t[]>(val_len_);
      for (size_t j = 0; j < val_len_; ++j) {
        uint8_t s_v = so_bl.val_ ? so_bl.val_[j] : 0;
        res.val_[j] = sn::obliv::ct_select<uint8_t>(s_v, res.val_[j], do_update);
      }
    }
  }

  if (!pad) { // No need to hide what was done
    memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
    memory_bytes_moved_total_ +=
        SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;
    return res;
  }

  for (auto &so : sub_oheaps_) {
    if (so != nullptr)
      so->DummyAccess(enc_key);
  }
  memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ +=
      SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;
  return res;
}

Block OHeap::ExtractMin(crypto::Key enc_key) {
  if (!size_)
    return Block(true);

  auto start_accesses = SubOHeapsMemoryAccessCountSum();
  auto start_bytes = SubOHeapsMemoryBytesMovedTotalSum();
  Block res(true);
  int found_idx = -1;
  for (int i = 0; i < 2; ++i) {
    // Skip the first sub structure if 1. it's null (size==1) or it's known to
    // be empty (IsPowerOfTwo(capacity_)).
    if (i == 0 && (sub_oheaps_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
    auto so_bl = sub_oheaps_[i]->FindMin(enc_key, false);
    
    bool is_valid = (so_bl.meta_.pos_ != 0);
    bool is_dummy = (res.meta_.pos_ == 0);
    bool is_lt = sn::obliv::ct_lt<static_path_oheap::Key>(so_bl.meta_.key_, res.meta_.key_);
    bool do_update = is_valid & (is_dummy | is_lt);
    
    found_idx = sn::obliv::ct_select<int>(i, found_idx, do_update);
    
    res.meta_.pos_ = sn::obliv::ct_select<static_path_oheap::Pos>(so_bl.meta_.pos_, res.meta_.pos_, do_update);
    res.meta_.key_ = sn::obliv::ct_select<static_path_oheap::Key>(so_bl.meta_.key_, res.meta_.key_, do_update);
    if (val_len_ > 0) {
      if (!res.val_) res.val_ = std::make_unique<uint8_t[]>(val_len_);
      for (size_t j = 0; j < val_len_; ++j) {
        uint8_t s_v = so_bl.val_ ? so_bl.val_[j] : 0;
        res.val_[j] = sn::obliv::ct_select<uint8_t>(s_v, res.val_[j], do_update);
      }
    }
  }

  for (int i = 0; i < 2; ++i) {
    // Skip the first sub structure if 1. it's null (size==1) or it's known to
    // be empty (IsPowerOfTwo(capacity_)).
    if (i == 0 && (sub_oheaps_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
    bool is_dummy = !sn::obliv::ct_eq<int>(found_idx, i);
    sub_oheaps_[i]->ExtractMin(enc_key, is_dummy);
  }
  --size_;
  memory_access_count_ += SubOHeapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ +=
      SubOHeapsMemoryBytesMovedTotalSum() - start_bytes;

  return res;
}

uint64_t OHeap::SubOHeapsMemoryAccessCountSum() {
  unsigned long long res = 0;
  for (auto &so : sub_oheaps_) {
    if (so != nullptr) {
      res += so->MemoryAccessCount();
    }
  }
  return res;
}

uint64_t OHeap::SubOHeapsMemoryBytesMovedTotalSum() {
  unsigned long long res = 0;
  for (auto &so : sub_oheaps_) {
    if (so != nullptr) {
      res += so->MemoryBytesMovedTotal();
    }
  }
  return res;
}
} // namespace dyno::dynamic_stepping_path_oheap
