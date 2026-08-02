#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/oram/core/access_ops.hpp"
#include "sonic/oram/pathoram/state.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::oram::pathoram {

template <typename Traits>
void read_path(
    state<Traits>& st, std::uint64_t leaf_ix, sn::oram::tree::path_buffer<typename Traits::block_t>& buffer
) {
  sn_prof_zone("pathoram.path.read");
  constexpr std::size_t bucket_size = Traits::bucket_size;

  const auto& shape = st.shape();
  auto& topo = st.topology();
  auto& storage = st.storage();

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(leaf_ix < shape.leaf_count, "pathoram::read_path: leaf_ix out of range");
  sn::util::log::ensure(
      buffer.height() == shape.height && buffer.bucket_size() == bucket_size,
      "pathoram::read_path: buffer shape mismatch"
  );
#endif

  // path buffer
  auto view = buffer.view();
  auto node_ids = view.node_ids();

  // get nodes along path
  topo.path_to_leaf(leaf_ix, node_ids);

  // read path blocks into buffer
  for (std::uint64_t depth = 0; depth <= shape.height; ++depth) {
    const std::uint64_t node_id = node_ids[depth];
    auto bucket_span = view.bucket_span(depth);
    auto& bucket = storage[node_id];
    sn::obliv::copy_n(bucket.slots.begin(), bucket_size, bucket_span.begin());
  }

#if defined(ORAM_DEBUG)
  st.log().dbgf("pathoram::read_path: fetched path leaf_ix=%d", leaf_ix);
  sn::oram::tree::debug::log_path_buffer(st.log(), view);
#endif
}

template <typename Traits, typename Mutator>
void access_path(
    state<Traits>& st, const sn::oram::access_request& req, typename state<Traits>::access_scratch& scratch,
    Mutator&& mutator
) {
#if defined(ORAM_DEBUG)
  st.log().trcf(
      "access_path: %s($%08x) leaf_ix=%d new_leaf_ix=%d", req.is_write ? "write" : "read", req.address, req.cur_leaf,
      req.new_leaf
  );
#endif

  constexpr std::size_t bucket_size = Traits::bucket_size;

  auto& stash = st.stash();
  auto& uid_gen = st.uid_gen();
  auto& buffer = scratch.buffer;
  using block_t = typename Traits::block_t;
  const sn::obliv::choice request_is_real = sn::obliv::choice(req.address >= 0);

#if defined(ORAM_DEBUG)
  const auto& shape = st.shape();
  sn::oram::validate_access<block_t>(
      static_cast<std::int64_t>(st.options().block_count), static_cast<std::int64_t>(shape.leaf_count), req
  );
  sn::util::log::ensure(
      buffer.height() == shape.height && buffer.bucket_size() == bucket_size,
      "pathoram::access_path: access buffer shape mismatch"
  );
#endif

  // read path to buffer
  {
    sn_prof_zone("pathoram.path.read_buffer");
    read_path<Traits>(st, static_cast<std::uint64_t>(req.cur_leaf), buffer);
  }
  // add path blocks to stash
  {
    sn_prof_zone("pathoram.stash.absorb");
    stash.absorb_path(buffer);
  }

  // extract target block from stash (or dummy if not present)
  block_t block;
  {
    sn_prof_zone("pathoram.stash.extract");
    block = stash.extract(req.address);
  }
  const sn::obliv::choice target_is_dummy = !block.is_real();
  const sn::obliv::choice should_materialize = request_is_real && target_is_dummy;

#if defined(ORAM_DEBUG)
  if (request_is_real.unwrap()) {
    sn::oram::access_debug::verify_materialization_consistency(
        st.assigned_blocks(), static_cast<std::uint64_t>(req.address), target_is_dummy, "pathoram::access_path"
    );
  }
#endif

  {
    sn_prof_zone("pathoram.path.materialize");
    sn::oram::materialize_block_cond(block, req.address, uid_gen, should_materialize);
  }

#if defined(ORAM_DEBUG)
  if (request_is_real.unwrap()) {
    sn::oram::access_debug::record_materialization(
        st.assigned_blocks(), static_cast<std::uint64_t>(req.address), block, "pathoram::access_path"
    );
  }
#endif

  // apply read/write request to block, and update leaf
  {
    sn_prof_zone("pathoram.path.apply");
    std::forward<Mutator>(mutator)(block, req);
    block.leaf_ix = req.new_leaf;
  }

  // add block back to stash
  {
    sn_prof_zone("pathoram.stash.insert");
    stash.insert(block);
  }
}

} // namespace sn::oram::pathoram
