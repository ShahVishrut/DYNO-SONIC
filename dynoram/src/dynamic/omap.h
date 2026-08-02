#ifndef DYNO_DYNAMIC_OMAP_STEPPING_PATH_OMAP_H_
#define DYNO_DYNAMIC_OMAP_STEPPING_PATH_OMAP_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "src/static/omap.h"
#include "src/utils/crypto.h"

namespace dyno::dynamic_stepping_path_omap {

using POMap = static_path_omap::OMap;

using Key = static_path_omap::Key;
using Val = static_path_omap::Val;
using KeyValPair = static_path_omap::KeyValPair;

class OMap {
 public:
  // PosixSingleFile -- On file store reverts to RAM store.
  explicit OMap(size_t val_len, std::string path = "",
                uint8_t max_levels_in_mem = 0)
      : val_len_(val_len),
        store_path_(std::move(path)),
        max_mem_level_(max_levels_in_mem) {}
  // Only implemented for benchmarks --- PosixSingleFile.
  OMap(int starting_size_power_of_two, size_t val_len,
       std::string path = "", uint8_t max_levels_in_mem = 0);
  void Grow(crypto::Key enc_key);
  void Shrink(crypto::Key enc_key);
  void Insert(Key k, Val v, crypto::Key enc_key);
  Val Read(Key k, crypto::Key enc_key);
  Val ReadAndRemove(Key k, crypto::Key enc_key);
  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] size_t Size() const;
  [[nodiscard]] uint64_t MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] uint64_t MemoryBytesMovedTotal() const { return memory_bytes_moved_total_; }
  [[nodiscard]] bool IsOnDisk() const;

 private:
  size_t capacity_ = 0;
  const size_t val_len_;
  size_t size_ = 0;
  const std::string store_path_ = "";
  std::array<std::unique_ptr<POMap>, 2> sub_omaps_{};
  uint64_t memory_access_count_ = 0;
  uint64_t memory_bytes_moved_total_ = 0;
  const uint8_t max_mem_level_;
  [[nodiscard]] size_t TotalSizeOfSubOmaps() const;
  [[nodiscard]] uint64_t SubOMapsMemoryAccessCountSum() const;
  [[nodiscard]] uint64_t SubOMapsMemoryBytesMovedTotalSum() const;
};
} // dyno::dynamic_stepping_path_omap

#endif //DYNO_DYNAMIC_OMAP_STEPPING_PATH_OMAP_H_
