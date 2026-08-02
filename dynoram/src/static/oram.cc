#include "oram.h"

#include <cstdint>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "openssl/rand.h"

#include "src/utils/bytes.h"
#include "src/utils/crypto.h"
#include "src/store/hybrid_store.h"
#include "src/store/posix_single_file_store.h"
#include "src/store/ram_store.h"
#include "src/store/store.h"

#define max(a, b) ((a)>(b)?(a):(b))

namespace dyno::static_path_oram {

Block::Block(uint8_t *data, size_t val_len) {
  bytes::FromBytes(data, meta_);
  val_ = std::make_unique<uint8_t[]>(val_len);
  std::copy(data + sizeof(BlockMetadata),
            data + sizeof(BlockMetadata) + val_len,
            val_.get());
}

Block::Block(const Block &b, const size_t val_len) : meta_(b.meta_) {
  if (!b.val_)
    return;
  val_ = std::make_unique<uint8_t[]>(val_len);
  std::copy_n(b.val_.get() + sizeof(BlockMetadata), val_len, val_.get());
}

void Block::ToBytes(size_t val_len, uint8_t *out) {
  const auto meta_f = reinterpret_cast<const uint8_t *> (std::addressof(meta_));
  const auto meta_l = meta_f + sizeof(BlockMetadata);
  std::copy(meta_f, meta_l, out);
  if (val_)
    std::copy(val_.get(), val_.get() + val_len, out + sizeof(BlockMetadata));
}

Bucket::Bucket(uint8_t *data, size_t val_len) {
  bytes::FromBytes(data, meta_);
  size_t offset = sizeof(BucketMetadata);
  for (int i = 0; i < kBucketSize; ++i) {
    blocks_[i] = Block(data + offset, val_len);
    offset += BlockSize(val_len);
  }
}

std::unique_ptr<uint8_t[]> Bucket::ToBytes(size_t val_len) {
  auto res = std::make_unique<uint8_t[]>(BucketSize(val_len));
  ToBytes(res.get(), val_len);
  return res;
}

void Bucket::ToBytes(uint8_t *res, size_t val_len) {
  const auto meta_f = reinterpret_cast<const uint8_t *> (std::addressof(meta_));
  size_t offset = sizeof(BucketMetadata);
  const auto meta_l = meta_f + offset;
  std::copy(meta_f, meta_l, res);
  for (int i = 0; i < kBucketSize; ++i) {
    blocks_[i].ToBytes(val_len, res + offset);
    offset += BlockSize(val_len);
  }
}

ORam::ORam(size_t n, size_t val_len,
           bool with_pos_map, bool with_key_gen)
    : capacity_(n),
      num_buckets_(max(1, n - 1)),
      val_len_(val_len),
      store_(std::make_unique<store::RamStore>(
          num_buckets_, EncryptedBucketSize(val_len_))),
      depth_(max(0, ceil(log2(n)) - 1)),
      with_pos_map_(with_pos_map),
      with_key_gen_(with_key_gen),
      bucket_buffer_(std::make_unique<uint8_t[]>(BucketSize(val_len))),
      enc_bucket_buffer_(std::make_unique<uint8_t[]>(EncryptedBucketSize(val_len))) {}

ORam::ORam(size_t n, size_t val_len, const std::string &path,
           uint8_t max_levels_in_mem, bool with_pos_map, bool with_key_gen)
    : capacity_(n),
      num_buckets_(max(1, n - 1)),
      val_len_(val_len),
      depth_(max(0, ceil(log2(n)) - 1)),
      with_pos_map_(with_pos_map),
      with_key_gen_(with_key_gen),
      bucket_buffer_(std::make_unique<uint8_t[]>(BucketSize(val_len))),
      enc_bucket_buffer_(std::make_unique<uint8_t[]>(EncryptedBucketSize(val_len))) {
  if (path.empty() || max_levels_in_mem >= depth_) {
    store_ = std::make_unique<store::RamStore>(
        num_buckets_, EncryptedBucketSize(val_len_));
    return;
  }

  size_t mem_buckets = (2UL << max_levels_in_mem) - 1;
  size_t disk_buckets = num_buckets_ - mem_buckets;

  auto disk_store = store::PosixSingleFileStore::Construct(
      disk_buckets, EncryptedBucketSize(val_len_), path, true);
  if (!disk_store) {
    std::cerr << "Failed to create file store." << std::endl;
    store_ = std::make_unique<store::RamStore>(
        num_buckets_, EncryptedBucketSize(val_len_));
    return;
  }

  is_on_disk_ = true;
  if (!mem_buckets) {
    store_ = std::unique_ptr<store::Store>(disk_store.value());
    return;
  }

  auto mem_store = std::make_unique<store::RamStore>(
      mem_buckets, EncryptedBucketSize(val_len_));

  std::vector<std::unique_ptr<store::Store>> s;
  s.push_back(std::move(mem_store));
  s.emplace_back(disk_store.value());
  store_ = std::unique_ptr<store::Store>(
      new store::HybridStore(std::move(s), {mem_buckets, num_buckets_}));
}

Block ORam::ReadAndRemove(Pos p, Key k, crypto::Key enc_key) {
  if (with_pos_map_) {
    if (pos_map_.find(k) != pos_map_.end()) {
      p = pos_map_[k];
      pos_map_.erase(k);
    } else {
      DummyAccess(enc_key);
      auto empty = Block(true);
      return empty;
    }
  }

  Block res = ReadPath(p, k, enc_key);
  Evict(p, enc_key);
  auto it = stash_.begin();
  while (it < stash_.end()) {
    if (it->meta_.key_ == k && it->meta_.pos_ == p)
      break;
    ++it;
  }
  if (it != stash_.end()) {
    res = std::move(*it);
    stash_.erase(it);
  }
  if (res.meta_.key_)
    --size_;
  res.meta_.pos_ = GeneratePos();
  return res;
}

Block ORam::Read(Pos p, Key k, crypto::Key enc_key) {
  if (with_pos_map_) {
    if (pos_map_.find(k) != pos_map_.end()) {
      p = pos_map_[k];
      pos_map_.erase(k);
    } else {
      DummyAccess(enc_key);
      auto empty = Block(true);
      return empty;
    }
  }

  Block res = ReadPath(p, k, enc_key);

  auto new_p = GeneratePos();
  res.meta_.pos_ = new_p;
  if (with_pos_map_)
    pos_map_[k] = new_p;

  if (res.meta_.key_)
    stash_.emplace_back(res, val_len_);
  Evict(p, enc_key);
  for (Block &b : stash_) { // The requested block may be in stash.
    if (b.meta_.pos_ == p && b.meta_.key_ == k) {
      b.meta_.pos_ = new_p;
      res = Block(b, val_len_);
    }
  }
  return res;
}

void ORam::Insert(Block block, crypto::Key enc_key) {
  if (with_pos_map_) {
    block.meta_.pos_ = GeneratePos();
    pos_map_[block.meta_.key_] = block.meta_.pos_;
  }

  // Shouldn't deterministically be the same as block.pos_
  // Can give more control to the caller on what pos to evict.
  auto write_pos = GeneratePos();
  ReadPath(write_pos, 0, enc_key);
  stash_.push_back(std::move(block));
  Evict(write_pos, enc_key);
  ++size_;
}

Pos ORam::GeneratePos() const {
  Pos res;
  RAND_bytes(reinterpret_cast<unsigned char *>(&res), sizeof(Pos));
  res = (res % capacity_) + 1;
  return res;
}

unsigned int ORam::PathAtLevel(Pos pos, unsigned int level) const {
  unsigned int base = capacity_ - 1 + pos;
  if (capacity_ > 1) base /= 2;
  return (base / (1UL << (depth_ - level))) - 1;
}

std::vector<unsigned int> ORam::Path(Pos pos) const {
  assert(1 <= pos && pos <= capacity_);
  std::vector<unsigned int> res(depth_ + 1);
  unsigned int i = 0;
  unsigned int index = capacity_ - 1 + pos;
  if (capacity_ > 1) // Corner case
    index /= 2; // Skip last level
  while (index > 0) {
    res[i++] = index - 1; // index is 1-based but we need 0-based array indexes.
    index /= 2;
  }
  return res;
}

Block ORam::ReadPath(Pos p, Key k, crypto::Key enc_key) {
  Block res(true);
  auto path = Path(p);
  ++memory_access_count_;
  memory_access_bytes_total_ +=
      path.size() * sizeof(EncryptedBucketSize(val_len_));
  for (auto it = path.rbegin(); it < path.rend(); ++it) {
    auto idx = *it;
    if (!bucket_valid_[idx]) {
      break;
    }
    auto eb = store_->Read(idx);
    auto plen = crypto::Decrypt(eb, EncryptedBucketSize(val_len_),
                                enc_key, bucket_buffer_.get());
    assert(plen == BucketSize(val_len_));
    auto bu = Bucket(bucket_buffer_.get(), val_len_);
    bucket_valid_[(2 * idx) + 1] = bu.meta_.flags_ & kLeftChildValid;
    bucket_valid_[(2 * idx) + 2] = bu.meta_.flags_ & kRightChildValid;
    for (int i = 0; i < kBucketSize; ++i) {
      if (!(bu.meta_.flags_ & kBlockValid[i]))
        break;
      if (k == bu.blocks_[i].meta_.key_) {
        res = std::move(bu.blocks_[i]);
      } else {
        stash_.push_back(std::move(bu.blocks_[i]));
      }
    }
  }
  return res;
}

// Evict takes Pos as input as we can evict a different path than the path read.
void ORam::Evict(Pos p, crypto::Key enc_key) {
  auto path = Path(p);
  ++memory_access_count_;
  memory_access_bytes_total_ += path.size() * EncryptedBucketSize(val_len_);
  std::vector<bool> deleted_from_stash(stash_.size());
  unsigned int level = depth_;
  for (unsigned int idx : path) {
    Bucket bu;
    int bucket_index = 0;

    for (int i = 0; i < stash_.size() && bucket_index < kBucketSize; i++) {
      if (deleted_from_stash[i])
        continue;
      if (PathAtLevel(stash_[i].meta_.pos_, level) == idx) {
        bu.blocks_[bucket_index] = std::move(stash_[i]);
        deleted_from_stash[i] = true;
//        stash_.erase(stash_.begin() + i);
//        --i;
        bu.meta_.flags_ |= kBlockValid[bucket_index];
        ++bucket_index;
      }
    }

    bucket_valid_[idx] = true;
    if (bucket_valid_[(2 * idx) + 1])
      bu.meta_.flags_ |= kLeftChildValid;
    if (bucket_valid_[(2 * idx) + 2])
      bu.meta_.flags_ |= kRightChildValid;

    bu.ToBytes(bucket_buffer_.get(), val_len_);
    auto success = crypto::Encrypt(bucket_buffer_.get(), BucketSize(val_len_),
                                   enc_key, enc_bucket_buffer_.get());
    assert(success);
    store_->Write(idx, enc_bucket_buffer_.get());
    level--;
  }

  // Src: https://stackoverflow.com/a/33494562/3338591
  auto it = deleted_from_stash.begin();
  stash_.erase(
      std::remove_if(stash_.begin(),
                     stash_.end(),
                     [&](Block &) { return *it++; }),
      stash_.end()
  );
  bucket_valid_.clear();
  bucket_valid_[0] = true;
}

void ORam::DummyAccess(crypto::Key enc_key) {
  auto p = GeneratePos();
  ReadPath(p, 0, enc_key);
  Evict(p, enc_key);
}

// Should only be called after allocation.
void ORam::FillWithDummies(crypto::Key enc_key) {
  ++memory_access_count_;
  memory_access_bytes_total_ += num_buckets_ * EncryptedBucketSize(val_len_);
  Bucket empty;
  empty.ToBytes(bucket_buffer_.get(), val_len_);

  for (unsigned int i = 0; i < num_buckets_; ++i) {
    // Re-encrypt each bucket with fresh randomness
    bool ok = crypto::Encrypt(bucket_buffer_.get(), BucketSize(val_len_),
                              enc_key, enc_bucket_buffer_.get());
    assert(ok);
    store_->Write(i, enc_bucket_buffer_.get());
  }
}

Key ORam::NextKey() {
  assert(with_key_gen_);
  if (!freed_keys_.empty()) {
    Key res = freed_keys_.back();
    freed_keys_.pop_back();
    return res;
  }
  return next_key_++;
}

void ORam::AddFreedKey(Key key) {
  assert(with_key_gen_);
  if (key == next_key_ - 1)
    --next_key_;
  else
    freed_keys_.push_back(key);
}
} // namespace dyno::static_path_oram

