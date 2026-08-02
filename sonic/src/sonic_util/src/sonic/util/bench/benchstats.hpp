#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace sn::util::bench {

enum class ci_method { none, student_t, bootstrap };

struct confidence_interval {
  double low = 0.0;
  double high = 0.0;
  ci_method method = ci_method::none;
};

inline double quantile(const std::vector<double>& sorted_values, double probability) {
  if (sorted_values.empty()) {
    return 0.0;
  }
  const double clamped = std::clamp(probability, 0.0, 1.0);
  const double index = clamped * static_cast<double>(sorted_values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(index));
  const auto upper = static_cast<std::size_t>(std::ceil(index));
  const double fraction = index - static_cast<double>(lower);
  if (upper >= sorted_values.size()) {
    return sorted_values.back();
  }
  return sorted_values[lower] + (sorted_values[upper] - sorted_values[lower]) * fraction;
}

inline double median_abs_deviation(const std::vector<double>& values, double median) {
  if (values.empty()) {
    return 0.0;
  }
  std::vector<double> deltas(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    deltas[i] = std::abs(values[i] - median);
  }
  std::nth_element(deltas.begin(), deltas.begin() + deltas.size() / 2, deltas.end());
  return deltas[deltas.size() / 2];
}

inline double t_critical_95(int degrees_of_freedom) noexcept {
  if (degrees_of_freedom <= 0) {
    return 0.0;
  }
  static constexpr double table[] = {0.0,   12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                                     2.201, 2.179,  2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080,
                                     2.074, 2.069,  2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
  if (degrees_of_freedom < static_cast<int>(sizeof(table) / sizeof(table[0]))) {
    return table[degrees_of_freedom];
  }
  return 1.96;
}

inline confidence_interval student_t_interval(double mean, double stddev, std::size_t samples) {
  confidence_interval ci{};
  ci.method = ci_method::student_t;
  if (samples < 2 || stddev <= 0.0) {
    ci.low = mean;
    ci.high = mean;
    return ci;
  }
  const double standard_error = stddev / std::sqrt(static_cast<double>(samples));
  const double crit = t_critical_95(static_cast<int>(samples) - 1);
  const double delta = crit * standard_error;
  ci.low = std::max(0.0, mean - delta);
  ci.high = mean + delta;
  return ci;
}

inline confidence_interval bootstrap_interval(
    const std::vector<double>& samples, unsigned bootstrap_samples, std::uint64_t seed
) {
  confidence_interval ci{};
  ci.method = ci_method::bootstrap;
  if (samples.size() < 2 || bootstrap_samples == 0) {
    double mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                  static_cast<double>(std::max<std::size_t>(samples.size(), 1));
    ci.low = mean;
    ci.high = mean;
    return ci;
  }
  if (seed == 0) {
    std::random_device rd;
    seed = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    if (seed == 0) {
      seed = 0x7f4a'9e39'3d5b'2b01ull;
    }
  }
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> pick(0, samples.size() - 1);
  std::vector<double> estimates;
  estimates.reserve(bootstrap_samples);
  for (unsigned i = 0; i < bootstrap_samples; ++i) {
    double sum = 0.0;
    for (std::size_t j = 0; j < samples.size(); ++j) {
      sum += samples[pick(rng)];
    }
    estimates.push_back(sum / static_cast<double>(samples.size()));
  }
  const auto low_index = static_cast<std::size_t>(std::floor(0.025 * static_cast<double>(estimates.size())));
  const auto high_index = static_cast<std::size_t>(std::floor(0.975 * static_cast<double>(estimates.size())));
  std::nth_element(estimates.begin(), estimates.begin() + low_index, estimates.end());
  const double low = estimates[low_index];
  std::nth_element(estimates.begin(), estimates.begin() + high_index, estimates.end());
  const double high = estimates[high_index];
  ci.low = std::min(low, high);
  ci.high = std::max(low, high);
  return ci;
}

struct stats_view_options {
  std::size_t drop_first = 0;
  double drop_fraction = 0.0;
  ci_method ci = ci_method::student_t;
  unsigned bootstrap_samples = 0;
  std::uint64_t bootstrap_seed = 0;
};

struct stat_accumulator_options {
  std::size_t warmup_samples = 0;
  bool reject_outliers = true;
  double outlier_mad_k = 4.0;
  bool keep_samples = true;
  std::size_t max_samples = 0;
  std::uint64_t reservoir_seed = 0;
};

struct stats_snapshot {
  std::vector<double> samples;
  double mean = 0.0;
  double median = 0.0;
  double q1 = 0.0;
  double q3 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double min = 0.0;
  double max = 0.0;
  double stddev = 0.0;
  double std_error = 0.0;
  double cv_percent = 0.0;
  confidence_interval ci{};
  std::size_t samples_recorded = 0;
  std::size_t samples_rejected = 0;
  std::size_t warmup_dropped = 0;
};

class sample_accumulator {
public:
  explicit sample_accumulator(stat_accumulator_options opts = {});

  bool add(double value);

  stats_snapshot snapshot(const stats_view_options& view = {}) const;

  std::size_t recorded() const noexcept { return recorded_; }
  std::size_t rejected() const noexcept { return rejected_; }
  std::size_t warmup_skipped() const noexcept { return warmup_skipped_; }

  double mean() const noexcept { return recorded_ > 0 ? mean_ : 0.0; }
  double sample_variance() const noexcept { return recorded_ > 1 ? m2_ / static_cast<double>(recorded_ - 1) : 0.0; }
  double min() const noexcept { return recorded_ > 0 ? min_ : 0.0; }
  double max() const noexcept { return recorded_ > 0 ? max_ : 0.0; }
  bool empty() const noexcept { return recorded_ == 0; }

private:
  std::uint64_t next_random() const;
  bool should_store() const;

  stat_accumulator_options opts_{};
  mutable std::uint64_t rng_state_{0x9e3779b97f4a7c15ull};
  std::vector<double> samples_;
  std::size_t seen_ = 0;
  std::size_t recorded_ = 0;
  std::size_t rejected_ = 0;
  std::size_t warmup_skipped_ = 0;
  double mean_ = 0.0;
  double m2_ = 0.0;
  double min_ = 0.0;
  double max_ = 0.0;
};

inline sample_accumulator::sample_accumulator(stat_accumulator_options opts) : opts_(opts) {
  if (opts_.max_samples == 0) {
    samples_.reserve(256);
  } else {
    samples_.reserve(opts_.max_samples);
  }
  if (opts_.reservoir_seed != 0) {
    rng_state_ = opts_.reservoir_seed;
  }
}

inline std::uint64_t sample_accumulator::next_random() const {
  rng_state_ = rng_state_ * 6364136223846793005ull + 1ull;
  return rng_state_;
}

inline bool sample_accumulator::should_store() const {
  if (!opts_.keep_samples) {
    return false;
  }
  if (opts_.max_samples == 0 || samples_.size() < opts_.max_samples) {
    return true;
  }
  const std::uint64_t idx = next_random() % recorded_;
  return idx < opts_.max_samples;
}

inline bool sample_accumulator::add(double value) {
  ++seen_;
  if (!std::isfinite(value) || value < 0.0) {
    ++rejected_;
    return false;
  }
  if (warmup_skipped_ < opts_.warmup_samples) {
    ++warmup_skipped_;
    return false;
  }

  const bool can_reject = opts_.reject_outliers && opts_.keep_samples && recorded_ >= 5 && !samples_.empty();
  if (can_reject) {
    auto scratch = samples_;
    std::nth_element(scratch.begin(), scratch.begin() + scratch.size() / 2, scratch.end());
    const double median = scratch[scratch.size() / 2];
    const double mad = 1.4826 * median_abs_deviation(samples_, median);
    if (mad > 0.0) {
      const double deviation = std::abs(value - median);
      if (deviation > opts_.outlier_mad_k * mad) {
        ++rejected_;
        return false;
      }
    }
  }

  ++recorded_;
  if (recorded_ == 1) {
    mean_ = value;
    min_ = value;
    max_ = value;
  } else {
    const double delta = value - mean_;
    mean_ += delta / static_cast<double>(recorded_);
    const double delta2 = value - mean_;
    m2_ += delta * delta2;
    if (value < min_) {
      min_ = value;
    }
    if (value > max_) {
      max_ = value;
    }
  }

  if (opts_.keep_samples) {
    if (opts_.max_samples == 0 || samples_.size() < opts_.max_samples) {
      samples_.push_back(value);
    } else if (should_store()) {
      const std::size_t idx = static_cast<std::size_t>(next_random() % opts_.max_samples);
      samples_[idx] = value;
    }
  }

  return true;
}

inline stats_snapshot sample_accumulator::snapshot(const stats_view_options& view) const {
  stats_snapshot snap{};
  snap.samples_rejected = rejected_;
  snap.warmup_dropped = warmup_skipped_;

  std::vector<double> working = samples_;
  if (view.drop_first > 0 && working.size() > view.drop_first) {
    working.erase(working.begin(), working.begin() + static_cast<std::ptrdiff_t>(view.drop_first));
    snap.warmup_dropped += view.drop_first;
  } else if (view.drop_first > 0 && working.size() <= view.drop_first) {
    snap.warmup_dropped += working.size();
    working.clear();
  }

  if (view.drop_fraction > 0.0 && !working.empty()) {
    const std::size_t extra = std::min<std::size_t>(
        working.size(), static_cast<std::size_t>(std::floor(working.size() * view.drop_fraction))
    );
    snap.warmup_dropped += extra;
    if (extra > 0) {
      working.erase(working.begin(), working.begin() + static_cast<std::ptrdiff_t>(extra));
    }
  }

  if (working.empty()) {
    snap.samples_recorded = recorded_;
    snap.mean = mean_;
    snap.min = min_;
    snap.max = max_;
    if (recorded_ > 1) {
      snap.stddev = std::sqrt(std::max(0.0, sample_variance()));
      snap.std_error = snap.stddev / std::sqrt(static_cast<double>(recorded_));
    }
    if (view.ci == ci_method::student_t) {
      snap.ci = student_t_interval(snap.mean, snap.stddev, snap.samples_recorded);
    } else if (view.ci == ci_method::bootstrap && view.bootstrap_samples > 0) {
      snap.ci = bootstrap_interval(samples_, view.bootstrap_samples, view.bootstrap_seed);
    }
    return snap;
  }

  snap.samples = working;
  snap.samples_recorded = working.size();

  double sum = 0.0;
  snap.min = working[0];
  snap.max = working[0];
  for (double v : working) {
    sum += v;
    if (v < snap.min) {
      snap.min = v;
    }
    if (v > snap.max) {
      snap.max = v;
    }
  }
  snap.mean = sum / static_cast<double>(working.size());

  double variance = 0.0;
  if (working.size() > 1) {
    for (double v : working) {
      const double diff = v - snap.mean;
      variance += diff * diff;
    }
    variance /= static_cast<double>(working.size() - 1);
    snap.stddev = std::sqrt(std::max(0.0, variance));
    snap.std_error = snap.stddev / std::sqrt(static_cast<double>(working.size()));
  }

  auto sorted = working;
  std::sort(sorted.begin(), sorted.end());
  snap.median = quantile(sorted, 0.5);
  snap.q1 = quantile(sorted, 0.25);
  snap.q3 = quantile(sorted, 0.75);
  snap.p95 = quantile(sorted, 0.95);
  snap.p99 = quantile(sorted, 0.99);
  if (snap.mean > 0.0 && snap.stddev > 0.0) {
    snap.cv_percent = (snap.stddev / snap.mean) * 100.0;
  }

  if (view.ci == ci_method::student_t) {
    snap.ci = student_t_interval(snap.mean, snap.stddev, snap.samples_recorded);
  } else if (view.ci == ci_method::bootstrap && view.bootstrap_samples > 0) {
    snap.ci = bootstrap_interval(sorted, view.bootstrap_samples, view.bootstrap_seed);
  }

  return snap;
}

}
