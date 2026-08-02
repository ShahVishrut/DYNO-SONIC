#include "omap.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "sonic/obliv/ops/core_ops.hpp"
#include "src/static/oram.h"
#include "src/utils/bytes.h"
#include "src/utils/crypto.h"
#include <iostream>
#include <vector>

#define max(a, b) ((a) > (b) ? (a) : (b))

namespace dyno::static_path_omap {

template <typename T>
inline bool my_ct_lt(T a, T b) {
    return a < b;
}

inline BlockPointer ct_select_bp(const BlockPointer &a, const BlockPointer &b, bool cond) {
  BlockPointer res;
  res.key_ = sn::obliv::ct_select<ORKey>(a.key_, b.key_, cond);
  res.pos_ = sn::obliv::ct_select<ORPos>(a.pos_, b.pos_, cond);
  return res;
}

Block::Block(uint8_t *data, size_t val_len) {
  if (!data)
    return;
  bytes::FromBytes(data, meta_);
  val_ = std::make_unique<uint8_t[]>(val_len);
  std::copy_n(data + sizeof(BlockMetadata), val_len, val_.get());
}

ORVal Block::ToBytes(size_t val_len) {
  ORVal res = std::make_unique<uint8_t[]>(BlockSize(val_len));
  const auto meta_f = reinterpret_cast<const uint8_t *>(std::addressof(meta_));
  std::copy_n(meta_f, sizeof(BlockMetadata), res.get());
  if (val_)
    std::copy_n(val_.get(), val_len, res.get() + sizeof(BlockMetadata));
  return res;
}

OMap::OMap(size_t n, size_t val_len, const std::string &path,
           uint8_t max_levels_in_mem)
    : capacity_(n), val_len_(val_len), max_depth_(ceil(1.44 * log2(n))), 
      pad_val_(ceil(1.44 * 3.0 * log2(n))), oram_(n + 1, BlockSize(val_len), false) {}

void OMap::FillWithDummies(crypto::Key enc_key) {
  // Sonic handles this internally on initialization if needed.
}

Val OMap::ReadAndRemove(Key k, crypto::Key enc_Key, bool &found) {
  auto replacement = Delete(k, root_, enc_Key);
  root_ = replacement;
  Val res = std::make_unique<uint8_t[]>(val_len_);
  std::fill_n(res.get(), val_len_, 0);
  
  if (delete_res_) {
    for (size_t i = 0; i < val_len_; ++i) {
      res.get()[i] = sn::obliv::ct_select<uint8_t>(delete_res_.get()[i], res.get()[i], delete_successful_);
    }
    delete_res_.reset();
  }
  
  found = delete_successful_;
  size_ -= sn::obliv::ct_select<size_t>(1, 0, delete_successful_);
  delete_successful_ = false;
  
  Finalize(enc_Key);
  return res;
}

Val OMap::Read(Key k, crypto::Key enc_Key) {
  // Use a dummy InsertRec to securely fetch nodes on the path
  // without modifying their values. We set updated = true
  // so do_update is false.
  bool inserted = true, updated = true;
  Val dummy_val = nullptr;
  root_ = InsertRec(k, dummy_val, root_, enc_Key, 0, inserted, updated, 0);

  Val res = std::make_unique<uint8_t[]>(val_len_);
  bool found = false;
  for (auto const &[key, block] : cache_) {
    bool is_match = (int)sn::obliv::ct_eq<Key>(k, block.meta_.key_) &
                    (int)!sn::obliv::ct_eq<ORKey>(key, 0);
    found = sn::obliv::ct_select<bool>(true, found, is_match);
    const uint8_t *src = block.val_.get();
    for (size_t i = 0; i < val_len_; ++i) {
      uint8_t bv = src ? src[i] : 0;
      res.get()[i] = sn::obliv::ct_select<uint8_t>(bv, res.get()[i], is_match);
    }
  }

  Finalize(enc_Key);
  return res;
}

void OMap::Insert(Key k, Val v, crypto::Key enc_key, bool is_dummy) {
  ORKey new_key = next_key_++;
  bool inserted = false;
  bool updated = false;
  root_ = InsertRec(k, v, root_, enc_key, 0, inserted, updated, new_key, is_dummy);
  
  inserted = sn::obliv::ct_select<bool>(false, inserted, is_dummy);
  if (inserted) {
    ++size_;
  }
  Finalize(enc_key);
}

BlockPointer OMap::InsertRec(Key k, Val &v, BlockPointer root_bp,
                             crypto::Key enc_key, uint32_t level,
                             bool &inserted, bool &updated, ORKey new_key, bool is_dummy) {
  if (level >= pad_val_) {
    return root_bp;
  }

  BlockPointer current_bp = root_bp;
  bool is_valid = !sn::obliv::ct_eq<ORKey>(current_bp.key_, 0);
  Block *b = Fetch(current_bp, enc_key);

  // Update current pointer with the newly assigned leaf from Sonic
  current_bp.pos_ = b->meta_.pos_;

  bool is_match = is_valid & sn::obliv::ct_eq<Key>(k, b->meta_.key_);
  bool do_update = is_match & !updated;
  updated = sn::obliv::ct_select<bool>(true, updated, do_update);
  if (do_update) {
    if (v)
      std::copy_n(v.get(), val_len_, b->val_.get());
    else
      std::fill_n(b->val_.get(), val_len_, 0);
  }

  bool hit_leaf = !is_valid & !inserted & !updated & !is_dummy;
  inserted = sn::obliv::ct_select<bool>(true, inserted, hit_leaf);

  if (hit_leaf) {
    cache_[new_key] = Block(k, nullptr, 1);
    cache_[new_key].meta_.pos_ = oram_.GenerateRandomLeaf();
    cache_[new_key].val_ = std::make_unique<uint8_t[]>(val_len_);
    if (v)
      std::copy_n(v.get(), val_len_, cache_[new_key].val_.get());
    else
      std::fill_n(cache_[new_key].val_.get(), val_len_, 0);
    current_bp.key_ = new_key;
    current_bp.pos_ = cache_[new_key].meta_.pos_;
    is_valid = true;
    b = &cache_[new_key];
  }

  bool go_left = my_ct_lt<Key>(k, b->meta_.key_);
  BlockPointer child_bp;
  child_bp.key_ =
      sn::obliv::ct_select<ORKey>(b->meta_.l_.key_, b->meta_.r_.key_, go_left);
  child_bp.pos_ =
      sn::obliv::ct_select<ORPos>(b->meta_.l_.pos_, b->meta_.r_.pos_, go_left);

  // Recurse deterministically
  BlockPointer new_child =
      InsertRec(k, v, child_bp, enc_key, level + 1, inserted, updated, new_key);

  // Reattach child (this captures the new_child's newly generated leaf)
  b->meta_.l_ = ct_select_bp(new_child, b->meta_.l_, go_left & is_valid);
  b->meta_.r_ = ct_select_bp(new_child, b->meta_.r_, !go_left & is_valid);

  // Adjust height
  uint8_t l_h = GetHeight(b->meta_.l_, enc_key);
  uint8_t r_h = GetHeight(b->meta_.r_, enc_key);
  uint8_t max_h = sn::obliv::ct_select<uint8_t>(
      l_h, r_h, my_ct_lt<uint8_t>(r_h, l_h));
  b->meta_.height_ =
      sn::obliv::ct_select<uint8_t>(1 + max_h, b->meta_.height_, is_valid);

  // Balance
  BlockPointer balanced = Balance(current_bp, enc_key);
  current_bp = ct_select_bp(balanced, current_bp, is_valid);

  return current_bp;
}

BlockPointer OMap::DeleteRec(Key k, BlockPointer current_bp, crypto::Key enc_key, uint32_t level) {
  if (level >= max_depth_) return current_bp;
  
  Block *b = Fetch(current_bp, enc_key);
  
  bool is_valid = !sn::obliv::ct_eq<ORKey>(current_bp.key_, 0);
  bool is_match = is_valid & sn::obliv::ct_eq<Key>(k, b->meta_.key_);
  
  bool do_delete = is_match & !delete_successful_;
  delete_successful_ = sn::obliv::ct_select<bool>(true, delete_successful_, do_delete);
  
  if (!delete_res_) {
    delete_res_ = std::make_unique<uint8_t[]>(val_len_);
    std::fill_n(delete_res_.get(), val_len_, 0);
  }
  for (size_t j = 0; j < val_len_; ++j) {
    uint8_t bv = b->val_ ? b->val_.get()[j] : 0;
    delete_res_.get()[j] = sn::obliv::ct_select<uint8_t>(bv, delete_res_.get()[j], do_delete);
  }
  
  for (size_t j = 0; j < val_len_; ++j) {
    uint8_t v = b->val_ ? b->val_[j] : 0;
    b->val_.get()[j] = sn::obliv::ct_select<uint8_t>(0, v, do_delete);
  }
  
  bool go_left = my_ct_lt<Key>(k, b->meta_.key_);
  bool keep_going = is_valid & !is_match;
  BlockPointer child_bp = ct_select_bp(b->meta_.l_, b->meta_.r_, go_left);
  child_bp = ct_select_bp(child_bp, {0, 0}, keep_going);
  
  BlockPointer new_child = DeleteRec(k, child_bp, enc_key, level + 1);
  
  bool is_left = keep_going & go_left;
  bool is_right = keep_going & !go_left;
  
  b->meta_.l_ = ct_select_bp(new_child, b->meta_.l_, is_left);
  b->meta_.r_ = ct_select_bp(new_child, b->meta_.r_, is_right);
  
  current_bp.pos_ = b->meta_.pos_;
  return current_bp;
}

BlockPointer OMap::Delete(Key k, BlockPointer root_bp, crypto::Key enc_key) {
  delete_successful_ = false;
  return DeleteRec(k, root_bp, enc_key, 0);
}

Block empty;
Block *OMap::Fetch(BlockPointer bp, crypto::Key enc_key) {
  if (!bp.key_) {
    // Pad access trace for dummy reads.
    ++accesses_before_finalize_;
    oram_.ReadAndRemove(0, 0, enc_key, false);

    // We allocate a dummy val_ so that ct_select operations in Delete don't
    // segfault on null pointers.
    if (cache_.find(0) == cache_.end()) {
      cache_[0] = Block(0, std::make_unique<uint8_t[]>(val_len_), 0);
      std::fill_n(cache_[0].val_.get(), val_len_, 0);
    }
    return &cache_[0];
  }

  if (cache_.find(bp.key_) != cache_.end()) {
    return &cache_[bp.key_];
  }

  if (!bp.pos_) {
    printf("ASSERTION FAILED: bp.key_=%llu, bp.pos_=%llu\n",
           (unsigned long long)bp.key_, (unsigned long long)bp.pos_);
    assert(bp.pos_);
  }
  ++accesses_before_finalize_;
  auto orb = oram_.ReadAndRemove(bp.pos_, bp.key_, enc_key);
  Block res(orb.val_.get(), val_len_);
  res.meta_.pos_ = orb.meta_.pos_;
  cache_[bp.key_] = std::move(res);
  return &cache_[bp.key_];
}

BlockPointer OMap::Balance(BlockPointer root_bp, crypto::Key enc_key) {
  BlockPointer ret_left_heavy = root_bp;
  BlockPointer ret_right_heavy = root_bp;

  Block *current_block = Fetch(root_bp, enc_key);
  // Update root_bp to have the new pos_ generated during Fetch
  root_bp.pos_ = current_block->meta_.pos_;

  int8_t bf = BalanceFactor(root_bp, enc_key);

  bool is_left_heavy = my_ct_lt<int8_t>(bf, -1);
  bool is_right_heavy = my_ct_lt<int8_t>(1, bf);

  // LEFT HEAVY LOGIC
  int8_t l_bf = BalanceFactor(current_block->meta_.l_, enc_key);
  bool do_left_right = my_ct_lt<int8_t>(0, l_bf);
  BlockPointer lr_rot = RotateLeft(current_block->meta_.l_, enc_key, do_left_right & is_left_heavy);

  current_block->meta_.l_ = ct_select_bp(lr_rot, current_block->meta_.l_,
                                         do_left_right & is_left_heavy);

  ret_left_heavy = RotateRight(root_bp, enc_key, is_left_heavy);

  // RIGHT HEAVY LOGIC
  int8_t r_bf = BalanceFactor(current_block->meta_.r_, enc_key);
  bool do_right_left = my_ct_lt<int8_t>(r_bf, 0);
  BlockPointer rl_rot = RotateRight(current_block->meta_.r_, enc_key, do_right_left & is_right_heavy);

  current_block->meta_.r_ = ct_select_bp(rl_rot, current_block->meta_.r_,
                                         do_right_left & is_right_heavy);

  ret_right_heavy = RotateLeft(root_bp, enc_key, is_right_heavy);

  BlockPointer ret = root_bp;
  ret = ct_select_bp(ret_left_heavy, ret, is_left_heavy);
  ret = ct_select_bp(ret_right_heavy, ret, is_right_heavy);

  bool valid = !sn::obliv::ct_eq<ORKey>(root_bp.key_, 0);
  BlockPointer res = ct_select_bp(ret, root_bp, valid);
  return res;
}

int8_t OMap::BalanceFactor(BlockPointer &bp, crypto::Key enc_key) {
  auto current_node = Fetch(bp, enc_key);
  bp.pos_ = current_node->meta_.pos_;
  auto lh = GetHeight(current_node->meta_.l_, enc_key);
  auto rh = GetHeight(current_node->meta_.r_, enc_key);
  return rh - lh;
}

uint8_t OMap::GetHeight(BlockPointer &bp, crypto::Key enc_key) {
  if (!bp.key_)
    return 0;
  Block *b = Fetch(bp, enc_key);
  bp.pos_ = b->meta_.pos_;
  return b->meta_.height_;
}

BlockPointer OMap::RotateLeft(BlockPointer root_bp, crypto::Key enc_key, bool cond) {
  auto p = Fetch(root_bp, enc_key);
  root_bp.pos_ = p->meta_.pos_;
  
  auto l = Fetch(p->meta_.l_, enc_key);
  p->meta_.l_.pos_ = l->meta_.pos_;
  
  auto r = Fetch(p->meta_.r_, enc_key);
  p->meta_.r_.pos_ = r->meta_.pos_;
  
  auto rl = Fetch(r->meta_.l_, enc_key);
  r->meta_.l_.pos_ = rl->meta_.pos_;
  
  auto rr = Fetch(r->meta_.r_, enc_key);
  r->meta_.r_.pos_ = rr->meta_.pos_;

  BlockPointer r_bp = p->meta_.r_;
  BlockPointer rl_bp = r->meta_.l_;

  auto res = r_bp;
  
  p->meta_.r_ = ct_select_bp(rl_bp, r_bp, cond);
  
  uint8_t m1 = sn::obliv::ct_select<uint8_t>(
      l->meta_.height_, rl->meta_.height_,
      my_ct_lt<uint8_t>(rl->meta_.height_, l->meta_.height_));
  uint8_t new_p_height = 1 + m1;
  p->meta_.height_ = sn::obliv::ct_select<uint8_t>(new_p_height, p->meta_.height_, cond);
  
  r->meta_.l_ = ct_select_bp(root_bp, rl_bp, cond);
  
  uint8_t m2 = sn::obliv::ct_select<uint8_t>(
      new_p_height, rr->meta_.height_,
      my_ct_lt<uint8_t>(rr->meta_.height_, new_p_height));
  uint8_t new_r_height = 1 + m2;
  r->meta_.height_ = sn::obliv::ct_select<uint8_t>(new_r_height, r->meta_.height_, cond);
  
  if (cache_.find(0) != cache_.end()) {
    cache_[0].meta_.l_ = {0, 0};
    cache_[0].meta_.r_ = {0, 0};
    cache_[0].meta_.height_ = 0;
  }

  return ct_select_bp(res, root_bp, cond);
}

BlockPointer OMap::RotateRight(BlockPointer root_bp, crypto::Key enc_key, bool cond) {
  auto p = Fetch(root_bp, enc_key);
  root_bp.pos_ = p->meta_.pos_;
  
  auto l = Fetch(p->meta_.l_, enc_key);
  p->meta_.l_.pos_ = l->meta_.pos_;
  
  auto r = Fetch(p->meta_.r_, enc_key);
  p->meta_.r_.pos_ = r->meta_.pos_;
  
  auto ll = Fetch(l->meta_.l_, enc_key);
  l->meta_.l_.pos_ = ll->meta_.pos_;
  
  auto lr = Fetch(l->meta_.r_, enc_key);
  l->meta_.r_.pos_ = lr->meta_.pos_;

  BlockPointer l_bp = p->meta_.l_;
  BlockPointer lr_bp = l->meta_.r_;

  auto res = l_bp;
  
  p->meta_.l_ = ct_select_bp(lr_bp, p->meta_.l_, cond);
  
  uint8_t m1 = sn::obliv::ct_select<uint8_t>(
      lr->meta_.height_, r->meta_.height_,
      my_ct_lt<uint8_t>(r->meta_.height_, lr->meta_.height_));
  uint8_t new_p_height = 1 + m1;
  p->meta_.height_ = sn::obliv::ct_select<uint8_t>(new_p_height, p->meta_.height_, cond);
  
  l->meta_.r_ = ct_select_bp(root_bp, lr_bp, cond);
  
  uint8_t m2 = sn::obliv::ct_select<uint8_t>(
      ll->meta_.height_, new_p_height,
      my_ct_lt<uint8_t>(new_p_height, ll->meta_.height_));
  uint8_t new_l_height = 1 + m2;
  l->meta_.height_ = sn::obliv::ct_select<uint8_t>(new_l_height, l->meta_.height_, cond);

  if (cache_.find(0) != cache_.end()) {
    cache_[0].meta_.l_ = {0, 0};
    cache_[0].meta_.r_ = {0, 0};
    cache_[0].meta_.height_ = 0;
  }

  return ct_select_bp(res, root_bp, cond);
}

void OMap::Finalize(crypto::Key enc_key) {
  while (accesses_before_finalize_ < pad_val_) {
    oram_.Read(0, 0, enc_key, false);
    ++accesses_before_finalize_;
  }
  accesses_before_finalize_ = 0;

  // Blocks in cache_ already contain their own updated new_leaf in meta_.pos_,
  // and their children's updated leaves in meta_.l_.pos_ and meta_.r_.pos_.
  for (auto &c : cache_) {
    ORKey ok = c.first;
    auto &b = c.second;
    ORPos op = b.meta_.pos_;
    auto ov = b.ToBytes(val_len_);
    b.val_.reset();
    oram_.Insert({op, ok, std::move(ov)}, enc_key);
  }
  auto writes_done = cache_.size();
  cache_.clear();

  // Pad writes (dummy writes to 0)
  while (writes_done++ < pad_val_) {
    static_path_oram::Block b(true);
    b.meta_.key_ = 0;
    b.val_ = std::make_unique<uint8_t[]>(BlockSize(val_len_));
    oram_.Insert(std::move(b), enc_key, false);
  }
}



BlockPointer OMap::TakeOneRec(BlockPointer current_bp, crypto::Key enc_key, bool &found, KeyValPair &res) {
  if (current_bp.key_ == 0) return current_bp;
  
  Block *b = Fetch(current_bp, enc_key);
  
  bool is_tombstone = true;
  if (b->val_) {
    for (size_t j = 0; j < val_len_; ++j) {
      if (b->val_[j] != 0) is_tombstone = false;
    }
  }
  
  bool is_target = !is_tombstone & !found;
  found = sn::obliv::ct_select<bool>(true, found, is_target);
  
  res.key_ = sn::obliv::ct_select<Key>(b->meta_.key_, res.key_, is_target);
  for (size_t j = 0; j < val_len_; ++j) {
    uint8_t bv = b->val_ ? b->val_.get()[j] : 0;
    res.val_.get()[j] = sn::obliv::ct_select<uint8_t>(bv, res.val_.get()[j], is_target);
  }
  for (size_t j = 0; j < val_len_; ++j) {
    uint8_t v = b->val_ ? b->val_[j] : 0;
    b->val_.get()[j] = sn::obliv::ct_select<uint8_t>(0, v, is_target);
  }
  
  b->meta_.l_ = TakeOneRec(b->meta_.l_, enc_key, found, res);
  b->meta_.r_ = TakeOneRec(b->meta_.r_, enc_key, found, res);
  
  current_bp.pos_ = b->meta_.pos_;
  return current_bp;
}

KeyValPair OMap::TakeOne(crypto::Key enc_key, bool &found) {
  found = false;
  KeyValPair res;
  res.key_ = 0;
  res.val_ = std::make_unique<uint8_t[]>(val_len_);
  std::fill_n(res.val_.get(), val_len_, 0);
  root_ = TakeOneRec(root_, enc_key, found, res);
  Finalize(enc_key);
  return res;
}

} // namespace dyno::static_path_omap
