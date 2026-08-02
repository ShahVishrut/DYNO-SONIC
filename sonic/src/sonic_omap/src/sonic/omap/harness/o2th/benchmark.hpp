#pragma once

#include <algorithm>
#include <cstddef>

#include "sonic/omap/harness/o2th/config.hpp"
#include "sonic/omap/harness/o2th/experiment.hpp"

namespace sn::omap::harness::o2th {

template <typename Table, typename Clock = sn::oram::harness::default_clock_traits>
benchmark_result benchmark(Table& table, const benchmark_options& opts) {
  experiment_options exp_opts{};
  exp_opts.iterations = std::max<std::size_t>(opts.iterations, static_cast<std::size_t>(1));
  exp_opts.workload = opts.workload;

  const auto exp = experiment<Table, Clock>(table, exp_opts);

  benchmark_result result{};
  result.build.seconds = exp.build_single.seconds + exp.build_batch.seconds;
  result.build.operations = exp.build_single.operations + exp.build_batch.operations;
  result.access_batch = exp.access_batch;
  result.retrieve.seconds = exp.retrieve_single.seconds + exp.retrieve_batch.seconds;
  result.retrieve.operations = exp.retrieve_single.operations + exp.retrieve_batch.operations;
  return result;
}

} // namespace sn::omap::harness::o2th
