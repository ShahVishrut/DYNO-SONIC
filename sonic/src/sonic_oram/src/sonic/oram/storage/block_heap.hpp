#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <sys/mman.h>

#include "sonic/util/span.hpp"

namespace sn::oram::zingoram::storage {

// raii owner for a contiguous slab of blocks covering all buckets
template <typename Block> class block_heap {
public:
  using block_t = Block;

  block_heap() = default;

  ~block_heap() {
    if (storage_ && storage_ != MAP_FAILED) {
      munmap(storage_, total_slots_ * sizeof(block_t));
    }
  }

  // Non-copyable
  block_heap(const block_heap&) = delete;
  block_heap& operator=(const block_heap&) = delete;

  // Movable
  block_heap(block_heap&& other) noexcept 
      : storage_(other.storage_), 
        total_slots_(other.total_slots_), 
        slots_per_bucket_(other.slots_per_bucket_) {
    other.storage_ = nullptr;
    other.total_slots_ = 0;
    other.slots_per_bucket_ = 0;
  }
  
  block_heap& operator=(block_heap&& other) noexcept {
    if (this != &other) {
      if (storage_ && storage_ != MAP_FAILED) {
        munmap(storage_, total_slots_ * sizeof(block_t));
      }
      storage_ = other.storage_;
      total_slots_ = other.total_slots_;
      slots_per_bucket_ = other.slots_per_bucket_;
      other.storage_ = nullptr;
      other.total_slots_ = 0;
      other.slots_per_bucket_ = 0;
    }
    return *this;
  }

  void configure(std::size_t node_count_inclusive, std::size_t slots_per_bucket) {
    if (storage_ && storage_ != MAP_FAILED) {
      munmap(storage_, total_slots_ * sizeof(block_t));
    }
    slots_per_bucket_ = slots_per_bucket;
    total_slots_ = node_count_inclusive * slots_per_bucket_;
    
    // O(1) Lazy Allocation using mmap. 
    // Pages will be lazily mapped/zeroed by the OS only when accessed.
    storage_ = static_cast<block_t*>(mmap(nullptr, total_slots_ * sizeof(block_t), 
                                          PROT_READ | PROT_WRITE, 
                                          MAP_PRIVATE | MAP_ANON, -1, 0));
    if (storage_ == MAP_FAILED) {
      throw std::bad_alloc();
    }
  }

  [[nodiscard]] block_t* base_for(std::size_t node_id) noexcept {
    const std::size_t offset = node_id * slots_per_bucket_;
    return storage_ + offset;
  }

  [[nodiscard]] block_t* base_for_index(std::size_t index) noexcept {
    const std::size_t offset = index * slots_per_bucket_;
    return storage_ + offset;
  }

  [[nodiscard]] sn::util::span<block_t> span_for(std::size_t node_id) noexcept {
    return sn::util::span<block_t>(base_for(node_id), slots_per_bucket_);
  }

  [[nodiscard]] std::size_t slots_per_bucket() const noexcept { return slots_per_bucket_; }

private:
  block_t* storage_ = nullptr;
  std::size_t total_slots_ = 0;
  std::size_t slots_per_bucket_ = 0;
};

} // namespace sn::oram::zingoram::storage
