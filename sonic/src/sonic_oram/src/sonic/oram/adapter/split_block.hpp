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

template <typename BackingOram, std::size_t SplitFactor> class split_block {
  static_assert(SplitFactor > 0, "split_block: SplitFactor must be positive");

public:
  using backing_type = BackingOram;
  using backing_state_type = std::remove_reference_t<decltype(std::declval<backing_type&>().state_ref())>;
  using backing_block_type = typename backing_state_type::block_t;
  using backing_access_scratch = typename backing_state_type::access_scratch;
  static constexpr std::size_t split_factor = SplitFactor;
  static constexpr std::size_t physical_block_bytes = backing_block_type::byte_size;
  static constexpr std::size_t logical_block_bytes = physical_block_bytes * split_factor;

  using block_t = logical_block<backing_block_type, logical_block_bytes>;
  using options_t = logical_options;
  using logical_access_request = adapter::logical_access_request;
  using access_scratch = adapter::access_scratch<backing_access_scratch>;

  split_block(
      backing_type& backing, options_t logical_opts,
      sn::util::log::logger log = sn::util::log::create("oram.adapter.split_block")
  ) :
      opts_(logical_opts), backing_(backing), log_(std::move(log)) {
    sn::util::log::ensure(
        opts_.block_count <= std::numeric_limits<std::size_t>::max() / split_factor, "split_block: block_count overflow"
    );
    const auto backing_opts = backing_.options();
    const auto backing_block_count = backing_opts.block_count;
    sn::util::log::ensure(opts_.block_count > 0, "split_block: logical block_count must be positive");
    sn::util::log::ensure(
        opts_.block_count <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() / split_factor),
        "split_block: logical block_count exceeds addressable range"
    );
    sn::util::log::ensure(
        backing_block_count == opts_.block_count * split_factor, "split_block: backing block_count mismatch"
    );
    sn::util::log::ensure(
        opts_.disjoint_epoch_window <= std::numeric_limits<std::uint64_t>::max() / split_factor,
        "split_block: disjoint_epoch_window overflow"
    );
    const std::uint64_t required_window = opts_.disjoint_epoch_window * split_factor;
    sn::util::log::ensure(
        backing_opts.disjoint_epoch_window >= required_window,
        "split_block: backing disjoint_epoch_window insufficient for split factor"
    );
    log_.inff(
        "adapter::split_block: logical_blocks=%zu logical_block_bytes=%zu split_factor=%zu logical_window=%llu "
        "backing_blocks=%zu backing_block_bytes=%zu backing_window=%llu leaf_count=%llu",
        opts_.block_count, logical_block_bytes, split_factor,
        static_cast<unsigned long long>(opts_.disjoint_epoch_window), backing_opts.block_count, physical_block_bytes,
        static_cast<unsigned long long>(backing_opts.disjoint_epoch_window),
        static_cast<unsigned long long>(backing_.shape().leaf_count)
    );
  }

  split_block(const split_block&) = delete;
  split_block& operator=(const split_block&) = delete;
  split_block(split_block&&) noexcept = default;
  split_block& operator=(split_block&&) noexcept = default;

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
    sn::util::log::ensure(req.in.size() == logical_block_bytes, "split_block: input span size mismatch");
    sn::util::log::ensure(req.out.size() == logical_block_bytes, "split_block: output span size mismatch");
    sn::util::log::ensure(req.address >= 0, "split_block: logical address must be non-negative");
    sn::util::log::ensure(
        static_cast<std::size_t>(req.address) < opts_.block_count, "split_block: logical address out of range"
    );
    sn::util::log::ensure(
        static_cast<std::uint64_t>(req.address) <= std::numeric_limits<std::uint32_t>::max(),
        "split_block: logical address exceeds 32-bit space"
    );
