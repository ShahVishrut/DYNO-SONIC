#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace sn {
namespace util {
namespace bench {

#ifndef SN_BENCH_STRINGIFY_IMPL
#define SN_BENCH_STRINGIFY_IMPL(x) #x
#define SN_BENCH_STRINGIFY(x) SN_BENCH_STRINGIFY_IMPL(x)
#define SN_BENCH_STRINGIFY_DEFINED 1
#endif

inline const char* compiler_info() {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#elif defined(_MSC_FULL_VER)
  return "msvc " SN_BENCH_STRINGIFY(_MSC_FULL_VER);
#elif defined(_MSC_VER)
  return "msvc " SN_BENCH_STRINGIFY(_MSC_VER);
#else
  return "unknown compiler";
#endif
}

inline const char* build_config_info() {
#if defined(__OPTIMIZE_SIZE__)
  return "SizeOptimized";
#elif defined(__OPTIMIZE__)
#if defined(NDEBUG)
  return "Release";
#else
  return "Optimized";
#endif
#elif defined(NDEBUG)
  return "RelWithDebInfo";
#else
  return "Debug";
#endif
}

inline const char* target_arch_info() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__powerpc64__)
  return "ppc64";
#elif defined(__powerpc__)
  return "ppc";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown-arch";
#endif
}

struct benchmark_config {
  std::size_t buffer_size = 65536;
  std::size_t iterations_per_sample = 100000;
  std::size_t num_samples = 100;
};

struct stats {
  double mean;
  double std_dev;
  double min;
  double max;
};

struct duration_unit {
  double scale;
  const char* label;
};

inline duration_unit select_duration_unit(double max_ns) {
  if (max_ns >= 1e9) {
    return {1e-9, "s"};
  }
  if (max_ns >= 1e6) {
    return {1e-6, "ms"};
  }
  if (max_ns >= 1e3) {
    return {1e-3, "us"};
  }
  return {1.0, "ns"};
}

inline duration_unit select_duration_unit(const stats& s) {
  const double max_val = std::max({std::abs(s.mean), std::abs(s.std_dev), std::abs(s.min), std::abs(s.max)});
  return select_duration_unit(max_val);
}

inline stats compute_stats(double* samples, std::size_t count) {
  std::sort(samples, samples + count);

  double sum = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    sum += samples[i];
  }
  const double mean = sum / static_cast<double>(count);

  double variance_sum = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double diff = samples[i] - mean;
    variance_sum += diff * diff;
  }
  const double std_dev = std::sqrt(variance_sum / static_cast<double>(count));

  return {mean, std_dev, samples[0], samples[count - 1]};
}

class timer {
public:
  void start() { start_time_ = std::chrono::high_resolution_clock::now(); }

  double elapsed_ns() const {
    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time_);
    return static_cast<double>(duration.count());
  }

private:
  std::chrono::high_resolution_clock::time_point start_time_{};
};

inline void consume(std::uint64_t value) noexcept {
  [[maybe_unused]] static volatile std::uint64_t sink = 0;
  sink ^= value;
}

}
}
}

#ifdef SN_BENCH_STRINGIFY_DEFINED
#undef SN_BENCH_STRINGIFY
#undef SN_BENCH_STRINGIFY_IMPL
#undef SN_BENCH_STRINGIFY_DEFINED
#endif
