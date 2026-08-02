#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <type_traits>

#include "sonic/crypto/random.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/oram/core/access_ops.hpp"
#include "sonic/oram/adapter/helpers.hpp"
#include "sonic/oram/adapter/types.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::adapter {

template <typename BackingOram> class direct_block {
public:
  using backing_type = BackingOram;
  using backing_state_type = std::remove_reference_t<decltype(std::declval<backing_type&>().state_ref())>;
  using backing_block_type = typename backing_state_type::block_t;
  using backing_access_scratch = typename backing_state_type::access_scratch;
  static constexpr std::size_t logical_block_bytes = backing_block_type::byte_size;

  using block_t = logical_block<backing_block_type, logical_block_bytes>;
  using options_t = logical_options;
  using logical_access_request = adapter::logical_access_request;
  using access_scratch = adapter::access_scratch<backing_access_scratch>;

  direct_block(
      backing_type& backing, options_t logical_opts,
      sn::util::log::logger log = sn::util::log::create("oram.adapter.direct_block")
  ) :
      opts_(logical_opts), backing_(backing), log_(std::move(log)) {
    sn::util::log::ensure(opts_.block_count > 0, "direct_block: logical block_count must be positive");
    sn::util::log::ensure(
        opts_.block_count <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
        "direct_block: logical block_count exceeds 32-bit address space"
    );
    sn::util::log::ensure(
        backing_.options().block_count == opts_.block_count, "direct_block: backing block_count mismatch"
    );
    sn::util::log::ensure(
        opts_.disjoint_epoch_window == backing_.options().disjoint_epoch_window,
        "direct_block: disjoint_epoch_window mismatch"
    );
    log_.inff(
        "adapter::direct_block: logical_blocks=%zu logical_block_bytes=%zu window=%llu backing_blocks=%zu "
        "backing_block_bytes=%zu leaf_count=%llu",
        opts_.block_count, logical_block_bytes, static_cast<unsigned long long>(opts_.disjoint_epoch_window),
        backing_.options().block_count, backing_block_type::byte_size,
        static_cast<unsigned long long>(backing_.shape().leaf_count)
    );
  }

  void initialize() {
    backing_.initialize();
    detail::reseed_prng(adapter_prng_);
    prf_key_ = sn::crypto::prf::generate_key(adapter_prng_.engine());
  }

  void configure_access_scratch(access_scratch& scratch) const {
    backing_.configure_access_scratch(scratch.backing);
    scratch.prf.set_key(prf_key_);
    detail::reseed_prng(scratch.prng);
  }

  template <typename Mutator = sn::oram::read_write_mutator>
  void access(const logical_access_request& req, access_scratch& scratch, Mutator&& mutator = Mutator{}) {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(req.in.size() == logical_block_bytes, "direct_block: input span size mismatch");
    sn::util::log::ensure(req.out.size() == logical_block_bytes, "direct_block: output span size mismatch");
    sn::util::log::ensure(req.address >= 0, "direct_block: logical address must be non-negative");
    sn::util::log::ensure(
        static_cast<std::size_t>(req.address) < opts_.block_count, "direct_block: logical address out of range"
    );
    sn::util::log::ensure(
        static_cast<std::uint64_t>(req.address) <= std::numeric_limits<std::uint32_t>::max(),
        "direct_block: logical address exceeds 32-bit space"
    );
#endif

    const sn::obliv::choice is_real(!req.is_dummy);
    const std::uint64_t leaf_count = backing_.shape().leaf_count;
    const std::uint32_t addr32 = static_cast<std::uint32_t>(req.address);

    // curr_leaf <- prf(address, counter)
    const std::uint64_t real_curr_leaf = detail::derive_leaf(scratch.prf, leaf_count, addr32, req.counter);
    // next_leaf <- prf(address, counter + 1)
    const std::uint64_t real_next_leaf =
        detail::derive_leaf(scratch.prf, leaf_count, addr32, static_cast<std::uint32_t>(req.counter + 1U));

    // random leaves for dummy accesses
    const std::uint64_t fake_curr_leaf = scratch.prng.random_u64(0, leaf_count);
    const std::uint64_t fake_next_leaf = scratch.prng.random_u64(0, leaf_count);

    // conditional real/dummy selection
    const std::uint64_t curr_leaf =
        sn::obliv::ct_select<std::uint64_t>(real_curr_leaf, fake_curr_leaf, is_real.unwrap());
    const std::uint64_t next_leaf =
        sn::obliv::ct_select<std::uint64_t>(real_next_leaf, fake_next_leaf, is_real.unwrap());
    const std::int64_t address = sn::obliv::ct_select<std::int64_t>(
        req.address, static_cast<std::int64_t>(backing_block_type::dummy_address), is_real.unwrap()
    );

    sn::oram::access_request backing_req{};
    backing_req.address = address;
    backing_req.cur_leaf = static_cast<std::int64_t>(curr_leaf);
    backing_req.new_leaf = static_cast<std::int64_t>(next_leaf);
    backing_req.is_write = req.is_write;
    backing_req.in = req.in;
    backing_req.out = req.out;
    backing_.access(backing_req, scratch.backing, std::forward<Mutator>(mutator));
  }

  void flush_epoch() { backing_.flush_epoch(); }

  void drop_epoch() {
    if constexpr (requires(backing_type& backing) { backing.drop_epoch(); }) {
      backing_.drop_epoch();
    } else {
      sn::util::log::fail("direct_block: backing ORAM does not support drop_epoch()");
    }
  }

  [[nodiscard]] const options_t& options() const noexcept { return opts_; }
  [[nodiscard]] const auto& shape() const noexcept { return backing_.shape(); }

  direct_block& state_ref() noexcept { return *this; }
  const direct_block& state_ref() const noexcept { return *this; }

  backing_type& backing_ref() noexcept { return backing_; }
  const backing_type& backing_ref() const noexcept { return backing_; }

  auto& backing_state_ref() noexcept { return backing_.state_ref(); }
  const auto& backing_state_ref() const noexcept { return backing_.state_ref(); }

private:
  options_t opts_{};
  backing_type& backing_;
  sn::crypto::prf::key_type prf_key_{};
  mutable sn::crypto::buffered_prng<> adapter_prng_{};
  sn::util::log::logger log_;
};

} // namespace sn::oram::adapter
