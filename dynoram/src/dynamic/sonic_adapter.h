#ifndef DYNO_DYNAMIC_ORAM_SONIC_ADAPTER_H_
#define DYNO_DYNAMIC_ORAM_SONIC_ADAPTER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "src/static/oram.h"
#include "src/utils/crypto.h"

namespace dyno::dynamic_stepping_path_oram {

// OMap requires at least 56 bytes for BlockMetadata + Val. OHeap requires at least 81 bytes. We use 128 bytes to be safe and power-of-two.
constexpr size_t kSonicBlockBytes = 128;

class SonicORamAdapter {
 public:
  SonicORamAdapter(size_t n, size_t val_len, bool with_pos_map = false);
  SonicORamAdapter(size_t n, size_t val_len, const std::string &file_path,
                   uint8_t max_levels_in_mem = 0,
                   bool with_pos_map = false, bool with_key_gen = false);
  ~SonicORamAdapter();

  static_path_oram::Block ReadAndRemove(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real = true, bool flush = true);
  static_path_oram::Block Read(static_path_oram::Pos p, static_path_oram::Key k, crypto::Key enc_key, bool is_real = true, bool flush = true);
  void Insert(static_path_oram::Block block, crypto::Key enc_key, bool is_real = true, bool is_new = false, bool flush = true);
  void FlushEpoch();

  std::vector<static_path_oram::Block> ReadAndRemoveBatch(const std::vector<std::pair<static_path_oram::Key, bool>>& keys_with_real_flags, crypto::Key enc_key);
  std::vector<static_path_oram::Block> ReadBatch(const std::vector<std::pair<static_path_oram::Key, bool>>& keys_with_real_flags, crypto::Key enc_key);
  void InsertBatch(std::vector<static_path_oram::Block>& blocks, crypto::Key enc_key);

  [[nodiscard]] uint64_t GenerateRandomLeaf() const;
  
  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] uint64_t MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] uint64_t MemoryBytesMovedTotal() const { return memory_bytes_moved_total_; }

 private:
  size_t capacity_;
  size_t val_len_;
  uint64_t memory_access_count_ = 0;
  uint64_t memory_bytes_moved_total_ = 0;
  bool lazy_mode_ = false;

  // Opaque pointer to hide Sonic details from the header
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dyno::dynamic_stepping_path_oram

#endif // DYNO_DYNAMIC_ORAM_SONIC_ADAPTER_H_
