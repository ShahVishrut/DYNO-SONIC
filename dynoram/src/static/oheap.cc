#include "oheap.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "openssl/rand.h"

#include "src/utils/bytes.h"
#include "src/utils/crypto.h"
#include "src/store/ram_store.h"
#include "src/store/store.h"
#include "sonic/obliv/ops/core_ops.hpp"

namespace dyno::static_path_oheap {

static void ObliviousCopyBlock(Block& dest, const Block& src1, const Block& src2, bool cond, size_t val_len) {
    dest.meta_.pos_ = sn::obliv::ct_select<Pos>(src1.meta_.pos_, src2.meta_.pos_, cond);
    dest.meta_.key_ = sn::obliv::ct_select<Key>(src1.meta_.key_, src2.meta_.key_, cond);
    if (val_len > 0) {
        if (!dest.val_) dest.val_ = std::make_unique<uint8_t[]>(val_len);
        const uint8_t* s1 = src1.val_ ? src1.val_.get() : nullptr;
        const uint8_t* s2 = src2.val_ ? src2.val_.get() : nullptr;
        for (size_t i = 0; i < val_len; ++i) {
            uint8_t b1 = s1 ? s1[i] : 0;
            uint8_t b2 = s2 ? s2[i] : 0;
            dest.val_[i] = sn::obliv::ct_select<uint8_t>(b1, b2, cond);
        }
    }
}

Block::Block(uint8_t *data, size_t val_len) {
  bytes::FromBytes(data, meta_);
  val_ = std::make_unique<uint8_t[]>(val_len);
  std::copy_n(data + sizeof(BlockMetadata), val_len, val_.get());
}

Block::Block(const Block &b, const size_t val_len) : meta_(b.meta_) {
  if (!b.val_)
    return;
//  const auto v = new uint8_t[val_len];
//  std::copy_n(b.val_.get(), val_len, v);
//  val_ = std::unique_ptr<uint8_t[]>(v);
  val_ = std::make_unique<uint8_t[]>(val_len);
  std::copy_n(b.val_.get(), val_len, val_.get());
}

void Block::ToBytes(size_t val_len, uint8_t *out) {
  const auto meta_f = reinterpret_cast<const uint8_t *> (std::addressof(meta_));
  std::copy_n(meta_f, sizeof(BlockMetadata), out);
  if (val_)
    std::copy_n(val_.get(), val_len, out + sizeof(BlockMetadata));
}

Bucket::Bucket(uint8_t *data, size_t val_len) {
  bytes::FromBytes(data, meta_);
  size_t offset = sizeof(BucketMetadata);
  for (int i = 0; i < kBucketSize; ++i) {
    if (!(meta_.flags_ & kBlockValid[i]))
      break;
    blocks_[i] = Block(data + offset, val_len);
    offset += BlockSize(val_len);
  }
  min_block_ = Block(data + sizeof(BucketMetadata)
                         + (kBucketSize * BlockSize(val_len)), val_len);
}

std::unique_ptr<uint8_t[]> Bucket::ToBytes(size_t val_len) {
  auto res = std::make_unique<uint8_t[]>(BucketSize(val_len));
  ToBytes(res.get(), val_len);
  return res;
}

void Bucket::ToBytes(uint8_t *res, size_t val_len) {
  const auto meta_f = reinterpret_cast<const uint8_t *> (std::addressof(meta_));
  std::copy_n(meta_f, sizeof(BucketMetadata), res);
  size_t offset = sizeof(BucketMetadata);
  for (int i = 0; i < kBucketSize; ++i) {
    blocks_[i].ToBytes(val_len, res + offset);
    offset += BlockSize(val_len);
  }
  min_block_.ToBytes(val_len, res + offset);
}

OHeap::OHeap(size_t n, size_t val_len)
    : capacity_(n),
      val_len_(val_len),
      depth_(ceil(log2(n))),
      num_buckets_((2 * n) - 1),
      store_(std::make_unique<dyno::dynamic_stepping_path_oram::SonicORamAdapter>(
          num_buckets_ + 1, BucketSize(val_len), true)) {}

Block OHeap::FindMin(crypto::Key enc_key, bool pad) {
  ++memory_access_count_;
  memory_access_bytes_total_ += EncryptedBucketSize(val_len_);
  Block res(true);
  
  auto b = store_->Read(0, 1, enc_key, true);
  auto bu = Bucket(b.val_.get(), val_len_);
  res = std::move(bu.min_block_);
  
  for (const auto& b : stash_) {
    bool is_valid = (b.meta_.pos_ != 0);
    bool is_lt = sn::obliv::ct_lt<Key>(b.meta_.key_, res.meta_.key_);
    bool is_dummy = (res.meta_.pos_ == 0);
    bool do_update = is_valid & (is_dummy | is_lt);
    ObliviousCopyBlock(res, b, res, do_update, val_len_);
  }

  if (pad)
    DummyAccess(enc_key, false);
  return res;
}

Block OHeap::ExtractMin(crypto::Key enc_key, bool is_dummy) {
  Block min_block = FindMin(enc_key, false);
  
  bool force_dummy = is_dummy | (min_block.meta_.pos_ == 0);
  min_block.meta_.pos_ = sn::obliv::ct_select<Pos>(0, min_block.meta_.pos_, force_dummy);

  auto dummy_paths = GeneratePathPair();
  Pos first_pos = sn::obliv::ct_select<Pos>(dummy_paths.first, min_block.meta_.pos_, force_dummy);
  Pos second_pos = sn::obliv::ct_select<Pos>(dummy_paths.second, GenerateSecondPos(first_pos), force_dummy);

  ReadPath(first_pos, enc_key,
           !force_dummy, min_block.meta_.key_, &min_block.val_);
  UpdateMinAndEvict(first_pos, enc_key);
  ReadPath(second_pos, enc_key);
  UpdateMinAndEvict(second_pos, enc_key);

  size_ -= sn::obliv::ct_select<size_t>(0, 1, force_dummy);

  return min_block;
}

void OHeap::Insert(Key k, Val v, crypto::Key enc_key, bool is_dummy) {
  FindMin(enc_key, false); // To maintain obliviousness
  auto p = GeneratePos();
  auto evict_paths = GeneratePathPair();
  
  Pos final_pos = sn::obliv::ct_select<Pos>(0, p, is_dummy);
  stash_.emplace_back(final_pos, k, std::move(v));
  size_ += sn::obliv::ct_select<size_t>(0, 1, is_dummy);
  
  ReadPath(evict_paths.first, enc_key);
  UpdateMinAndEvict(evict_paths.first, enc_key);
  ReadPath(evict_paths.second, enc_key);
  UpdateMinAndEvict(evict_paths.second, enc_key);
}

Pos OHeap::GeneratePos() const {
  Pos res;
  RAND_bytes(reinterpret_cast<unsigned char *>(&res), sizeof(Pos));
  res = (res % capacity_) + 1;
  return res;
}

void OHeap::DummyAccess(crypto::Key enc_key, bool with_find_min) {
  if (with_find_min)
    FindMin(enc_key, false);
  auto p2 = GeneratePathPair();
  ReadPath(p2.first, enc_key);
  UpdateMinAndEvict(p2.first, enc_key);
  ReadPath(p2.second, enc_key);
  UpdateMinAndEvict(p2.second, enc_key);
}

// Should only be called after allocation.
void OHeap::FillWithDummies(crypto::Key enc_key) {
  ++memory_access_count_;
  memory_access_bytes_total_ += num_buckets_ * EncryptedBucketSize(val_len_);
  Bucket empty;
  empty.meta_.flags_ = 0;
  for (int j = 0; j < kBucketSize; ++j) {
      empty.blocks_[j] = dyno::static_path_oheap::Block(true);
  }
  empty.min_block_ = dyno::static_path_oheap::Block(true);

  for (size_t i = 0; i < num_buckets_; ++i) {
    dyno::static_path_oram::Block b(true);
    b.meta_.key_ = i + 1;
    b.val_ = empty.ToBytes(val_len_);
    store_->Insert(std::move(b), enc_key, true);
  }
}

void OHeap::ReadPath(Pos p, crypto::Key enc_key,
                     bool erase_if_found, Key k, Val *v) {
  bool found_res = false; // Duplicates are allowed
  auto path = Path(p);
  ++memory_access_count_;
  memory_access_bytes_total_ += path.size() * EncryptedBucketSize(val_len_);
  for (auto it = path.rbegin(); it < path.rend(); ++it) {
    auto idx = *it;

    auto sb = store_->ReadAndRemove(0, idx + 1, enc_key, true);
    auto bu = Bucket(sb.val_.get(), val_len_);

    for (int i = 0; i < kBucketSize; ++i) {
      bool is_valid = (bu.meta_.flags_ & kBlockValid[i]) != 0;
      
      bool is_target = (int)is_valid & (int)erase_if_found & 
                       sn::obliv::ct_eq<Pos>(p, bu.blocks_[i].meta_.pos_) & 
                       sn::obliv::ct_eq<Key>(k, bu.blocks_[i].meta_.key_);
      
      bool val_eq = true;
      if (v && v->get() && bu.blocks_[i].val_) {
         for (size_t j = 0; j < val_len_; ++j) {
            val_eq &= sn::obliv::ct_eq<uint8_t>(v->get()[j], bu.blocks_[i].val_.get()[j]);
         }
      } else if (v && v->get()) {
         val_eq = false;
      }
      is_target &= val_eq;
      
      bool do_erase = !found_res & is_target;
      found_res = sn::obliv::ct_select<bool>(true, found_res, do_erase);
      
      bu.blocks_[i].meta_.pos_ = sn::obliv::ct_select<Pos>(0, bu.blocks_[i].meta_.pos_, do_erase);
      bu.blocks_[i].meta_.key_ = sn::obliv::ct_select<Key>(0, bu.blocks_[i].meta_.key_, do_erase);
      
      if (is_valid) {
         stash_.push_back(std::move(bu.blocks_[i]));
      }
    }
    for (int i = 0; i < kBucketSize; ++i) bu.blocks_[i].val_.reset();
    bu.min_block_.val_.reset();
  }
  
  if (erase_if_found) {
    for (auto& b : stash_) {
      bool is_valid = (b.meta_.pos_ != 0);
      bool is_target = (int)is_valid & 
                       sn::obliv::ct_eq<Pos>(p, b.meta_.pos_) & 
                       sn::obliv::ct_eq<Key>(k, b.meta_.key_);
      
      bool val_eq = true;
      if (v && v->get() && b.val_) {
         for (size_t j = 0; j < val_len_; ++j) {
            val_eq &= sn::obliv::ct_eq<uint8_t>(v->get()[j], b.val_.get()[j]);
         }
      } else if (v && v->get()) {
         val_eq = false;
      }
      is_target &= val_eq;
      
      bool do_erase = !found_res & is_target;
      found_res = sn::obliv::ct_select<bool>(true, found_res, do_erase);
      
      b.meta_.pos_ = sn::obliv::ct_select<Pos>(0, b.meta_.pos_, do_erase);
      b.meta_.key_ = sn::obliv::ct_select<Key>(0, b.meta_.key_, do_erase);
    }
  }
}

void OHeap::UpdateMinAndEvict(Pos pos, crypto::Key enc_key) {
  auto path = Path(pos);
  ++memory_access_count_;
  memory_access_bytes_total_ += path.size() * EncryptedBucketSize(val_len_);
  std::vector<bool> deleted_from_stash(stash_.size());
  unsigned int level = depth_;
  Block children_min_block(true);
  for (unsigned int idx : path) {
    Bucket bu;
    int bucket_index = 0;
    for (int i = 0; i < stash_.size(); i++) {
      bool not_deleted = !deleted_from_stash[i];
      bool is_path = (PathAtLevel(stash_[i].meta_.pos_, level) == idx);
      bool has_space = sn::obliv::ct_lt<int>(bucket_index, kBucketSize);
      bool do_insert = not_deleted & is_path & has_space;

      for (int j = 0; j < kBucketSize; ++j) {
         bool match_j = sn::obliv::ct_eq<int>(bucket_index, j) & do_insert;
         ObliviousCopyBlock(bu.blocks_[j], stash_[i], bu.blocks_[j], match_j, val_len_);
         bu.meta_.flags_ = sn::obliv::ct_select<uint8_t>(bu.meta_.flags_ | kBlockValid[j], bu.meta_.flags_, match_j);
      }
      
      deleted_from_stash[i] = sn::obliv::ct_select<bool>(true, deleted_from_stash[i], do_insert);
      bucket_index = sn::obliv::ct_select<int>(bucket_index + 1, bucket_index, do_insert);
    }

    // find min block
    int min_i = -1;
    Key min_k = children_min_block.meta_.key_;
    for (int i = 0; i < kBucketSize; ++i) {
      bool is_valid = (bu.meta_.flags_ & kBlockValid[i]) != 0;
      bool children_min_dummy = (children_min_block.meta_.pos_ == 0);
      bool is_lt = sn::obliv::ct_lt<Key>(bu.blocks_[i].meta_.key_, min_k);
      
      bool update_min = is_valid & ((sn::obliv::ct_eq<int>(min_i, -1) & children_min_dummy) | is_lt);
      
      min_i = sn::obliv::ct_select<int>(i, min_i, update_min);
      min_k = sn::obliv::ct_select<Key>(bu.blocks_[i].meta_.key_, min_k, update_min);
    }

    // set min block
    Block chosen_block(true);
    for (int i = 0; i < kBucketSize; ++i) {
      bool match = sn::obliv::ct_eq<int>(min_i, i);
      ObliviousCopyBlock(chosen_block, bu.blocks_[i], chosen_block, match, val_len_);
    }
    bool use_child = sn::obliv::ct_eq<int>(min_i, -1);
    ObliviousCopyBlock(bu.min_block_, children_min_block, chosen_block, use_child, val_len_);

    // update children_min_block
    auto sibling_min_block = SiblingMin(idx, enc_key);
    bool sib_valid = (sibling_min_block.meta_.pos_ != 0);
    bool cur_valid = (bu.min_block_.meta_.pos_ != 0);
    bool sib_lt = sn::obliv::ct_lt<Key>(sibling_min_block.meta_.key_, bu.min_block_.meta_.key_);
    
    bool update_child = sib_valid & (!cur_valid | sib_lt);
    bool use_cur = !update_child & !sn::obliv::ct_eq<int>(min_i, -1);
    
    Block next_children_min_block(true);
    ObliviousCopyBlock(next_children_min_block, sibling_min_block, children_min_block, update_child, val_len_);
    ObliviousCopyBlock(children_min_block, bu.min_block_, next_children_min_block, use_cur, val_len_);

    dyno::static_path_oram::Block sb(true);
    sb.meta_.key_ = idx + 1;
    sb.val_ = bu.ToBytes(val_len_);
    store_->Insert(std::move(sb), enc_key, true);
    --level;
    sibling_min_block.val_.reset();
  }

  auto it = deleted_from_stash.begin();
  stash_.erase(
      std::remove_if(stash_.begin(), stash_.end(),
                     [&](Block &b) { bool del = *it++; return del || b.meta_.pos_ == 0; }),
      stash_.end()
  );
}

Block OHeap::SiblingMin(unsigned int idx, crypto::Key enc_key) {
  unsigned int sibling_idx = idx % 2 ? idx + 1 : idx - 1;
  if (idx == 0)
    return Block(true);

  memory_access_bytes_total_ += EncryptedBucketSize(val_len_);

  auto sb = store_->Read(0, sibling_idx + 1, enc_key, true);
  auto bu = Bucket(sb.val_.get(), val_len_);
  return std::move(bu.min_block_);
}

std::vector<unsigned int> OHeap::Path(Pos pos) const {
  assert(1 <= pos && pos <= capacity_);
  std::vector<unsigned int> res(depth_ + 1);
  unsigned int i = 0;
  unsigned int index = capacity_ - 1 + pos;
  while (index > 0) {
    res[i++] = index - 1; // index is 1-based but we need 0-based array indexes.
    index /= 2;
  }
  return res;
}

unsigned int OHeap::PathAtLevel(Pos pos, unsigned int level) const {
  if (pos == 0) return 0xFFFFFFFF; // Dummies never intersect
  return ((capacity_ - 1 + pos) / (1UL << (depth_ - level))) - 1;
}

std::pair<Pos, Pos> OHeap::GeneratePathPair() const {
  // 1 .. 2^{k-1}
  Pos pos1 = 1 + ((GeneratePos() - 1) >> 1);
  // 2^{k-1}+1 .. 2^k
  Pos pos2 = 1 + (((GeneratePos() - 1) >> 1) | (capacity_ >> 1));
  return std::make_pair(pos1, pos2);
}

Pos OHeap::GenerateSecondPos(Pos p) const {
  // 2^{k-1} if p >= 2^{k-1}; else 0
  Pos base = ((capacity_ >> 1) & (p - 1)) ^ (capacity_ >> 1);
  return (base | ((GeneratePos() - 1) >> 1)) + 1;
}
} // namespace dyno::static_path_oheap
