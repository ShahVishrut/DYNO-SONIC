#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include "sonic/oram/harness/detail/client_access.hpp"
#include "sonic/oram/harness/config.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::harness::detail {

struct worker_setup {
  std::size_t worker_count = 1;
  std::optional<sn::threads::thread_team> parallel{};
};

[[nodiscard]] inline worker_setup resolve_worker_setup(const worker_pool_config& workers) {
  worker_setup setup{};
  if (workers.pool == nullptr) {
    sn::util::log::ensure(workers.max_workers == 0, "worker config: max_workers requires a non-null worker pool");
    return setup;
  }

  sn::threads::thread_team parallel(*workers.pool);
  const std::size_t available = parallel.logical_threads();
  sn::util::log::ensure(available > 0, "worker config: worker pool has zero logical workers");
  if (workers.max_workers != 0) {
    sn::util::log::ensure(
        workers.max_workers <= available, "worker config: max_workers exceeds logical worker capacity"
    );
    parallel = parallel.limited_to(workers.max_workers);
  }

  setup.worker_count = parallel.logical_threads();
  setup.parallel = std::move(parallel);
  return setup;
}

template <typename Client>
[[nodiscard]] inline std::size_t resolve_concurrency(
    const Client& client, std::size_t available_workers, std::size_t block_count
) noexcept {
  const std::size_t client_cap = max_access_concurrency(client);
  return std::max<std::size_t>(1, std::min({available_workers, client_cap, block_count}));
}

template <typename Fn>
inline void for_each_active_worker(
    std::optional<sn::threads::thread_team>& workers, std::size_t active_workers, Fn&& fn
) noexcept {
  if (!workers || active_workers <= 1) {
    fn(0);
    return;
  }

  const std::size_t logical_workers = workers->logical_threads();
  workers->parallel_work([&, logical_workers](std::size_t worker_index) noexcept {
    if (worker_index >= active_workers || worker_index >= logical_workers) {
      return;
    }
    fn(worker_index);
  });
}

} // namespace sn::oram::harness::detail
