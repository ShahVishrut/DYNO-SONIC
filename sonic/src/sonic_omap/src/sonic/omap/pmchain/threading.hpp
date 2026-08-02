#pragma once

#include <algorithm>
#include <cstddef>

#include "sonic/threads/parallelism.hpp"
#include "sonic/util/log.hpp"

namespace sn::omap::pmchain {

struct threading {
  sn::threads::parallelism_config domain{};
  sn::threads::parallelism_config access{};
  sn::threads::parallelism_config oram{};
  sn::threads::parallelism_config eviction{};
};

[[nodiscard]] inline threading resolve_threading(
    std::size_t access_logical, std::size_t oram_logical, std::size_t eviction_logical
) {
  const auto access = sn::threads::resolve_parallelism(access_logical);
  const auto oram =
      sn::threads::resolve_parallelism(oram_logical == 0 ? access.logical : std::min(oram_logical, access.logical));
  const auto eviction = sn::threads::resolve_parallelism(eviction_logical);
  sn::util::log::ensure(access.logical > 0, "pmchain::threading: access logical threads must be positive");
  sn::util::log::ensure(eviction.logical > 0, "pmchain::threading: eviction logical threads must be positive");
  sn::util::log::ensure(oram.logical > 0, "pmchain::threading: oram logical threads must be positive");
  sn::util::log::ensure(
      oram.logical <= access.logical, "pmchain::threading: oram logical threads cannot exceed access logical threads"
  );

  threading plan{};
  plan.access = access;
  plan.oram = oram;
  plan.eviction = eviction;
  plan.domain = sn::threads::resolve_parallelism(std::max(access.logical, eviction.logical));
  return plan;
}

} // namespace sn::omap::pmchain
