#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <cstring>

#include "sonic/storage/io/backend.hpp"
#include "sonic/threads/sync.hpp"

namespace sn::storage::io {

class memory_backend : public backend {
public:
  explicit memory_backend(std::size_t page_bytes) : page_bytes_(page_bytes) {}

  void read_page(std::uint64_t page_id, void* dst, std::size_t bytes) override {
    sn::threads::lock_guard lock(mutex_);
    auto& buf = pages_[page_id];
    if (buf.empty()) {
      buf.assign(page_bytes_, std::byte{});
    }
    if (buf.size() < bytes) {
      buf.resize(bytes, std::byte{});
    }
    std::memcpy(dst, buf.data(), bytes);
  }

  void write_page(std::uint64_t page_id, const void* src, std::size_t bytes) override {
    sn::threads::lock_guard lock(mutex_);
    auto& buf = pages_[page_id];
    if (buf.size() < bytes) {
      buf.resize(bytes);
    }
    std::memcpy(buf.data(), src, bytes);
  }

  void flush() override {

  }

  void drop_cache() override {

  }

private:
  std::size_t page_bytes_;
  std::unordered_map<std::uint64_t, std::vector<std::byte>> pages_;
  mutable sn::threads::mutex mutex_;
};

}
