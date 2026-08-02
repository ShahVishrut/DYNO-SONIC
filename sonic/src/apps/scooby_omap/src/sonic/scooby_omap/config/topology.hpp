#pragma once

#include "sonic/scooby_omap/config/plan.hpp"

namespace sn::scooby::omap {

struct topology_plan {
  std::int32_t client_base{0};
  std::int32_t load_balancer_base{0};
  std::int32_t suboram_base{0};
  std::int32_t total_roles{0};
};

struct role_assignment {
  scooby_omap_role role{scooby_omap_role::client};
  std::uint32_t index{0};
};

inline topology_plan build_topology(const plan_config& plan, int world_size) {
  topology_plan topo{};
  topo.client_base = 0;
  topo.load_balancer_base = topo.client_base + static_cast<std::int32_t>(plan.client_count);
  topo.suboram_base = topo.load_balancer_base + static_cast<std::int32_t>(plan.load_balancer_count);
  topo.total_roles = topo.suboram_base + static_cast<std::int32_t>(plan.suboram_count);
  sn::util::log::ensuref(
      world_size >= topo.total_roles, "scooby-omap: world_size=%d smaller than required roles=%d", world_size,
      topo.total_roles
  );
  return topo;
}

inline topology_plan build_topology(const plan_config& plan) {
  return build_topology(plan, sn::sgxbridge::dist::world_size());
}

inline int client_rank(const topology_plan& topo, std::uint32_t index) {
  return topo.client_base + static_cast<int>(index);
}

inline int load_balancer_rank(const topology_plan& topo, std::uint32_t index) {
  return topo.load_balancer_base + static_cast<int>(index);
}

inline int suboram_rank(const topology_plan& topo, std::uint32_t index) {
  return topo.suboram_base + static_cast<int>(index);
}

inline role_assignment derive_role_for_rank(const topology_plan& topo, int rank) {
  sn::util::log::ensuref(
      rank >= 0 && rank < topo.total_roles, "scooby-omap: rank=%d outside configured topology roles=%d", rank,
      topo.total_roles
  );
  if (rank < topo.load_balancer_base) {
    return role_assignment{scooby_omap_role::client, static_cast<std::uint32_t>(rank - topo.client_base)};
  }
  if (rank < topo.suboram_base) {
    return role_assignment{scooby_omap_role::load_balancer, static_cast<std::uint32_t>(rank - topo.load_balancer_base)};
  }
  return role_assignment{scooby_omap_role::suboram, static_cast<std::uint32_t>(rank - topo.suboram_base)};
}

inline role_assignment derive_role(const topology_plan& topo) {
  const int rank = sn::sgxbridge::dist::rank();
  return derive_role_for_rank(topo, rank);
}

}
