#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "sonic/demo/logic/commands/common.hpp"
#include "sonic/demo/logic/context.hpp"
#include "sonic/demo/types/intents.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/threads/thread_pool.hpp"
#include "sonic/util/picoformat.hpp"

namespace sn::demo::logic::commands::parallel_scan {

constexpr std::uint32_t kDefaultWorkers = 4;
constexpr std::uint32_t kMaxWorkers = 32;
constexpr std::uint32_t kMaxElements = 1u << 20;

inline std::uint32_t clamp_workers(std::uint32_t requested) noexcept {
  const std::uint32_t workers = requested == 0 ? kDefaultWorkers : requested;
  return workers > kMaxWorkers ? kMaxWorkers : workers;
}

inline std::uint32_t clamp_elements(std::uint32_t requested) noexcept {
  const std::uint32_t elements = requested == 0 ? 1 : requested;
  return elements > kMaxElements ? kMaxElements : elements;
}

inline types::command_result run(const types::parallel_scan_intent& intent, execution_context& ctx) {
  if (!ctx.threadpools.available()) {
    return detail::make_error(types::result_status::unsupported, "thread pool provider unavailable");
  }

  const std::uint32_t requested_logical_workers = clamp_workers(intent.requested_workers);
  const std::uint32_t elements = clamp_elements(intent.elements);

  const auto parallelism = sn::threads::resolve_parallelism(static_cast<std::size_t>(requested_logical_workers));

  sn::sgxbridge::tp::session pool_session;
  const auto acquire = pool_session.open(
      ctx.threadpools, detail::make_threadpool_request(parallelism.background, "sonic_demo.parallel_scan")
  );
  if (!acquire.succeeded() || pool_session.pool() == nullptr) {
    return detail::session_error("sonic_demo.parallel_scan", acquire);
  }

  sn::threads::thread_team worker(pool_session.pool_ref());
  std::vector<std::uint64_t> partials(worker.logical_threads(), 0);

  ctx.logger.inff(
      "parallel_scan config: elements=%u workers=%zu (requested=%u background=%zu)", elements, worker.logical_threads(),
      requested_logical_workers, parallelism.background
  );

  worker.parallel_for<std::uint32_t>(0, elements, [&](std::uint32_t idix, std::size_t worker_index) noexcept {
    partials[worker_index] += static_cast<std::uint64_t>(idix);
  });

  std::uint64_t aggregate = 0;
  for (auto value : partials) {
    aggregate += value;
  }

  std::string partials_display = "partials: [ ";
  for (std::size_t i = 0; i < partials.size(); ++i) {
    if (i != 0) {
      partials_display.append(" | ");
    }
    partials_display.append(pfm::format("%llu", static_cast<unsigned long long>(partials[i])));
  }
  partials_display.append(" ]");
  ctx.logger.inf(partials_display);

  const std::uint64_t expected = static_cast<std::uint64_t>(elements) * static_cast<std::uint64_t>(elements - 1) / 2ull;
  if (aggregate != expected) {
    return detail::make_error(
        types::result_status::internal_error,
        pfm::format(
            "parallel scan validation failed: sum=%llu expected=%llu", static_cast<unsigned long long>(aggregate),
            static_cast<unsigned long long>(expected)
        )
    );
  }

  ctx.logger.inff(
      "validation ok: sum=%llu expected=%llu", static_cast<unsigned long long>(aggregate),
      static_cast<unsigned long long>(expected)
  );

  types::command_result result{};
  if (!result.output.assign(
          pfm::format(
              "parallel scan ok: elements=%u workers=%zu (requested=%u background=%zu) sum=%llu", elements,
              worker.logical_threads(), requested_logical_workers, parallelism.background,
              static_cast<unsigned long long>(aggregate)
          )
      )) {
    return detail::make_error(types::result_status::internal_error, "result output truncated");
  }
  result.status = types::result_status::ok;
  return result;
}

}
