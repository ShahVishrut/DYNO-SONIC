#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/util/span.hpp"

namespace sn::storage::io {

class backend {
public:
  virtual ~backend() = default;

  virtual void read_page(std::uint64_t page_id, void* dst, std::size_t bytes) = 0;
  virtual void write_page(std::uint64_t page_id, const void* src, std::size_t bytes) = 0;

  struct page_view {
    std::uint64_t page_id;
    const void* src;
    std::size_t bytes;
  };
  virtual void write_pages(sn::util::span<const page_view> pages) {
    for (const auto& p : pages) {
      write_page(p.page_id, p.src, p.bytes);
    }
  }

  virtual void flush() {}
  virtual void drop_cache() {}
  virtual void hint_prefetch(std::uint64_t, std::size_t) {}
  virtual void resize(std::uint64_t, std::size_t) {}
};

}
