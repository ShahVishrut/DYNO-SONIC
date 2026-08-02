#ifndef DYNO_STATIC_OHEAP_PATH_OHEAP_H
#define DYNO_STATIC_OHEAP_PATH_OHEAP_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "openssl/rand.h"

#include "src/utils/bytes.h"
#include "src/utils/crypto.h"
#include "src/store/store.h"
#include "src/dynamic/sonic_adapter.h"

namespace dyno::static_path_oheap {

using Pos = uint32_t;
using Key = uint32_t;
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
  Val val_ = nullptr;

  explicit Block(bool zero_fill = false) : meta_(zero_fill) {}
  Block(Pos p, Key k, Val v) : meta_(p, k), val_(std::move(v)) {}
  Block(Pos p, Key k) : meta_(p, k) {}
  Block(uint8_t *data, size_t val_len);
  Block(const Block &b, size_t val_len);
  void ToBytes(size_t val_len, uint8_t *out);
};

static constexpr const unsigned int kBucketSize = 3;
static constexpr auto kBlockValid{[]() constexpr {
  std::array<char, kBucketSize> res{};
  for (int i = 0; i < kBucketSize; ++i)
    res[i] = 0b100 << i;
  return res;
}()};

class BucketMetadata {
 public:
  uint8_t flags_ = 0;
};

static size_t BucketSize(size_t val_len) {
  return sizeof(BucketMetadata) + ((kBucketSize + 1) * BlockSize(val_len));
}

static size_t EncryptedBucketSize(size_t val_len) {
  return crypto::CiphertextLen(BucketSize(val_len));
}

class Bucket {
 public:
  std::array<Block, kBucketSize> blocks_;
  Block min_block_;
  BucketMetadata meta_{0};

  Bucket() = default;
  Bucket(uint8_t *data, size_t val_len);

  std::unique_ptr<uint8_t[]> ToBytes(size_t val_len);
  void ToBytes(uint8_t *res, size_t val_len);
};

// Assumes 1-based positions ([1, N]) and power-of-two sizes.
class OHeap {
 public:
  OHeap(size_t n, size_t val_len);

  Block FindMin(crypto::Key enc_key, bool pad = true);
  Block ExtractMin(crypto::Key enc_key, bool is_dummy = false);
  void Insert(Key k, Val v, crypto::Key enc_key, bool is_dummy = false);
  void DummyAccess(crypto::Key enc_key, bool with_find_min = true);
  void FillWithDummies(crypto::Key enc_key);
  [[nodiscard]] size_t Capacity() const { return capacity_; }
  [[nodiscard]] size_t Size() const { return size_; }
  [[nodiscard]] Pos GeneratePos() const;
  [[nodiscard]] unsigned long long MemoryAccessCount() const { return memory_access_count_; }
  [[nodiscard]] unsigned long long MemoryBytesMovedTotal() const { return memory_access_bytes_total_; };

 private:
  size_t capacity_;
  size_t size_ = 0;
  size_t val_len_;
  unsigned int depth_;
  size_t num_buckets_;
  std::unique_ptr<dyno::dynamic_stepping_path_oram::SonicORamAdapter> store_;
  std::vector<Block> stash_;
  unsigned long long memory_access_count_ = 0;
  unsigned long long memory_access_bytes_total_ = 0;
  // todo: remove if unused!
  std::unique_ptr<uint8_t[]> bucket_buffer_;
  std::unique_ptr<uint8_t[]> enc_bucket_buffer_;

  void ReadPath(Pos p, crypto::Key enc_key,
                bool erase_if_found = false, Key k = 0, Val *v = nullptr);
  void UpdateMinAndEvict(Pos p, crypto::Key enc_key);
  Block SiblingMin(unsigned int idx, crypto::Key enc_key);
  [[nodiscard]] std::vector<unsigned int> Path(Pos p) const;
  [[nodiscard]] unsigned int PathAtLevel(Pos p, unsigned int level) const;
  [[nodiscard]] std::pair<Pos, Pos> GeneratePathPair() const;
  [[nodiscard]] Pos GenerateSecondPos(Pos p) const;
};
} // namespace dyno::static_path_oheap

#endif //DYNO_STATIC_OHEAP_PATH_OHEAP_H
