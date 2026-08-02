#pragma once

#include <string>
#include <utility>

#include "sonic/oram/core/access_ops.hpp"
#include "sonic/oram/pathoram/access.hpp"
#include "sonic/oram/pathoram/eviction.hpp"
#include "sonic/oram/pathoram/state.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::oram::pathoram {

template <typename Traits> class client {
public:
  using options_t = typename Traits::options_t;
  using state_type = sn::oram::pathoram::state<Traits>;
  using access_scratch = typename state_type::access_scratch;

  explicit client(options_t opts) :
      log_(sn::util::log::create("pathoram:client")),
      st_(std::move(opts), log_.child("pathoram")),
      scheduler_(st_.options().evict_batch) {
    log_.inf(
        "creating pathoram: block_count=" + std::to_string(st_.options().block_count) +
        " height=" + std::to_string(st_.shape().height)
    );
  }

  void initialize() {
    sn_prof_zone("pathoram.client.initialize");
    st_.initialize();
  }

  void configure_access_scratch(access_scratch& scratch) const { st_.configure_access_scratch(scratch); }

  template <typename Mutator = sn::oram::read_write_mutator>
  void access(const sn::oram::access_request& req, access_scratch& scratch, Mutator&& mutator = Mutator{}) {
    sn_prof_zone("pathoram.client.access");
    access_path<Traits>(st_, req, scratch, std::forward<Mutator>(mutator));
    scheduler_.record_access(static_cast<std::uint64_t>(req.cur_leaf));
    if (scheduler_.needs_eviction()) {
      eviction_request ev{scheduler_.take_evict_leaves()};
      sn_prof_zone("pathoram.client.evict");
      evict(st_, ev, st_.eviction_buffers());
    }
  }

  const options_t& options() const noexcept { return st_.options(); }
  const typename state_type::geometry& shape() const noexcept { return st_.shape(); }

private:
  sn::util::log::logger log_;
  state_type st_;
  schedule scheduler_;
};

} // namespace sn::oram::pathoram
