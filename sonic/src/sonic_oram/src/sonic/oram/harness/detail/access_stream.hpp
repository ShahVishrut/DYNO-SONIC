#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/oram/harness/detail/access_schedule.hpp"
#include "sonic/oram/harness/detail/client_access.hpp"
#include "sonic/oram/harness/detail/random.hpp"
#include "sonic/oram/harness/detail/workers.hpp"

namespace sn::oram::harness::detail {

template <std::size_t BlockBytes, typename Client> struct stream_worker_state {
  sn::crypto::buffered_prng<> rng{};
  std::array<std::uint8_t, BlockBytes> in_buf{};
  std::array<std::uint8_t, BlockBytes> out_buf{};
  access_scratch_type<Client> scratch{};
};

// initialize state for each worker
template <std::size_t BlockBytes, typename Client>
[[nodiscard]] inline std::vector<stream_worker_state<BlockBytes, Client>> make_stream_worker_states(
    Client& client, std::size_t concurrency, detail::seed_stream& seeds
) {
  std::vector<stream_worker_state<BlockBytes, Client>> states(concurrency);
  for (std::size_t i = 0; i < concurrency; ++i) {
    states[i].rng = detail::make_prng(seeds);
    configure_access_scratch(client, states[i].scratch);
  }
  return states;
}

// limit stream workers by window size
[[nodiscard]] inline std::size_t stream_worker_count(
    std::size_t requested_concurrency, std::size_t access_count, std::size_t window_size = 0
) noexcept {
  const std::size_t live_limit = window_size == 0 ? access_count : std::min<std::size_t>(access_count, window_size);
  return std::max<std::size_t>(1, std::min(requested_concurrency, std::max<std::size_t>(1, live_limit)));
}

template <typename Fn>
inline void for_each_stream_index(
    std::optional<sn::threads::thread_team>& workers, std::size_t worker_count, std::size_t access_count, Fn&& fn
) {
  std::atomic<std::size_t> issued{0};
  auto worker_task = [&](std::size_t worker_index) noexcept {
    while (true) {
      const std::size_t global_index = issued.fetch_add(1, std::memory_order_relaxed);
      if (global_index >= access_count) {
        break;
      }
      fn(worker_index, global_index);
    }
  };

  for_each_active_worker(workers, worker_count, [&](std::size_t worker_index) noexcept { worker_task(worker_index); });
}

[[nodiscard]] inline access_coord make_access_coord(
    std::uint64_t global_base, std::size_t local_index, std::uint64_t window_index, std::size_t slot_in_window,
    std::size_t worker_index
) noexcept {
  return access_coord{
      global_base + static_cast<std::uint64_t>(local_index), window_index, slot_in_window, worker_index
  };
}

template <typename AccessFn>
inline void run_plain_access_span(
    std::optional<sn::threads::thread_team>& workers, std::size_t worker_count, std::uint64_t global_base,
    std::size_t access_count, std::uint64_t window_base, AccessFn&& access_fn
) {
  for_each_stream_index(workers, worker_count, access_count, [&](std::size_t worker_index, std::size_t local_index) {
    const access_coord coord = make_access_coord(global_base, local_index, window_base, local_index, worker_index);
    access_fn(worker_index, coord);
  });
}

template <typename AccessFn, typename CloseFn>
[[nodiscard]] inline std::uint64_t run_windowed_access_span(
    std::optional<sn::threads::thread_team>& workers, std::size_t worker_count, std::uint64_t global_base,
    std::size_t access_count, std::size_t window_size, std::uint64_t window_base, AccessFn&& access_fn,
    CloseFn&& close_window
) {
  std::uint64_t window_count = 0;
  std::uint64_t next_global = global_base;
  std::size_t remaining = access_count;

  while (remaining > 0) {
    const std::size_t current_window = std::min(window_size, remaining);
    const std::size_t current_workers = stream_worker_count(worker_count, current_window, current_window);

    run_plain_access_span(workers, current_workers, next_global, current_window, window_base + window_count, access_fn);
    close_window(window_base + window_count);

    next_global += current_window;
    remaining -= current_window;
    ++window_count;
  }

  return window_count;
}

// run a contiguous access span, with window handling
template <typename AccessFn, typename CloseFn>
[[nodiscard]] inline std::uint64_t run_access_span(
    std::optional<sn::threads::thread_team>& workers, std::size_t concurrency, std::uint64_t global_base,
    std::size_t access_count, std::size_t window_size, std::uint64_t window_base, AccessFn&& access_fn,
    CloseFn&& close_window
) {
  if (access_count == 0) {
    return 0;
  }

  const std::size_t worker_count = stream_worker_count(concurrency, access_count, window_size);
  if (window_size == 0) {
    run_plain_access_span(
        workers, worker_count, global_base, access_count, window_base, std::forward<AccessFn>(access_fn)
    );
    return 0;
  }

  return run_windowed_access_span(
      workers, worker_count, global_base, access_count, window_size, window_base, std::forward<AccessFn>(access_fn),
      std::forward<CloseFn>(close_window)
  );
}

} // namespace sn::oram::harness::detail
