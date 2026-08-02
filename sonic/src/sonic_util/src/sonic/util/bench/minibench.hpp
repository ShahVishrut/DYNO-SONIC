#pragma once

#include "sonic/util/oe/math_compat.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SONIC_NO_OS)
#define SN_MINIBENCH_HAS_OS 0
#else
#define SN_MINIBENCH_HAS_OS 1
#endif

#if SN_MINIBENCH_HAS_OS
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#endif

#include "sonic/util/cputimer.hpp"
#include "sonic/util/bench/benchstats.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if SN_MINIBENCH_HAS_OS
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif
#endif

namespace sn::util::bench {

struct cycle_environment {
  double ns_per_cycle = 1.0;
  double resolution_cycles = 1.0;
  double cost_cycles = 1.0;
  bool clock_stable = true;

  double resolution_ns() const noexcept { return resolution_cycles * ns_per_cycle; }
  double cost_ns() const noexcept { return cost_cycles * ns_per_cycle; }
};

namespace detail {

namespace platform {

#if SN_MINIBENCH_HAS_OS
inline unsigned normalize_core_index(unsigned requested_core) noexcept {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) {
    return requested_core;
  }
  return requested_core % hw;
}

#if defined(__linux__)
inline bool set_thread_affinity(unsigned normalized_core) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  const unsigned limited = normalized_core % CPU_SETSIZE;
  CPU_SET(limited, &set);
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}
#elif defined(_WIN32)
inline bool set_thread_affinity(unsigned normalized_core) noexcept {
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << (normalized_core % (sizeof(DWORD_PTR) * 8));
  const DWORD_PTR previous = ::SetThreadAffinityMask(::GetCurrentThread(), mask);
  return previous != 0;
}
#elif defined(__APPLE__)
inline bool set_thread_affinity(unsigned normalized_core) noexcept {
  thread_port_t mach_thread = mach_thread_self();
  thread_affinity_policy_data_t policy = {static_cast<integer_t>(normalized_core)};
  const kern_return_t result =
      thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy), 1);
  mach_port_deallocate(mach_task_self(), mach_thread);
  return result == KERN_SUCCESS;
}
#else
inline bool set_thread_affinity(unsigned normalized_core) noexcept {
  (void) normalized_core;
  return false;
}
#endif

inline void report_pin_attempt(unsigned normalized_core, bool pinned, bool verbose) {
  if (!verbose) {
    return;
  }
  static std::once_flag report_once;
  std::call_once(report_once, [pinned]() {
    std::cout << "[minibench] pin " << (pinned ? "ok" : "failed") << '\n';
  });
}

inline bool probe_clock_stability(const cycle_environment& env) noexcept {
  using steady = std::chrono::steady_clock;
  const auto t0 = steady::now();
  const auto c0 = sn::util::cpu_timer::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto t1 = steady::now();
  const auto c1 = sn::util::cpu_timer::now();
  const auto delta_cycles = c1 - c0;
  if (delta_cycles == 0) {
    return false;
  }
  const auto delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  if (delta_ns <= 0) {
    return false;
  }
  const double measured = static_cast<double>(delta_ns) / static_cast<double>(delta_cycles);
  if (!sn::util::math::is_finite(measured) || measured <= 0.0) {
    return false;
  }
  const double drift = std::abs(measured - env.ns_per_cycle) / env.ns_per_cycle;
  return drift <= 0.01;
}
#else
inline unsigned normalize_core_index(unsigned requested_core) noexcept { return requested_core; }

inline bool set_thread_affinity(unsigned normalized_core) noexcept {
  (void) normalized_core;
  return false;
}

inline void report_pin_attempt(unsigned normalized_core, bool pinned, bool verbose) noexcept {
  (void) normalized_core;
  (void) pinned;
  (void) verbose;
}

inline bool probe_clock_stability(const cycle_environment&) noexcept { return true; }
#endif

}

