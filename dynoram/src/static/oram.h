#ifndef DYNO_STATIC_ORAM_PATH_ORAM_H
#define DYNO_STATIC_ORAM_PATH_ORAM_H

#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <string>

#include "src/utils/crypto.h"
#include "src/store/store.h"

namespace dyno::static_path_oram {

using Pos = uint32_t; // TODO: 32
using Key = uint32_t; // TODO: 32
using Val = std::unique_ptr<uint8_t[]>;

class BlockMetadata {
 public:
  Pos pos_;
  Key key_;

  explicit BlockMetadata(bool zero_fill = false) {
    if (zero_fill) {
      pos_ = 0;
      key_ = 0;
    }
  }
  BlockMetadata(Pos p, Key k) : pos_(p), key_(k) {}
};

static size_t BlockSize(size_t val_len) {
  return sizeof(BlockMetadata) + val_len;
}

class Block {
 public:
  BlockMetadata meta_;
  Val val_;

  explicit Block(bool zero_fill = false) : meta_(zero_fill) {}
  Block(Pos p, Key k, Val v) : meta_(p, k), val_(std::move(v)) {}
  Block(Pos p, Key k) : meta_(p, k) {}
  Block(uint8_t *data, size_t val_len);
  Block(const Block &b, size_t val_len);

  void ToBytes(size_t val_len, uint8_t *out);
};

const static unsigned char kLeftChildValid = 0x01;
const static unsigned char kRightChildValid = 0x02;
static constexpr const unsigned int kBucketSize = 4; // Z in PathORAM paper
static constexpr auto kBlockValid{[]() constexpr {
  std::array<char, kBucketSize> res{};
  for (int i = 0; i < kBucketSize; ++i)
    res[i] = 0b100 << i;
  return res;
}()};

class BucketMetadata {
 public:
  uint8_t flags_ = 0; // Only for optimizations.
};

static size_t BucketSize(size_t val_len) {
  return sizeof(BucketMetadata) + (kBucketSize * BlockSize(val_len));
}

static size_t EncryptedBucketSize(size_t val_len) {
  return crypto::CiphertextLen(BucketSize(val_len));
}

class Bucket {
 public:
  std::array<Block, kBucketSize> blocks_;
  BucketMetadata meta_{0};

  Bucket() = default;
  Bucket(uint8_t *data, size_t val_len);

  std::unique_ptr<uint8_t[]> ToBytes(size_t val_len);
  void ToBytes(uint8_t *res, size_t val_len);
};

// Assumes 1-based positions ([1, N]) and power-of-two sizes.
class ORam {
 public:
  // RAM
  ORam(size_t n, size_t val_len,
       bool with_pos_map = false, bool with_key_gen = false);
  // PosixSingleFile -- On file store error reverts to RAM store.
  ORam(size_t n, size_t val_len, const std::string &file_path,
       uint8_t max_levels_in_mem = 0,
       bool with_pos_map = false, bool with_key_gen = false);

  Block ReadAndRemove(Pos p, Key k, crypto::Key enc_key);
  Block Read(Pos p, Key k, crypto::Key enc_key);
  void Insert(Block block, crypto::Key enc_key);
  void DummyAccess(crypto::Key enc_key);
  void FillWithDummies(crypto::Key enc_key);
  [[nodiscard]] Pos GeneratePos() const;
  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] size_t Size() const { return size_; }
  [[nodiscard]] uint64_t MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] uint64_t MemoryBytesMovedTotal() const { return memory_access_bytes_total_; };
  [[nodiscard]] bool IsOnDisk() const { return is_on_disk_; }

  // A client should either always use these or never use them.
  // Doing both leads to undefined behavior.
  // They only work when `with_key_gen = true`.
  Key NextKey();
  void AddFreedKey(Key key);

 private:
  size_t capacity_;
  size_t size_ = 0;
  size_t val_len_;
  uint32_t depth_;
  size_t num_buckets_;
  std::unique_ptr<store::Store> store_;
  std::vector<Block> stash_;
  bool with_pos_map_;
  std::map<Key, Pos> pos_map_{};
  bool with_key_gen_ = false;
  Key next_key_ = 1;
  std::vector<Key> freed_keys_;
  std::map<Pos, bool> bucket_valid_{};
  uint64_t memory_access_count_ = 0;
  uint64_t memory_access_bytes_total_ = 0;
  std::unique_ptr<uint8_t[]> bucket_buffer_;
  std::unique_ptr<uint8_t[]> enc_bucket_buffer_;
  bool is_on_disk_ = false;

  Block ReadPath(Pos p, Key k, crypto::Key enc_key);
  void Evict(Pos p, crypto::Key enc_key);
  [[nodiscard]] std::vector<unsigned int> Path(Pos pos) const;
  [[nodiscard]] uint32_t PathAtLevel(Pos p, unsigned int level) const;
};

} // namespace dyno::static_path_oram
#endif //DYNO_STATIC_ORAM_PATH_ORAM_H
