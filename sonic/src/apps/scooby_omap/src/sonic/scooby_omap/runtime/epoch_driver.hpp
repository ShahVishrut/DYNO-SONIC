#pragma once

#include "sonic/sgxbridge/common/time.hpp"

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/runtime/epoch.hpp"

namespace sn::scooby::omap {

enum class endpoint_kind { client, load_balancer, suboram };

constexpr const char* endpoint_label(endpoint_kind endpoint) {
  switch (endpoint) {
  case endpoint_kind::client:
    return "client";
  case endpoint_kind::load_balancer:
    return "lb";
  case endpoint_kind::suboram:
    return "suboram";
  }
  return "unknown";
}

inline auto epoch_guard(const plan_config& plan, const sn::sgxbridge::time::steady_clock::time_point& start_time) {
  return [&](std::uint64_t epoch) {
    if (plan.stress_runtime_seconds == 0) {
      return epoch < plan.epoch_count;
    }
    const double elapsed =
        sn::sgxbridge::time::to_nanoseconds(sn::sgxbridge::time::since(start_time)) / 1'000'000'000.0;
    return elapsed < static_cast<double>(plan.stress_runtime_seconds);
  };
}

template <typename State> class epoch_scope {
public:
  epoch_scope(State& state, endpoint_kind endpoint, std::uint64_t epoch) :
      state_(state), endpoint_(endpoint), epoch_(epoch) {
    ++state_.metrics.epochs_started;
    state_.epochs.set_expected(epoch_, state_.epoch_expectations);
  }

  epoch_scope(const epoch_scope&) = delete;
  epoch_scope& operator=(const epoch_scope&) = delete;

  ~epoch_scope() {
    if (completed_) {
      state_.epochs.erase(epoch_);
    }
  }

  void mark_completed() {
    ++state_.metrics.epochs_completed;
    completed_ = true;
  }

  endpoint_kind endpoint() const { return endpoint_; }
  std::uint64_t epoch() const { return epoch_; }

private:
  State& state_;
  endpoint_kind endpoint_;
  std::uint64_t epoch_{0};
  bool completed_{false};
};

template <typename State, typename EpochFn, typename AfterEpochFn>
bool drive_epochs(
    State& state, const plan_config& plan, endpoint_kind endpoint, EpochFn&& per_epoch, AfterEpochFn&& after_epoch
) {
  auto should_continue = epoch_guard(plan, state.start_time);
  bool ok = true;
  for (std::uint64_t epoch = 0; should_continue(epoch); ++epoch) {
    epoch_scope<State> scope(state, endpoint, epoch);
    if (!per_epoch(epoch)) {
      ok = false;
      break;
    }
    scope.mark_completed();
    after_epoch(epoch);
  }
  return ok;
}

}