inline void compiler_barrier() noexcept {
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

template <class T> inline void do_not_optimize(const T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("" : : "g"(value) : "memory");
#elif defined(_MSC_VER)
  _ReadWriteBarrier();
  (void) *reinterpret_cast<volatile const unsigned char*>(&value);
  _ReadWriteBarrier();
#else
  auto volatile ptr = &value;
  (void) ptr;
#endif
}

template <class T> inline T&& black_box(T&& value) noexcept {
  do_not_optimize(value);
  return std::forward<T>(value);
}

inline double measure_empty_loop(int repeats) noexcept {
  if (repeats <= 0) {
    return 0.0;
  }
  compiler_barrier();
  const auto start = sn::util::cpu_timer::now();
  for (int i = 0; i < repeats; ++i) {
    compiler_barrier();
  }
  const auto end = sn::util::cpu_timer::now();
  compiler_barrier();
  const auto diff = static_cast<double>(end - start);
  return diff > 0.0 ? diff : 0.0;
}

inline cycle_environment measure_cycle_environment() noexcept {
  cycle_environment env{};
  env.ns_per_cycle = sn::util::cpu_timer::nanoseconds_per_cycle();

  double best = std::numeric_limits<double>::max();
  double sum_positive = 0.0;
  int count_positive = 0;

  constexpr int samples = 2048;
  for (int i = 0; i < samples; ++i) {
    const auto a = sn::util::cpu_timer::now();
    const auto b = sn::util::cpu_timer::now();
    const auto delta = static_cast<double>(b - a);
    if (delta > 0.0) {
      best = std::min(best, delta);
      sum_positive += delta;
      ++count_positive;
    }
  }

  if (best == std::numeric_limits<double>::max()) {
    best = 1.0;
  }
  env.resolution_cycles = best;
  if (count_positive > 0) {
    env.cost_cycles = sum_positive / static_cast<double>(count_positive);
  } else {
    env.cost_cycles = best;
  }

  env.clock_stable = platform::probe_clock_stability(env);
  return env;
}

struct pin_options {
  bool enable = false;
  unsigned core = 0;
  bool verbose = false;
};

inline bool maybe_pin_thread(const pin_options& opts) {
  if (!opts.enable) {
    return false;
  }
  const unsigned normalized = platform::normalize_core_index(opts.core);
  const bool pinned = platform::set_thread_affinity(normalized);
  platform::report_pin_attempt(normalized, pinned, opts.verbose);
  return pinned;
}

template <typename Func, bool TakesIndex> struct invocation_result;

template <typename Func> struct invocation_result<Func, true> {
  using type = std::invoke_result_t<Func, int>;
};

template <typename Func> struct invocation_result<Func, false> {
  using type = std::invoke_result_t<Func>;
};

template <typename T, bool IsVoid = std::is_void_v<T>> class value_slot;

template <typename T> class value_slot<T, false> {
public:
  using stored_type = std::decay_t<T>;

  value_slot() = default;
  value_slot(const value_slot&) = delete;
  value_slot& operator=(const value_slot&) = delete;
  value_slot(value_slot&& other) noexcept : engaged_(other.engaged_) {
    if (engaged_) {
      new (&storage_) stored_type(std::move(*other.ptr()));
    }
    other.reset();
  }
  value_slot& operator=(value_slot&&) = delete;

  ~value_slot() { reset(); }

  template <typename U> void set(U&& value) {
    reset();
    new (&storage_) stored_type(std::forward<U>(value));
    engaged_ = true;
  }

  bool has_value() const noexcept { return engaged_; }

  stored_type take() {
    if (!engaged_) {
      std::abort();
    }
    stored_type result = std::move(*ptr());
    reset();
    return result;
  }

private:
  void reset() noexcept {
    if (engaged_) {
      ptr()->~stored_type();
      engaged_ = false;
    }
  }

  stored_type* ptr() noexcept { return reinterpret_cast<stored_type*>(&storage_); }

  alignas(stored_type) unsigned char storage_[sizeof(stored_type)]{};
  bool engaged_ = false;
};

template <typename T> class value_slot<T, true> {
public:
  void set() noexcept {}
  bool has_value() const noexcept { return false; }
  void take() noexcept {}
};

class tail_sampler {
public:
  tail_sampler(double rate, const cycle_environment& env) : rate_(rate), env_(env) {
    samples_.reserve(static_cast<std::size_t>(kMaxSamples / 2));
  }

  bool enabled() const noexcept { return rate_ > 0.0; }

  bool has_samples() const noexcept { return !samples_.empty(); }

  void quantiles(double& p95, double& p99) const {
    if (samples_.empty()) {
      return;
    }
    auto sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    p95 = sn::util::bench::quantile(sorted, 0.95);
    p99 = sn::util::bench::quantile(sorted, 0.99);
  }

  bool should_sample() { return enabled() && next_uniform() <= rate_; }

  template <typename Callable> auto measure(Callable&& callable) {
    using result_t = std::invoke_result_t<Callable&&>;
    detail::compiler_barrier();
    const auto start = sn::util::cpu_timer::now();
    if constexpr (std::is_void_v<result_t>) {
      std::forward<Callable>(callable)();
      detail::compiler_barrier();
      const auto end = sn::util::cpu_timer::now();
      detail::compiler_barrier();
      record_sample(end - start);
    } else {
      auto result = std::forward<Callable>(callable)();
      detail::compiler_barrier();
      const auto end = sn::util::cpu_timer::now();
      detail::compiler_barrier();
      record_sample(end - start);
      return result;
    }
  }

private:
  static constexpr std::size_t kMaxSamples = 8192;

  double rate_ = 0.0;
  cycle_environment env_{};
  std::vector<double> samples_;
  std::uint64_t rng_state_ = 0x9e3779b97f4a7c15ull;
  std::size_t total_seen_ = 0;

  std::uint64_t next_random() {
    rng_state_ = rng_state_ * 6364136223846793005ull + 1ull;
    return rng_state_;
  }

  double next_uniform() {
    const auto r = next_random();
    return static_cast<double>((r >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 53);
  }

  void record_sample(std::uint64_t cycles) {
    double corrected = static_cast<double>(cycles) - env_.cost_cycles;
    if (corrected < 0.0) {
      corrected = 0.0;
    }
    const double ns = corrected * env_.ns_per_cycle;
    ++total_seen_;
    if (samples_.size() < kMaxSamples) {
      samples_.push_back(ns);
      return;
    }
    const std::size_t idx = static_cast<std::size_t>(next_random() % total_seen_);
    if (idx < samples_.size()) {
      samples_[idx] = ns;
    }
  }
};

template <typename Func> class invocation_helper {
public:
  using func_type = std::decay_t<Func>;
  static constexpr bool takes_index = std::is_invocable_v<func_type&, int>;
  using result_type = typename invocation_result<func_type&, takes_index>::type;

  static void invoke(
      func_type& func, int iterations, const std::optional<std::reference_wrapper<tail_sampler>>& tail = std::nullopt
  ) {
    const bool tail_enabled = tail.has_value();
    if (!tail_enabled) {
      invoke_plain(func, iterations);
      return;
    }
    auto& sampler = tail->get();

    if constexpr (std::is_void_v<result_type>) {
      if constexpr (takes_index) {
        for (int i = 0; i < iterations; ++i) {
          if (sampler.should_sample()) {
            sampler.measure([&]() { std::invoke(func, i); });
          } else {
            std::invoke(func, i);
          }
        }
      } else {
        for (int i = 0; i < iterations; ++i) {
          if (sampler.should_sample()) {
            sampler.measure([&]() { std::invoke(func); });
          } else {
            std::invoke(func);
          }
        }
      }
    } else {
      value_slot<result_type> slot;
      if constexpr (takes_index) {
        for (int i = 0; i < iterations; ++i) {
          if (sampler.should_sample()) {
            slot.set(sampler.measure([&]() { return std::invoke(func, i); }));
          } else {
            slot.set(std::invoke(func, i));
          }
        }
      } else {
        for (int i = 0; i < iterations; ++i) {
          if (sampler.should_sample()) {
            slot.set(sampler.measure([&]() { return std::invoke(func); }));
          } else {
            slot.set(std::invoke(func));
          }
        }
      }
      if (slot.has_value()) {
        detail::black_box(slot.take());
      }
    }
  }

private:
  static void invoke_plain(func_type& func, int iterations) {
    if constexpr (std::is_void_v<result_type>) {
      if constexpr (takes_index) {
        for (int i = 0; i < iterations; ++i) {
          std::invoke(func, i);
        }
      } else {
        for (int i = 0; i < iterations; ++i) {
          std::invoke(func);
        }
      }
    } else {
      value_slot<result_type> slot;
      if constexpr (takes_index) {
        for (int i = 0; i < iterations; ++i) {
          slot.set(std::invoke(func, i));
        }
      } else {
        for (int i = 0; i < iterations; ++i) {
          slot.set(std::invoke(func));
        }
      }
      if (slot.has_value()) {
        detail::black_box(slot.take());
      }
    }
  }
};

struct sample_capture {
  double per_iteration_ns = 0.0;
  bool recorded = false;
};

template <typename Func>
sample_capture measure_callable(Func& func, int iterations, const cycle_environment& env, double overhead_cycles) {
  if (iterations <= 0) {
    return {0.0, false};
  }
  compiler_barrier();
  const auto start = sn::util::cpu_timer::now();
  invocation_helper<Func>::invoke(func, iterations);
  compiler_barrier();
  const auto end = sn::util::cpu_timer::now();
  compiler_barrier();

  if (end <= start) {
    return {0.0, false};
  }
  const double cycles = static_cast<double>(end - start);
  double corrected = cycles - overhead_cycles;
  if (corrected < 0.0) {
    corrected = 0.0;
  }
  const double per_iter = corrected * env.ns_per_cycle / static_cast<double>(iterations);
  return {per_iter, true};
}

}

struct latency_options {
  int min_samples = 16;
  int max_samples = 200;
  int min_iterations = 1;
  int max_iterations = 1 << 21;
  double target_rel_error = 0.02;
  double min_sample_ns = 2.0e5;
  double warmup_budget_ns = 1.0e8;
  bool pin_thread = false;
  unsigned pin_core = 0;
  bool verbose = false;
  bool reject_outliers = true;
  double outlier_mad_k = 4.0;
  unsigned bootstrap_samples = 0;
  std::uint64_t bootstrap_seed = 0;
  double tail_sample_rate = 0.0;
};

struct latency_result {
  std::vector<double> samples_ns;
  double mean_ns = 0.0;
  double median_ns = 0.0;
  double q1_ns = 0.0;
  double q3_ns = 0.0;
  double min_ns = 0.0;
  double max_ns = 0.0;
  double stddev_ns = 0.0;
  double std_error_ns = 0.0;
  double cv_percent = 0.0;
  confidence_interval ci{};
  int iterations_per_sample = 1;
  int samples_recorded = 0;
  int samples_rejected = 0;
  int warmup_runs = 0;
  std::uint64_t total_iterations = 0;
  cycle_environment env{};
  double tail_p95_ns = 0.0;
  double tail_p99_ns = 0.0;
  bool tail_available = false;
};

struct throughput_options {
  std::uint64_t iteration_count = 0;
  std::uint64_t batch_size = 0;
  double warmup_budget_ns = 1.0e8;
  bool pin_thread = false;
  unsigned pin_core = 0;
  bool verbose = false;
  bool record_windows = true;
  int max_windows = 0;
};

struct throughput_result {
  std::vector<double> batch_ops_per_sec;
  double total_time_ns = 0.0;
  double total_ops_per_sec = 0.0;
  double mean_ops_per_sec = 0.0;
  double median_ops_per_sec = 0.0;
  double min_ops_per_sec = 0.0;
  double max_ops_per_sec = 0.0;
  confidence_interval ci{};
  std::uint64_t iterations = 0;
  std::uint64_t batch_size = 0;
  int batches = 0;
  int warmup_batches = 0;
  cycle_environment env{};
};

namespace detail {

class latency_accumulator {
public:
  latency_accumulator(latency_options opts, cycle_environment env) :
      opts_(std::move(opts)),
      env_(env),
      stats_(
          stat_accumulator_options{
              0, opts_.reject_outliers, opts_.outlier_mad_k, true,
              static_cast<std::size_t>(opts_.max_samples > 0 ? opts_.max_samples : 0), 0x9e3779b97f4a7c15ull
          }
      ),
      tail_sampler_(opts_.tail_sample_rate, env_) {}

  bool record_cycles(double cycles, int iterations) {
    if (iterations <= 0) {
      return false;
    }
    double corrected = cycles - overhead_cycles_;
    if (corrected < 0.0) {
      corrected = 0.0;
    }
    const double per_iter = corrected * env_.ns_per_cycle / static_cast<double>(iterations);
    return record_value(per_iter, iterations);
  }

  bool record_ns(double elapsed_ns, int iterations, bool already_per_iteration) {
    if (iterations <= 0) {
      return false;
    }
    const double per_iter = already_per_iteration ? elapsed_ns : (elapsed_ns / static_cast<double>(iterations));
    return record_value(per_iter, iterations);
  }

  bool should_continue() const {
    const int attempts = static_cast<int>(stats_.recorded() + stats_.rejected());
    if (attempts >= opts_.max_samples) {
      return false;
    }
    if (static_cast<int>(stats_.recorded()) < opts_.min_samples) {
      return true;
    }
    if (stats_.mean() <= 0.0) {
      return attempts < opts_.max_samples;
    }
    if (stats_.recorded() < 2) {
      return attempts < opts_.max_samples;
    }
    const double variance = stats_.sample_variance();
    if (variance <= 0.0) {
      return false;
    }
    if (opts_.target_rel_error <= 0.0) {
      return attempts < opts_.max_samples;
    }
    const double stddev = std::sqrt(variance);
    const double std_error = stddev / std::sqrt(static_cast<double>(stats_.recorded()));
    const double t_value = sn::util::bench::t_critical_95(static_cast<int>(stats_.recorded()) - 1);
    if (t_value <= 0.0) {
      return attempts < opts_.max_samples;
    }
    const double rel_half_width = (t_value * std_error) / stats_.mean();
    return rel_half_width > opts_.target_rel_error;
  }

  latency_result finalize() {
    latency_result result{};
    stats_view_options view{};
    if (opts_.bootstrap_samples > 0) {
      view.ci = ci_method::bootstrap;
      view.bootstrap_samples = opts_.bootstrap_samples;
      view.bootstrap_seed = opts_.bootstrap_seed;
    }
    auto snap = stats_.snapshot(view);
    result.samples_ns = snap.samples;
    result.samples_recorded = static_cast<int>(snap.samples_recorded);
    result.samples_rejected = static_cast<int>(snap.samples_rejected);
    result.total_iterations = total_iterations_;
    result.iterations_per_sample = iterations_hint_;
    result.warmup_runs = warmup_runs_;
    result.env = env_;
    if (tail_sampler_.has_samples()) {
      result.tail_available = true;
      tail_sampler_.quantiles(result.tail_p95_ns, result.tail_p99_ns);
    }

    if (snap.samples_recorded == 0) {
      return result;
    }

    result.mean_ns = snap.mean;
    result.stddev_ns = snap.stddev;
    result.std_error_ns = snap.std_error;
    result.min_ns = snap.min;
    result.max_ns = snap.max;
    result.median_ns = snap.median;
    result.q1_ns = snap.q1;
    result.q3_ns = snap.q3;
    if (result.mean_ns > 0.0 && result.stddev_ns > 0.0) {
      result.cv_percent = (result.stddev_ns / result.mean_ns) * 100.0;
    }

    result.ci = snap.ci;

    return result;
  }

  void set_iterations_hint(int iterations) { iterations_hint_ = iterations; }

  void set_warmup_runs(int runs) { warmup_runs_ = runs; }

  void set_overhead_cycles(double cycles) { overhead_cycles_ = cycles; }

  std::optional<std::reference_wrapper<tail_sampler>> tail_sampler_handle() {
    if (tail_sampler_.enabled()) {
      return tail_sampler_;
    }
    return std::nullopt;
  }

private:
  bool record_value(double per_iter_ns, int iterations) {
    total_iterations_ += static_cast<std::uint64_t>(iterations);
    return stats_.add(per_iter_ns);
  }

  latency_options opts_;
  cycle_environment env_;
  sample_accumulator stats_;
  std::uint64_t total_iterations_ = 0;
  int iterations_hint_ = 1;
  int warmup_runs_ = 0;
  double overhead_cycles_ = env_.cost_cycles;
  tail_sampler tail_sampler_;
};

class throughput_accumulator {
public:
  throughput_accumulator(throughput_options opts, cycle_environment env) : opts_(std::move(opts)), env_(env) {
    if (opts_.record_windows && opts_.max_windows > 0) {
      batches_.reserve(static_cast<std::size_t>(opts_.max_windows));
    }
  }

  void record_batch_cycles(double cycles, std::uint64_t iterations) {
    if (iterations == 0) {
      return;
    }
    double corrected_cycles = cycles - env_.cost_cycles;
    if (corrected_cycles < 0.0) {
      corrected_cycles = 0.0;
    }
    double elapsed_ns = corrected_cycles * env_.ns_per_cycle;
    total_iterations_ += iterations;
    total_time_ns_ += elapsed_ns;
    const bool unbounded = opts_.max_windows <= 0;
    if (opts_.record_windows && (unbounded || batches_.size() < static_cast<std::size_t>(opts_.max_windows))) {
      const double ops = static_cast<double>(iterations) / (elapsed_ns * 1e-9);
      batches_.push_back(ops);
      min_ops_ = batches_.size() == 1 ? ops : std::min(min_ops_, ops);
      max_ops_ = batches_.size() == 1 ? ops : std::max(max_ops_, ops);
    }
    ++batches_recorded_;
  }

  void add_warmup_batch() { ++warmup_batches_; }

  throughput_result finalize() {
    throughput_result result{};
    result.iterations = total_iterations_;
    result.total_time_ns = total_time_ns_;
    result.batches = batches_recorded_;
    result.warmup_batches = warmup_batches_;
    result.env = env_;

    if (total_time_ns_ > 0.0 && total_iterations_ > 0) {
      result.total_ops_per_sec = static_cast<double>(total_iterations_) / (total_time_ns_ * 1e-9);
    }

    if (opts_.record_windows && !batches_.empty()) {
      result.batch_ops_per_sec = batches_;
      auto sorted = batches_;
      std::sort(sorted.begin(), sorted.end());
      result.median_ops_per_sec = sn::util::bench::quantile(sorted, 0.5);
      result.min_ops_per_sec = sorted.front();
      result.max_ops_per_sec = sorted.back();
      double mean = std::accumulate(batches_.begin(), batches_.end(), 0.0) / static_cast<double>(batches_.size());
      result.mean_ops_per_sec = mean;
      double variance = 0.0;
      for (double v : batches_) {
        const double diff = v - mean;
        variance += diff * diff;
      }
      if (batches_.size() > 1) {
        variance /= static_cast<double>(batches_.size() - 1);
      }
      const double stddev = batches_.size() > 1 ? std::sqrt(std::max(0.0, variance)) : 0.0;
      result.ci = sn::util::bench::student_t_interval(mean, stddev, batches_.size());
    } else {
      result.mean_ops_per_sec = result.total_ops_per_sec;
      result.median_ops_per_sec = result.total_ops_per_sec;
      result.min_ops_per_sec = result.total_ops_per_sec;
      result.max_ops_per_sec = result.total_ops_per_sec;
      result.ci.low = result.total_ops_per_sec;
      result.ci.high = result.total_ops_per_sec;
      result.ci.method = ci_method::none;
    }

    return result;
  }

private:
  throughput_options opts_;
  cycle_environment env_;
  std::vector<double> batches_;
  std::uint64_t total_iterations_ = 0;
  double total_time_ns_ = 0.0;
  double min_ops_ = 0.0;
  double max_ops_ = 0.0;
  int batches_recorded_ = 0;
  int warmup_batches_ = 0;
};

class sample_guard {
public:
  sample_guard(latency_accumulator& acc, int iterations) noexcept : acc_(&acc), iterations_(iterations) {
    compiler_barrier();
    start_ = sn::util::cpu_timer::now();
  }

  sample_guard(sample_guard&& other) noexcept : acc_(other.acc_), iterations_(other.iterations_), start_(other.start_) {
    other.acc_ = nullptr;
    other.iterations_ = 0;
  }

  sample_guard(const sample_guard&) = delete;
  sample_guard& operator=(const sample_guard&) = delete;

  ~sample_guard() {
    if (!acc_) {
      return;
    }
    compiler_barrier();
    const auto end = sn::util::cpu_timer::now();
    compiler_barrier();
    const double cycles = static_cast<double>(end - start_);
    acc_->record_cycles(cycles, iterations_);
  }

private:
  latency_accumulator* acc_ = nullptr;
  int iterations_ = 0;
  sn::util::cpu_timer::counter_type start_ = 0;
};

class batch_guard {
public:
  batch_guard(throughput_accumulator& acc, std::uint64_t iterations) noexcept : acc_(&acc), iterations_(iterations) {
    compiler_barrier();
    start_ = sn::util::cpu_timer::now();
  }

  batch_guard(batch_guard&& other) noexcept : acc_(other.acc_), iterations_(other.iterations_), start_(other.start_) {
    other.acc_ = nullptr;
    other.iterations_ = 0;
  }

  batch_guard(const batch_guard&) = delete;
  batch_guard& operator=(const batch_guard&) = delete;

  ~batch_guard() {
    if (!acc_) {
      return;
    }
    compiler_barrier();
    const auto end = sn::util::cpu_timer::now();
    compiler_barrier();
    const double cycles = static_cast<double>(end - start_);
    acc_->record_batch_cycles(cycles, iterations_);
  }

private:
  throughput_accumulator* acc_ = nullptr;
  std::uint64_t iterations_ = 0;
  sn::util::cpu_timer::counter_type start_ = 0;
};

}

class latency_session {
public:
  explicit latency_session(latency_options opts = {}) :
      opts_(std::move(opts)), env_(initialize_environment(opts_)), accumulator_(opts_, env_) {}

  detail::sample_guard start_sample(int iterations) { return detail::sample_guard(accumulator_, iterations); }

  void submit_sample(double elapsed_ns, int iterations, bool already_per_iteration = false) {
    accumulator_.record_ns(elapsed_ns, iterations, already_per_iteration);
  }

  bool should_continue() const { return accumulator_.should_continue(); }

  void set_iterations_per_sample(int iterations) { accumulator_.set_iterations_hint(iterations); }

  void set_warmup_runs(int runs) { accumulator_.set_warmup_runs(runs); }

  void set_overhead_cycles(double cycles) { accumulator_.set_overhead_cycles(cycles); }

  std::optional<std::reference_wrapper<detail::tail_sampler>> tail_sampler() {
    return accumulator_.tail_sampler_handle();
  }

  latency_result finalize() { return accumulator_.finalize(); }

  const cycle_environment& environment() const noexcept { return env_; }

private:
  static cycle_environment initialize_environment(const latency_options& opts) {
    detail::maybe_pin_thread(detail::pin_options{opts.pin_thread, opts.pin_core, opts.verbose});
    return detail::measure_cycle_environment();
  }

  latency_options opts_;
  cycle_environment env_;
  detail::latency_accumulator accumulator_;
};

class throughput_session {
public:
  explicit throughput_session(throughput_options opts = {}) :
      opts_(std::move(opts)), env_(initialize_environment(opts_)), accumulator_(opts_, env_) {}

  detail::batch_guard start_batch(std::uint64_t iterations) { return detail::batch_guard(accumulator_, iterations); }

  void submit_batch(double elapsed_ns, std::uint64_t iterations) {
    const double cycles = elapsed_ns / env_.ns_per_cycle;
    accumulator_.record_batch_cycles(cycles, iterations);
  }

  void add_warmup_batch() { accumulator_.add_warmup_batch(); }

  throughput_result finalize() { return accumulator_.finalize(); }

  const cycle_environment& environment() const noexcept { return env_; }

private:
  static cycle_environment initialize_environment(const throughput_options& opts) {
    detail::maybe_pin_thread(detail::pin_options{opts.pin_thread, opts.pin_core, opts.verbose});
    return detail::measure_cycle_environment();
  }

  throughput_options opts_;
  cycle_environment env_;
  detail::throughput_accumulator accumulator_;
};

namespace detail {

template <typename Callable>
sample_capture calibrate_sample(Callable& callable, int iterations, const cycle_environment& env) {
  return measure_callable(callable, iterations, env, measure_empty_loop(iterations));
}

template <typename Callable>
void invoke_for_iterations(
    Callable& callable, int iterations, std::optional<std::reference_wrapper<tail_sampler>> tail = std::nullopt
) {
  invocation_helper<Callable>::invoke(callable, iterations, tail);
}

template <typename Callable>
void invoke_throughput_batch(Callable& callable, std::uint64_t iterations, std::uint64_t batch_index) {
  if constexpr (std::is_invocable_v<Callable&, std::uint64_t, std::uint64_t>) {
    std::invoke(callable, iterations, batch_index);
  } else if constexpr (std::is_invocable_v<Callable&, std::uint64_t>) {
    std::invoke(callable, iterations);
  } else {
    for (std::uint64_t i = 0; i < iterations; ++i) {
      invocation_helper<Callable>::invoke(callable, 1);
    }
  }
}

template <typename Callable>
std::uint64_t calibrate_batch_size(
    const throughput_options& opts, std::uint64_t iterations, const cycle_environment& env, Callable& func,
    bool& probe_executed
) {
  probe_executed = false;
  if (iterations == 0) {
    return 0;
  }
  if (opts.batch_size > 0) {
    return std::min(opts.batch_size, iterations);
  }
  std::uint64_t candidate = std::max<std::uint64_t>(1, iterations / 32);
  candidate = std::min(candidate, iterations);

  const double target_ns = std::max({env.resolution_ns() * 1000.0, env.cost_ns() * 50.0, 2.0e5});

  compiler_barrier();
  const auto start = sn::util::cpu_timer::now();
  invoke_throughput_batch(func, candidate, 0);
  compiler_barrier();
  const auto end = sn::util::cpu_timer::now();
  probe_executed = true;

  const auto delta_cycles = end - start;
  if (delta_cycles <= 0) {
    return candidate;
  }

  double corrected_cycles = static_cast<double>(delta_cycles) - env.cost_cycles;
  if (corrected_cycles < 0.0) {
    corrected_cycles = 0.0;
  }
  const double observed_ns = corrected_cycles * env.ns_per_cycle;
  if (!(observed_ns > 0.0)) {
    return candidate;
  }

  double scale = target_ns / observed_ns;
  if (!sn::util::math::is_finite(scale) || scale <= 0.0) {
    scale = 1.0;
  }
  const double adjusted = std::clamp(scale * static_cast<double>(candidate), 1.0, static_cast<double>(iterations));
  return static_cast<std::uint64_t>(adjusted);
}

}

template <typename Callable> latency_result measure_latency(Callable&& callable, latency_options opts = {}) {
  Callable func = std::forward<Callable>(callable);
  latency_session session(opts);
  const auto& env = session.environment();

  int iterations = std::max(opts.min_iterations, 1);
  const double resolution_target = env.resolution_ns() * 1000.0;
  const double user_target = std::max(opts.min_sample_ns, env.cost_ns() * 50.0);
  const double target_ns = std::max(resolution_target, user_target);
  double baseline_ns = 0.0;

  while (true) {
    auto sample = detail::calibrate_sample(func, iterations, env);
    if (!sample.recorded) {
      break;
    }
    baseline_ns = sample.per_iteration_ns * static_cast<double>(iterations);
    if (baseline_ns >= target_ns || iterations >= opts.max_iterations) {
      break;
    }
    if (iterations > opts.max_iterations / 2) {
      iterations = opts.max_iterations;
    } else {
      iterations *= 2;
    }
  }

  iterations = std::clamp(iterations, opts.min_iterations, opts.max_iterations);
  session.set_iterations_per_sample(iterations);
  session.set_overhead_cycles(detail::measure_empty_loop(iterations));

  if (opts.warmup_budget_ns > 0.0 && baseline_ns > 0.0) {
    int warmup_runs = static_cast<int>(opts.warmup_budget_ns / baseline_ns);
    warmup_runs = std::clamp(warmup_runs, 0, opts.max_samples);
    session.set_warmup_runs(warmup_runs);
    for (int i = 0; i < warmup_runs; ++i) {
      detail::invoke_for_iterations(func, iterations);
    }
  }

  auto tail_ref = session.tail_sampler();
  while (session.should_continue()) {
    auto guard = session.start_sample(iterations);
    detail::invoke_for_iterations(func, iterations, tail_ref);
    (void) guard;
  }

  return session.finalize();
}

template <typename Callable>
throughput_result measure_throughput(std::uint64_t iterations, Callable&& callable, throughput_options opts = {}) {
  Callable func = std::forward<Callable>(callable);
  throughput_session session(opts);
  auto& env = session.environment();

  if (iterations == 0) {
    return session.finalize();
  }

  std::uint64_t remaining = iterations;
  bool probe_executed = false;
  std::uint64_t batch_size = detail::calibrate_batch_size(opts, iterations, env, func, probe_executed);
  if (batch_size == 0) {
    batch_size = 1;
  }
  if (probe_executed) {
    session.add_warmup_batch();
  }

  auto measure_batch_ns = [&](std::uint64_t iters, std::uint64_t index) {
    detail::compiler_barrier();
    const auto start = sn::util::cpu_timer::now();
    detail::invoke_throughput_batch(func, iters, index);
    detail::compiler_barrier();
    const auto end = sn::util::cpu_timer::now();
    double corrected_cycles = static_cast<double>(end - start) - env.cost_cycles;
    if (corrected_cycles < 0.0) {
      corrected_cycles = 0.0;
    }
    return corrected_cycles * env.ns_per_cycle;
  };

  std::uint64_t batch_index = probe_executed ? 1ULL : 0ULL;

  if (opts.warmup_budget_ns > 0.0) {
    const std::uint64_t warmup_iters = std::min(remaining, batch_size);
    const double first_ns = measure_batch_ns(warmup_iters, batch_index);
    session.add_warmup_batch();
    ++batch_index;
    if (first_ns > 0.0) {
      const int warmup_batches = static_cast<int>(opts.warmup_budget_ns / first_ns);
      for (int i = 1; i < warmup_batches; ++i) {
        (void) measure_batch_ns(warmup_iters, batch_index++);
        session.add_warmup_batch();
      }
    }
  }

  while (remaining > 0) {
    const std::uint64_t this_batch = std::min(batch_size, remaining);
    auto guard = session.start_batch(this_batch);
    detail::invoke_throughput_batch(func, this_batch, batch_index);
    (void) guard;
    remaining -= this_batch;
    ++batch_index;
  }

  auto result = session.finalize();
  result.batch_size = batch_size;
  return result;
}

template <typename Callable> throughput_result measure_throughput(Callable&& callable, throughput_options opts) {
  const std::uint64_t iterations = opts.iteration_count;
  return measure_throughput(iterations, std::forward<Callable>(callable), std::move(opts));
}

enum class format_style { compact, pretty };

struct format_options {
  int duration_precision = 2;
  bool show_samples = true;
  bool show_environment = false;
  format_style style = format_style::compact;
};

inline int clamp_precision(int precision) noexcept { return std::clamp(precision, 0, 9); }

inline std::string format_fixed(double value, int precision) {
  if (!sn::util::math::is_finite(value)) {
    return "nan";
  }
  char buffer[64];
  const int clamped = clamp_precision(precision);
  const int count = std::snprintf(buffer, sizeof(buffer), "%.*f", clamped, value);
  if (count < 0) {
    return "nan";
  }
  if (static_cast<std::size_t>(count) >= sizeof(buffer)) {
    std::vector<char> dynamic(static_cast<std::size_t>(count) + 1);
    std::snprintf(dynamic.data(), dynamic.size(), "%.*f", clamped, value);
    return std::string(dynamic.data());
  }
  return std::string(buffer, static_cast<std::size_t>(count));
}

inline std::string format_duration(double ns, int precision) {
  double value = ns;
  const double abs_ns = std::abs(ns);
  const char* unit = "ns";
  if (abs_ns >= 1.0e9) {
    value = ns / 1.0e9;
    unit = "s";
  } else if (abs_ns >= 1.0e6) {
    value = ns / 1.0e6;
    unit = "ms";
  } else if (abs_ns >= 1.0e3) {
    value = ns / 1.0e3;
    unit = "us";
  }
  std::string text = format_fixed(value, precision);
  text.push_back(' ');
  text.append(unit);
  return text;
}

inline std::string format_ci(const confidence_interval& ci, int precision) {
  if (ci.method == ci_method::none) {
    return "[n/a]";
  }
  std::string text;
  text.reserve(64);
  text.push_back('[');
  text.append(format_duration(ci.low, precision));
  text.append(", ");
  text.append(format_duration(ci.high, precision));
  text.push_back(']');
  return text;
}

inline std::string environment_details(const cycle_environment& env, const format_options& opts) {
  if (!opts.show_environment) {
    return {};
  }
  std::string text;
  text.reserve(64);
  text.append("res=");
  text.append(format_duration(env.resolution_ns(), opts.duration_precision));
  text.append(", overhead=");
  text.append(format_duration(env.cost_ns(), opts.duration_precision));
  if (!env.clock_stable) {
    text.append(", clock=unstable");
  }
  return text;
}

inline std::string format_latency_compact(const latency_result& result, const format_options& opts) {
  std::string text;
  text.reserve(256);
  text.append("latency mean=");
  text.append(format_duration(result.mean_ns, opts.duration_precision));
  text.append(" median=");
  text.append(format_duration(result.median_ns, opts.duration_precision));
  text.append(" stddev=");
  text.append(format_duration(result.stddev_ns, opts.duration_precision));
  text.append(" ci95=");
  text.append(format_ci(result.ci, opts.duration_precision));
  text.append(" cv=");
  text.append(format_fixed(result.cv_percent, 2));
  text.push_back('%');
  if (opts.show_samples) {
    text.append(" samples=");
    text.append(std::to_string(result.samples_recorded));
    text.push_back('/');
    text.append(std::to_string(result.samples_recorded + result.samples_rejected));
    text.append(" iter/sample=");
    text.append(std::to_string(result.iterations_per_sample));
  }
  if (result.tail_available) {
    text.append(" p95=");
    text.append(format_duration(result.tail_p95_ns, opts.duration_precision));
    text.append(" p99=");
    text.append(format_duration(result.tail_p99_ns, opts.duration_precision));
  }
  const auto env_info = environment_details(result.env, opts);
  if (!env_info.empty()) {
    text.append(" env(");
    text.append(env_info);
    text.push_back(')');
  }
  return text;
}

inline void append_row(std::string& out, std::string_view label, std::string value) {
  out.append("  ");
  out.append(label);
  if (label.size() < 18) {
    out.append(18 - label.size(), ' ');
  } else {
    out.push_back(' ');
  }
  out.append(value);
  out.push_back('\n');
}

inline std::string format_latency_pretty(const latency_result& result, const format_options& opts) {
  std::string text;
  text.reserve(512);
  text.append("latency results\n");

  if (result.samples_recorded == 0) {
    append_row(text, "note", "no timing samples recorded");
  } else {
    append_row(text, "mean", format_duration(result.mean_ns, opts.duration_precision));
    append_row(text, "median", format_duration(result.median_ns, opts.duration_precision));
    append_row(text, "std dev", format_duration(result.stddev_ns, opts.duration_precision));
    append_row(text, "cv", format_fixed(result.cv_percent, 2) + "%");
    append_row(text, "95% CI", format_ci(result.ci, opts.duration_precision));
    append_row(
        text, "q1/q3",
        format_duration(result.q1_ns, opts.duration_precision) + " / " +
            format_duration(result.q3_ns, opts.duration_precision)
    );
    append_row(
        text, "min/max",
        format_duration(result.min_ns, opts.duration_precision) + " / " +
            format_duration(result.max_ns, opts.duration_precision)
    );
  }

  if (opts.show_samples) {
    append_row(
        text, "samples",
        std::to_string(result.samples_recorded) + " recorded, " + std::to_string(result.samples_rejected) + " rejected"
    );
    append_row(text, "iter/sample", std::to_string(result.iterations_per_sample));
    if (result.warmup_runs > 0) {
      append_row(text, "warmup runs", std::to_string(result.warmup_runs));
    }
  }

  if (result.tail_available) {
    append_row(text, "p95", format_duration(result.tail_p95_ns, opts.duration_precision));
    append_row(text, "p99", format_duration(result.tail_p99_ns, opts.duration_precision));
  }

  const auto env_info = environment_details(result.env, opts);
  if (!env_info.empty()) {
    append_row(text, "environment", env_info);
  }

  return text;
}

inline std::string format_ops(double value) {
  std::string text = format_fixed(value, 2);
  text.append(" op/s");
  return text;
}

inline std::string format_throughput_compact(const throughput_result& result, const format_options& opts) {
  std::string text;
  text.reserve(256);
  text.append("throughput total=");
  text.append(format_ops(result.total_ops_per_sec));
  text.append(" mean=");
  text.append(format_ops(result.mean_ops_per_sec));
  text.append(" median=");
  text.append(format_ops(result.median_ops_per_sec));
  text.append(" ci95=");
  text.append(format_fixed(result.ci.low, 2));
  text.push_back('-');
  text.append(format_fixed(result.ci.high, 2));
  text.append(" op/s");
  if (opts.show_samples) {
    text.append(" batches=");
    text.append(std::to_string(result.batches));
    if (result.warmup_batches > 0) {
      text.append(" warmup=");
      text.append(std::to_string(result.warmup_batches));
    }
  }
  const auto env_info = environment_details(result.env, opts);
  if (!env_info.empty()) {
    text.append(" env(");
    text.append(env_info);
    text.push_back(')');
  }
  return text;
}

inline std::string format_throughput_pretty(const throughput_result& result, const format_options& opts) {
  std::string text;
  text.reserve(256);
  text.append("throughput results\n");

  append_row(text, "total", format_ops(result.total_ops_per_sec));
  append_row(text, "mean", format_ops(result.mean_ops_per_sec));
  append_row(text, "median", format_ops(result.median_ops_per_sec));
  append_row(text, "min/max", format_ops(result.min_ops_per_sec) + " / " + format_ops(result.max_ops_per_sec));
  if (result.ci.method != ci_method::none) {
    append_row(text, "95% CI", format_fixed(result.ci.low, 2) + " - " + format_fixed(result.ci.high, 2) + " op/s");
  }

  if (opts.show_samples) {
    append_row(text, "batches", std::to_string(result.batches));
    if (result.warmup_batches > 0) {
      append_row(text, "warmup batches", std::to_string(result.warmup_batches));
    }
  }

  const auto env_info = environment_details(result.env, opts);
  if (!env_info.empty()) {
    append_row(text, "environment", env_info);
  }

  return text;
}

inline std::string format(const latency_result& result, const format_options& opts = {}) {
  if (opts.style == format_style::pretty) {
    return format_latency_pretty(result, opts);
  }
  return format_latency_compact(result, opts);
}

inline std::string format(const throughput_result& result, const format_options& opts = {}) {
  if (opts.style == format_style::pretty) {
    return format_throughput_pretty(result, opts);
  }
  return format_throughput_compact(result, opts);
}

}
