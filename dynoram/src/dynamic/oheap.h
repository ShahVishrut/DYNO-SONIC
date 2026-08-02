#ifndef DYNO_DYNAMIC_OHEAP_STEPPING_PATH_O_HEAP_H_
#define DYNO_DYNAMIC_OHEAP_STEPPING_PATH_O_HEAP_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "src/static/oheap.h"
#include "src/utils/crypto.h"

namespace dyno::dynamic_stepping_path_oheap {

using POHeap = static_path_oheap::OHeap;
using Block = static_path_oheap::Block;
using Key = static_path_oheap::Key;
using Val = static_path_oheap::Val;

class OHeap {
 public:
  OHeap(size_t val_len) : val_len_(val_len) {}
  // Only implemented for benchmarks.
  OHeap(int starting_size_power_of_two, size_t val_len);
  void Grow(crypto::Key enc_key);
  void Shrink(crypto::Key enc_key);
  void Insert(Key k, Val v, crypto::Key enc_key, bool pad = true);
  Block FindMin(crypto::Key enc_key, bool pad = true);
  Block ExtractMin(crypto::Key enc_key);
  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] size_t Size() const { return size_; }
  [[nodiscard]] uint64_t MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] uint64_t MemoryBytesMovedTotal() const { return memory_bytes_moved_total_; }

 private:
  size_t capacity_ = 0;
  const size_t val_len_;
  size_t size_ = 0;
  std::array<std::unique_ptr<POHeap>, 2> sub_oheaps_{};
  uint64_t memory_access_count_ = 0;
  uint64_t memory_bytes_moved_total_ = 0;
  uint64_t SubOHeapsMemoryAccessCountSum();
  uint64_t SubOHeapsMemoryBytesMovedTotalSum();
};
} // namespace dyno::dynamic_stepping_path_oheap

#endif //DYNO_DYNAMIC_OHEAP_STEPPING_PATH_O_HEAP_H_
