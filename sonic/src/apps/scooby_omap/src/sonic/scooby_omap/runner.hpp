#pragma once

#include "sonic/scooby_node/types/context.hpp"
#include "sonic/scooby_node/types/intents.hpp"

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/topology.hpp"
#include "sonic/scooby_omap/client/runner.hpp"
#include "sonic/scooby_omap/load_balancer/runner.hpp"
#include "sonic/scooby_omap/suboram/runner.hpp"

namespace sn::scooby::omap {

inline types::command_result run(const types::scooby_omap_intent& intent, types::execution_context& ctx) {
  if (!ctx.transport_enabled) {
    return types::make_result(types::result_status::internal_error, "distributed transport unavailable");
  }

  plan_config plan = make_plan(intent);
  const topology_plan topo = build_topology(plan);
  if (plan.auto_assign_role) {
    const role_assignment derived = derive_role(topo);
    plan.role = derived.role;
    plan.role_index = derived.index;
    ctx.logger.inf(
        pfm::format(
            "scooby-omap auto role rank=%d resolved=%s index=%u", sn::sgxbridge::dist::rank(), describe_role(plan.role),
            plan.role_index
        )
    );
  }
  log_plan_summary(plan, ctx);

  switch (plan.role) {
  case scooby_omap_role::client:
    return run_client(plan, ctx);
  case scooby_omap_role::load_balancer:
    return run_load_balancer(plan, ctx);
  case scooby_omap_role::suboram:
    return run_suboram(plan, ctx);
  }

  return types::make_result(types::result_status::internal_error, "unknown scooby-omap role");
}

}
