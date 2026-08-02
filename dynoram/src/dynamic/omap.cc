#include "omap.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "src/static/omap.h"
#include "src/utils/crypto.h"
#include "sonic/obliv/ops/core_ops.hpp"

namespace dyno::dynamic_stepping_path_omap {

namespace {
bool IsPowerOfTwo(size_t x) {
  return !(x & (x - 1));
}
} // namespace

OMap::OMap(int starting_size_power_of_two, size_t val_len,
           std::string path, uint8_t max_levels_in_mem)
    : capacity_(1UL << starting_size_power_of_two),
      val_len_(val_len),
      size_(1UL << starting_size_power_of_two),
      store_path_(std::move(path)),
      max_mem_level_(max_levels_in_mem) {
  auto base_cap = capacity_ >> 1;
  if (capacity_)
    for (int i = 0; i < 2; ++i)
      sub_omaps_[i] = std::make_unique<POMap>(base_cap << i, val_len_,
                                              store_path_, max_mem_level_);
}

void OMap::Grow(crypto::Key enc_key) {
  if (capacity_ == 0) {
    sub_omaps_[1] = std::make_unique<POMap>(1, val_len_,
                                            store_path_, max_mem_level_);
    ++capacity_;
    return;
  }

  if (IsPowerOfTwo(capacity_)) {
    assert(sub_omaps_[1] != nullptr);
    sub_omaps_[0] = std::move(sub_omaps_[1]);
    sub_omaps_[1] = std::make_unique<POMap>(2 * capacity_, val_len_,
                                            store_path_, max_mem_level_);
  }

  assert(sub_omaps_[0] != nullptr && sub_omaps_[1] != nullptr);
  auto start_accesses = SubOMapsMemoryAccessCountSum();
  auto start_bytes = SubOMapsMemoryBytesMovedTotalSum();
  bool found = false;
  auto move_kv = sub_omaps_[0]->TakeOne(enc_key, found);
  sub_omaps_[1]->Insert(move_kv.key_, std::move(move_kv.val_), enc_key, !found);
  ++capacity_;
  memory_access_count_ += SubOMapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubOMapsMemoryBytesMovedTotalSum() - start_bytes;
}

void OMap::Shrink(crypto::Key enc_key) {
  if (capacity_ == 0)
    return;

  assert(capacity_ > size_);

  if (capacity_ == 1) {
    for (auto &so : sub_omaps_) {
      so.reset();
    }
    capacity_ = 0;
    return;
  }

  assert(sub_omaps_[0] != nullptr && sub_omaps_[1] != nullptr);
  auto start_accesses = SubOMapsMemoryAccessCountSum();
  auto start_bytes = SubOMapsMemoryBytesMovedTotalSum();
  for (int i = 0; i < 2; ++i) {
    KeyValPair move_kv;
    bool found = false;
    if (sub_omaps_[0]->Size() < sub_omaps_[0]->Capacity()) {
      move_kv = sub_omaps_[1]->TakeOne(enc_key, found);
    } else {
      move_kv.key_ = 0;
      move_kv.val_ = std::make_unique<uint8_t[]>(val_len_);
      std::fill_n(move_kv.val_.get(), val_len_, 0);
      sub_omaps_[1]->Read(0, enc_key); // Dummy
    }
    sub_omaps_[0]->Insert(move_kv.key_, std::move(move_kv.val_), enc_key, !found);
  }
  --capacity_;
  memory_access_count_ += SubOMapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubOMapsMemoryBytesMovedTotalSum() - start_bytes;

  if (IsPowerOfTwo(capacity_)) {
    sub_omaps_[1] = std::move(sub_omaps_[0]);
    size_t smaller_size = capacity_ / 2;
    if (smaller_size) {
      sub_omaps_[0] =
          std::make_unique<POMap>(capacity_ / 2, val_len_,
                                  store_path_, max_mem_level_);
    } else {
      sub_omaps_[0].reset();
    }
  }
}

void OMap::Insert(Key key, Val val, crypto::Key enc_key) {
  assert(size_ < capacity_);
  auto start_accesses = SubOMapsMemoryAccessCountSum();
  auto start_bytes = SubOMapsMemoryBytesMovedTotalSum();
  size_t pre_size = TotalSizeOfSubOmaps();
  if (sub_omaps_[0] != nullptr) { // Corner case: capacity = 1
    bool found = false;
    sub_omaps_[0]->ReadAndRemove(key, enc_key, found);
  }
  sub_omaps_[1]->Insert(key, std::move(val), enc_key);
  if (TotalSizeOfSubOmaps() > pre_size)
    ++size_; // Else it was a pre-existing key
  memory_access_count_ += SubOMapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubOMapsMemoryBytesMovedTotalSum() - start_bytes;
}

Val OMap::Read(Key key, crypto::Key enc_key) {
  Val res = std::make_unique<uint8_t[]>(val_len_);
  std::fill_n(res.get(), val_len_, 0);
  auto start_accesses = SubOMapsMemoryAccessCountSum();
  auto start_bytes = SubOMapsMemoryBytesMovedTotalSum();
  for (int i = 0; i < 2; ++i) {
    // Skip the first sub structure if 1. it's null (size==1) or it's known to
    // be empty (IsPowerOfTwo(capacity_)).
    if (i == 0 && (sub_omaps_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
    auto so_val = sub_omaps_[i]->Read(key, enc_key);
    for (size_t j = 0; j < val_len_; ++j) {
        res.get()[j] ^= so_val.get()[j];
    }
  }
  memory_access_count_ += SubOMapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubOMapsMemoryBytesMovedTotalSum() - start_bytes;
  return res;
}

Val OMap::ReadAndRemove(Key key, crypto::Key enc_key) {
  size_t pre_size = TotalSizeOfSubOmaps();
  Val res = std::make_unique<uint8_t[]>(val_len_);
  std::fill_n(res.get(), val_len_, 0);
  auto start_accesses = SubOMapsMemoryAccessCountSum();
  auto start_bytes = SubOMapsMemoryBytesMovedTotalSum();
  for (int i = 0; i < 2; ++i) {
    // Skip the first sub structure if 1. it's null (size==1) or it's known to
    // be empty (IsPowerOfTwo(capacity_)).
    if (i == 0 && (sub_omaps_[i] == nullptr || IsPowerOfTwo(capacity_)))
      continue;
    bool found = false;
    auto so_val = sub_omaps_[i]->ReadAndRemove(key, enc_key, found);
    for (size_t j = 0; j < val_len_; ++j) {
      res.get()[j] = sn::obliv::ct_select<uint8_t>(so_val.get()[j], res.get()[j], found);
    }
  }
  if (TotalSizeOfSubOmaps() < pre_size)
    --size_;
  memory_access_count_ += SubOMapsMemoryAccessCountSum() - start_accesses;
  memory_bytes_moved_total_ += SubOMapsMemoryBytesMovedTotalSum() - start_bytes;
  return res;
}

size_t OMap::TotalSizeOfSubOmaps() const {
  size_t res = 0;
  for (auto &so : sub_omaps_)
    if (so != nullptr)
      res += so->Size();
  return res;
}

size_t OMap::Size() const {
  return size_;
}

uint64_t OMap::SubOMapsMemoryAccessCountSum() const {
  unsigned long long res = 0;
  for (auto &so : sub_omaps_) {
    if (so != nullptr) {
      res += so->MemoryAccessCount();
    }
  }
  return res;
}

uint64_t OMap::SubOMapsMemoryBytesMovedTotalSum() const {
  unsigned long long res = 0;
  for (auto &so : sub_omaps_) {
    if (so != nullptr) {
      res += so->MemoryBytesMovedTotal();
    }
  }
  return res;
}

bool OMap::IsOnDisk() const {
  bool res = false;
  for (auto &so : sub_omaps_)
    res |= so->IsOnDisk();
  return res;
}
} // dyno::dynamic_stepping_path_omap