#endif

    const sn::obliv::choice is_real(!req.is_dummy);
    const std::uint64_t leaf_count = backing_.shape().leaf_count;
    const std::size_t part_bytes = physical_block_bytes;
    const std::uint32_t logical_addr32 = static_cast<std::uint32_t>(req.address);

    // sequentially retrieve adjacent parts of the block
    for (std::size_t part = 0; part < split_factor; ++part) {
      const std::size_t offset = part * part_bytes;
      auto in_part = sn::util::span<std::uint8_t>(req.in.data() + offset, part_bytes);
      auto out_part = sn::util::span<std::uint8_t>(req.out.data() + offset, part_bytes);

      // curr_leaf <- prf(address, part, counter)
      const std::uint64_t real_curr_leaf =
          detail::derive_leaf(scratch.prf, leaf_count, logical_addr32, static_cast<std::uint16_t>(part), req.counter);
      // next_leaf <- prf(address, part, counter + 1)
      const std::uint64_t real_next_leaf = detail::derive_leaf(
          scratch.prf, leaf_count, logical_addr32, static_cast<std::uint16_t>(part),
          static_cast<std::uint32_t>(req.counter + 1U)
      );

      // random leaves for dummy accesses
      const std::uint64_t fake_curr_leaf = scratch.prng.random_u64(0, leaf_count);
      const std::uint64_t fake_next_leaf = scratch.prng.random_u64(0, leaf_count);

      // phys_address = logical_address * SplitFactor + part
      const std::uint64_t base_addr = static_cast<std::uint64_t>(req.address) * split_factor;
      sn::util::log::ensure(
          base_addr <= std::numeric_limits<std::uint64_t>::max() - part, "split_block: address overflow"
      );
      const std::int64_t real_address = static_cast<std::int64_t>(base_addr + part);

      // conditional real/dummy selection
      const std::uint64_t curr_leaf =
          sn::obliv::ct_select<std::uint64_t>(real_curr_leaf, fake_curr_leaf, is_real.unwrap());
      const std::uint64_t next_leaf =
          sn::obliv::ct_select<std::uint64_t>(real_next_leaf, fake_next_leaf, is_real.unwrap());
      const std::int64_t address = sn::obliv::ct_select<std::int64_t>(
          real_address, static_cast<std::int64_t>(backing_block_type::dummy_address), is_real.unwrap()
      );

      sn::oram::access_request backing_req{};
      backing_req.address = address;
      backing_req.cur_leaf = static_cast<std::int64_t>(curr_leaf);
      backing_req.new_leaf = static_cast<std::int64_t>(next_leaf);
      backing_req.is_write = req.is_write;
      backing_req.in = in_part;
      backing_req.out = out_part;
      backing_.access(backing_req, scratch.backing, std::forward<Mutator>(mutator));
    }
  }

  void flush_epoch() { backing_.flush_epoch(); }

  void drop_epoch() {
    if constexpr (requires(backing_type& backing) { backing.drop_epoch(); }) {
      backing_.drop_epoch();
    } else {
      sn::util::log::fail("split_block: backing ORAM does not support drop_epoch()");
    }
  }

  [[nodiscard]] const options_t& options() const noexcept { return opts_; }
  [[nodiscard]] const auto& shape() const noexcept { return backing_.shape(); }

  split_block& state_ref() noexcept { return *this; }
  const split_block& state_ref() const noexcept { return *this; }

  backing_type& backing_ref() noexcept { return backing_; }
  const backing_type& backing_ref() const noexcept { return backing_; }

  auto& backing_state_ref() noexcept { return backing_.state_ref(); }
  const auto& backing_state_ref() const noexcept { return backing_.state_ref(); }

  backing_type& backing() noexcept { return backing_; }
  const backing_type& backing() const noexcept { return backing_; }

private:
  options_t opts_{};
  backing_type& backing_;
  sn::crypto::prf::key_type prf_key_{};
  mutable sn::crypto::buffered_prng<> adapter_prng_{};
  sn::util::log::logger log_;
};

} // namespace sn::oram::adapter
