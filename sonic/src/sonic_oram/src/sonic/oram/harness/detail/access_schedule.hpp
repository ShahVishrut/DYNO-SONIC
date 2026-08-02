#pragma once

#include <cstddef>
#include <cstdint>
#include <numeric>

#include "sonic/oram/harness/config.hpp"

namespace sn::oram::harness::detail {

// harness coordinate for schedule
struct access_coord {
  std::uint64_t global_index = 0;
  std::uint64_t window_index = 0;
  std::size_t slot_in_window = 0;
  std::size_t worker_index = 0;
};

// one address every time
struct fixed_schedule {
  std::uint64_t address = 0;
};

// one address per worker
struct per_worker_fixed_schedule {
  std::uint64_t block_count = 0;
  std::uint64_t start = 0;
};

// walk addresses in unit stride
struct round_robin_schedule {
  std::uint64_t block_count = 0;
  std::uint64_t start = 0;
  schedule_scope scope = schedule_scope::stream;
};

// walk addresses with coprime stride
struct affine_schedule {
  std::uint64_t block_count = 0;
  std::uint64_t start = 0;
  std::uint64_t step = 1;
  schedule_scope scope = schedule_scope::stream;
};

// advance across stream or restart per window
[[nodiscard]] inline std::uint64_t scope_index(schedule_scope scope, const access_coord& coord) noexcept {
  return scope == schedule_scope::window ? static_cast<std::uint64_t>(coord.slot_in_window) : coord.global_index;
}

[[nodiscard]] inline std::uint64_t normalize_mod(std::uint64_t value, std::uint64_t mod) noexcept {
  return mod == 0 ? 0 : (value % mod);
}

// disjoint runs need a full cycle, so move to the next coprime stride
[[nodiscard]] inline std::uint64_t choose_coprime_step(std::uint64_t modulus, std::uint64_t preferred) noexcept {
  if (modulus <= 1) {
    return 1;
  }
  std::uint64_t candidate = preferred == 0 ? 1 : (preferred % modulus);
  if (candidate == 0) {
    candidate = 1;
  }
  while (std::gcd(candidate, modulus) != 1) {
    ++candidate;
    if (candidate >= modulus) {
      candidate = 1;
    }
  }
  return candidate;
}

[[nodiscard]] inline std::uint64_t address_of(const fixed_schedule& schedule, access_coord) noexcept {
  return schedule.address;
}

[[nodiscard]] inline std::uint64_t address_of(const per_worker_fixed_schedule& schedule, access_coord coord) noexcept {
  if (schedule.block_count == 0) {
    return 0;
  }
  return (schedule.start + static_cast<std::uint64_t>(coord.worker_index)) % schedule.block_count;
}

[[nodiscard]] inline std::uint64_t address_of(const round_robin_schedule& schedule, access_coord coord) noexcept {
  if (schedule.block_count == 0) {
    return 0;
  }
  const auto index = scope_index(schedule.scope, coord);
  return (schedule.start + index) % schedule.block_count;
}

// use the stride for a fast permutation of address space
[[nodiscard]] inline std::uint64_t address_of(const affine_schedule& schedule, access_coord coord) noexcept {
  if (schedule.block_count == 0) {
    return 0;
  }
  const auto index = scope_index(schedule.scope, coord);
  const auto offset = (schedule.step * normalize_mod(index, schedule.block_count)) % schedule.block_count;
  return (schedule.start + offset) % schedule.block_count;
}

[[nodiscard]] inline std::uint64_t unique_period(const fixed_schedule&) noexcept { return 1; }

[[nodiscard]] inline std::uint64_t unique_period(const per_worker_fixed_schedule&) noexcept { return 1; }

[[nodiscard]] inline std::uint64_t unique_period(const round_robin_schedule& schedule) noexcept {
  return schedule.block_count == 0 ? 1 : schedule.block_count;
}

// affine is full period when the stride is coprime with modulus
[[nodiscard]] inline std::uint64_t unique_period(const affine_schedule& schedule) noexcept {
  if (schedule.block_count == 0) {
    return 1;
  }
  return std::gcd(schedule.block_count, schedule.step) == 1
             ? schedule.block_count
             : schedule.block_count / std::gcd(schedule.block_count, schedule.step);
}

} // namespace sn::oram::harness::detail
