#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "sonic/util/picoformat.hpp"
#include "sonic/util/humanize.hpp"

namespace sn::storage::cache {

struct stats_snapshot {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t loads = 0;
  std::uint64_t evictions = 0;
  std::uint64_t dirty_evictions = 0;
  std::uint64_t clean_evictions = 0;
  std::uint64_t writebacks = 0;
  std::uint64_t flushes = 0;
  std::uint64_t read_ios = 0;
  std::uint64_t write_ios = 0;
  std::uint64_t prefetches = 0;
  std::uint64_t read_bytes = 0;
  std::uint64_t write_bytes = 0;
  std::uint64_t peak_resident = 0;
};

struct stats {
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> misses{0};
  std::atomic<std::uint64_t> loads{0};
  std::atomic<std::uint64_t> evictions{0};
  std::atomic<std::uint64_t> dirty_evictions{0};
  std::atomic<std::uint64_t> clean_evictions{0};
  std::atomic<std::uint64_t> writebacks{0};
  std::atomic<std::uint64_t> flushes{0};
  std::atomic<std::uint64_t> read_ios{0};
  std::atomic<std::uint64_t> write_ios{0};
  std::atomic<std::uint64_t> prefetches{0};
  std::atomic<std::uint64_t> read_bytes{0};
  std::atomic<std::uint64_t> write_bytes{0};
  std::atomic<std::uint64_t> peak_resident{0};

  void reset() noexcept {
    hits.store(0, std::memory_order_relaxed);
    misses.store(0, std::memory_order_relaxed);
    loads.store(0, std::memory_order_relaxed);
    evictions.store(0, std::memory_order_relaxed);
    dirty_evictions.store(0, std::memory_order_relaxed);
    clean_evictions.store(0, std::memory_order_relaxed);
    writebacks.store(0, std::memory_order_relaxed);
    flushes.store(0, std::memory_order_relaxed);
    read_ios.store(0, std::memory_order_relaxed);
    write_ios.store(0, std::memory_order_relaxed);
    prefetches.store(0, std::memory_order_relaxed);
    read_bytes.store(0, std::memory_order_relaxed);
    write_bytes.store(0, std::memory_order_relaxed);
    peak_resident.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] stats_snapshot snapshot() const noexcept {
    stats_snapshot s{};
    s.hits = hits.load(std::memory_order_relaxed);
    s.misses = misses.load(std::memory_order_relaxed);
    s.loads = loads.load(std::memory_order_relaxed);
    s.evictions = evictions.load(std::memory_order_relaxed);
    s.dirty_evictions = dirty_evictions.load(std::memory_order_relaxed);
    s.clean_evictions = clean_evictions.load(std::memory_order_relaxed);
    s.writebacks = writebacks.load(std::memory_order_relaxed);
    s.flushes = flushes.load(std::memory_order_relaxed);
    s.read_ios = read_ios.load(std::memory_order_relaxed);
    s.write_ios = write_ios.load(std::memory_order_relaxed);
    s.prefetches = prefetches.load(std::memory_order_relaxed);
    s.read_bytes = read_bytes.load(std::memory_order_relaxed);
    s.write_bytes = write_bytes.load(std::memory_order_relaxed);
    s.peak_resident = peak_resident.load(std::memory_order_relaxed);
    return s;
  }
};

inline std::string format_stats(const stats_snapshot& s, std::uint64_t app_ops = 0) {
  const std::uint64_t cache_ops = s.hits + s.misses;
  const double cache_ops_d = static_cast<double>(cache_ops);
  const double hit_rate = cache_ops_d > 0.0 ? (static_cast<double>(s.hits) / cache_ops_d) * 100.0 : 0.0;

  std::string formatted = ::pfm::format(
      "cache ops=%llu hit=%.2f%% io=%llu/%llu bytes=%s/%s",
      static_cast<unsigned long long>(cache_ops), hit_rate, static_cast<unsigned long long>(s.read_ios),
      static_cast<unsigned long long>(s.write_ios), sn::util::humanize::bytes(s.read_bytes),
      sn::util::humanize::bytes(s.write_bytes)
  );

  if (app_ops > 0) {
    const double op = static_cast<double>(app_ops);
    const double read_io_per = static_cast<double>(s.read_ios) / op;
    const double write_io_per = static_cast<double>(s.write_ios) / op;
    const double read_bytes_per = static_cast<double>(s.read_bytes) / op;
    const double write_bytes_per = static_cast<double>(s.write_bytes) / op;
    formatted = ::pfm::format(
        "%s app_ops=%llu io/op=%.3f/%.3f bytes/op=%.1f/%.1f", formatted,
        static_cast<unsigned long long>(app_ops), read_io_per, write_io_per, read_bytes_per, write_bytes_per
    );
  }

  return formatted;
}

}
