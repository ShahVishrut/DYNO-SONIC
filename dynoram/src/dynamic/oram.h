#ifndef DYNO_DYNAMIC_ORAM_STEPPING_PATH_ORAM_H_
#define DYNO_DYNAMIC_ORAM_STEPPING_PATH_ORAM_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "src/static/oram.h"
#include "src/utils/crypto.h"
#include "src/dynamic/sonic_adapter.h"

namespace dyno::dynamic_stepping_path_oram {

using PORam = SonicORamAdapter;
using PORamBlock = static_path_oram::Block;

using Key = static_path_oram::Key;
using Val = static_path_oram::Val;

class Block {
 public:
  Key key_ = 0;
  Val val_;

  explicit Block() = default;
  explicit Block(PORamBlock b) : key_(b.meta_.key_), val_(std::move(b.val_)) {}
};

// Assumes 1-based positions ([1, N]).
class ORam {
 public:
  explicit ORam(size_t val_len) : val_len_(val_len) {}
  // Only implemented for benchmarks.
  ORam(int starting_size_power_of_two, size_t val_len);
  void Grow(crypto::Key enc_key);
  Block ReadAndRemove(Key k, crypto::Key enc_key);
  Block Read(Key k, crypto::Key enc_key);
  void Insert(Key k, Val v, crypto::Key enc_key);
  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] size_t Size() const { return size_; }
  [[nodiscard]] uint64_t MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] uint64_t MemoryBytesMovedTotal() const { return memory_bytes_moved_total_; }

 private:
  size_t capacity_ = 0;
  const size_t val_len_;
  size_t size_ = 0;
  std::array<std::unique_ptr<PORam>, 2> sub_orams_{};
  uint8_t SubOramIndex(Key k);
  uint64_t memory_access_count_ = 0;
  uint64_t memory_bytes_moved_total_ = 0;
  uint64_t SubORamsMemoryAccessCountSum();
  uint64_t SubORamsMemoryBytesMovedTotalSum();
};
} // dyno::dynamic_stepping_path_oram

#endif //DYNO_DYNAMIC_ORAM_STEPPING_PATH_ORAM_H_
