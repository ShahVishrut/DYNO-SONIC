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
  enum class OpType { Search, Update, Delete, Insert };

  struct BatchOperation {
    OpType type;
    Key key;
    Val val; // For Update/Insert
    Block result; // For Search
    int8_t sub_oram_idx = -1;
    uint64_t phys_k = 0;
    uint64_t cur_leaf = 0;
    uint64_t new_leaf = 0;
  };

  void ExecuteBatch(std::vector<BatchOperation>& batch, crypto::Key enc_key, bool steady_state = true);

  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] size_t Size() const { return size_; }
  [[nodiscard]] uint64_t MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] uint64_t MemoryBytesMovedTotal() const { return memory_bytes_moved_total_; }

  [[nodiscard]] size_t GetSubORamValidCount(int index) const {
      size_t count = 0;
      if (index == 0 && sub_orams_[0]) {
          for (size_t i = 1; i < log_map_[0].size(); ++i) {
              if (log_map_[0][i].logical_key != 0) count++;
          }
      } else if (index == 1 && sub_orams_[1]) {
          for (size_t i = 1; i < log_map_[1].size(); ++i) {
              if (log_map_[1][i].logical_key != 0) count++;
          }
      }
      return count;
  }
  
  [[nodiscard]] size_t GetSubORamCapacity(int index) const {
      if (index == 0 && sub_orams_[0]) return sub_orams_[0]->Capacity();
      if (index == 1 && sub_orams_[1]) return sub_orams_[1]->Capacity();
      return 0;
  }

 private:
  size_t capacity_ = 0;
  const size_t val_len_;
  size_t size_ = 0;
  std::array<std::unique_ptr<PORam>, 2> sub_orams_{};
  struct LogEntry {
      uint64_t logical_key;
      uint64_t leaf;
  };
  std::vector<LogEntry> log_map_[2];
  uint64_t memory_access_count_ = 0;
  uint64_t memory_bytes_moved_total_ = 0;
  uint64_t SubORamsMemoryAccessCountSum();
  uint64_t SubORamsMemoryBytesMovedTotalSum();
};
} // dyno::dynamic_stepping_path_oram

#endif //DYNO_DYNAMIC_ORAM_STEPPING_PATH_ORAM_H_
