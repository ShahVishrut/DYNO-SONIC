#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/oram/harness/detail/access_stream.hpp"
#include "sonic/oram/harness/detail/client_access.hpp"
#include "sonic/oram/harness/detail/leaf_table.hpp"
#include "sonic/oram/harness/detail/run_plan.hpp"
#include "sonic/oram/harness/detail/shadow_state.hpp"
#include "sonic/oram/harness/detail/workers.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::harness::detail {

[[nodiscard]] inline std::size_t validate_window_size(const run_plan& plan, std::size_t access_count) noexcept {
  if (plan.mode == run_mode::disjoint_windowed) {
    return plan.requested_window_size;
  }
  if (plan.mode == run_mode::disjoint_online_only) {
    return plan.requested_window_size != 0 ? std::min(plan.requested_window_size, access_count) : access_count;
  }
  return 0;
}

template <typename Buffer> inline void fill_address_pattern(Buffer& buffer, std::uint64_t address) noexcept {
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = static_cast<std::uint8_t>((address + static_cast<std::uint64_t>(i)) & 0xFFu);
  }
}

template <std::size_t BlockBytes, typename Client, typename AccessFn>
inline void for_each_validate_access(
    Client& client, const run_plan& plan, const leaf_table& leaves, std::optional<sn::threads::thread_team>& workers,
    std::size_t access_count, seed_stream& seeds, AccessFn&& access_fn
) {
  const std::size_t window_size = validate_window_size(plan, access_count);
  const std::size_t worker_count = stream_worker_count(plan.concurrency, access_count, window_size);
  auto states = make_stream_worker_states<BlockBytes>(client, worker_count, seeds);

  (void) run_access_span(
      workers, states.size(), 0, access_count, window_size, 0,
      [&](std::size_t worker_index, const access_coord& coord) {
        auto& state = states[worker_index];
        const std::uint64_t address = plan.schedule.address(coord);
        const std::uint64_t cur_leaf = leaves.current(address);
        const std::uint64_t new_leaf = sample_leaf(state.rng, plan.leaf_count);
        access_fn(state, coord, address, cur_leaf, new_leaf);
      },
      [&](std::uint64_t) { apply_window_close(client, plan.close); }
  );
}

template <typename traits_t, typename Client>
void run_dummy_probe(Client& client, std::uint64_t leaf_count, seed_stream& seeds, window_close close_mode) {
  constexpr std::int64_t dummy_address = traits_t::block_t::dummy_address;
  std::array<std::uint8_t, traits_t::block_t::byte_size> in_buf{};
  std::array<std::uint8_t, traits_t::block_t::byte_size> out_buf{};
  auto rng = make_prng(seeds);
  access_scratch_type<Client> scratch;
  configure_access_scratch(client, scratch);

  auto issue = [&](bool is_write) {
    const std::uint64_t cur_leaf = sample_leaf(rng, leaf_count);
    const std::uint64_t new_leaf = sample_leaf(rng, leaf_count);
    if (is_write) {
      fill_address_pattern(in_buf, 0xD00Du);
    } else {
      in_buf.fill(0);
    }
    out_buf.fill(0);
    issue_access(client, scratch, dummy_address, cur_leaf, new_leaf, is_write, in_buf, out_buf);
    apply_window_close(client, close_mode);
  };

  issue(false);
  issue(true);
}

template <typename traits_t, typename Client>
void run_round_trip(
    Client& client, leaf_table& leaves, std::uint64_t leaf_count, seed_stream& seeds, window_close close
) {
  std::array<std::uint8_t, traits_t::block_t::byte_size> in_buf{};
  std::array<std::uint8_t, traits_t::block_t::byte_size> out_buf{};
  std::array<std::uint8_t, traits_t::block_t::byte_size> expected{};
  auto rng = make_prng(seeds);
  access_scratch_type<Client> scratch;
  configure_access_scratch(client, scratch);

  constexpr std::uint64_t address = 0;
  fill_address_pattern(in_buf, address);
  fill_address_pattern(expected, address);

  const std::uint64_t write_leaf = leaves.current(address);
  const std::uint64_t write_new_leaf = sample_leaf(rng, leaf_count);
  issue_access(client, scratch, static_cast<std::int64_t>(address), write_leaf, write_new_leaf, true, in_buf, out_buf);
  leaves.commit(address, write_new_leaf);
  apply_window_close(client, close);

  in_buf.fill(0);
  out_buf.fill(0);
  const std::uint64_t read_leaf = leaves.current(address);
  const std::uint64_t read_new_leaf = sample_leaf(rng, leaf_count);
  issue_access(client, scratch, static_cast<std::int64_t>(address), read_leaf, read_new_leaf, false, in_buf, out_buf);
  sn::util::log::ensure(
      std::equal(out_buf.begin(), out_buf.end(), expected.begin()), "validate: serial round-trip mismatch"
  );
  leaves.commit(address, read_new_leaf);
  apply_window_close(client, close);
}

template <typename traits_t, typename Client>
void run_stateful_batch_pass(
    Client& client, const run_plan& plan, leaf_table& leaves, shadow_state<traits_t::block_t::byte_size>& shadow,
    std::optional<sn::threads::thread_team>& workers, std::size_t access_count, bool is_write, seed_stream& seeds
) {
  for_each_validate_access<traits_t::block_t::byte_size>(
      client, plan, leaves, workers, access_count, seeds,
      [&](auto& state, const access_coord&, std::uint64_t address, std::uint64_t cur_leaf, std::uint64_t new_leaf) {
        auto& expected = shadow.block(address);

        if (is_write) {
          fill_address_pattern(expected, address);
          state.out_buf.fill(0);
          issue_access(
              client, state.scratch, static_cast<std::int64_t>(address), cur_leaf, new_leaf, true, expected,
              state.out_buf
          );
        } else {
          state.in_buf.fill(0);
          state.out_buf.fill(0);
          issue_access(
              client, state.scratch, static_cast<std::int64_t>(address), cur_leaf, new_leaf, false, state.in_buf,
              state.out_buf
          );
          sn::util::log::ensure(
              std::equal(state.out_buf.begin(), state.out_buf.end(), expected.begin()), "validate: data mismatch"
          );
        }

        leaves.commit(address, new_leaf);
      }
  );
}

template <typename traits_t, typename Client>
void run_online_only_batch_pass(
    Client& client, const run_plan& plan, const leaf_table& leaves, std::optional<sn::threads::thread_team>& workers,
    std::size_t access_count, seed_stream& seeds
) {
  for_each_validate_access<traits_t::block_t::byte_size>(
      client, plan, leaves, workers, access_count, seeds,
      [&](auto& state, const access_coord& coord, std::uint64_t address, std::uint64_t cur_leaf,
          std::uint64_t new_leaf) {
        const bool is_write = (coord.global_index & 1u) != 0u;

        if (is_write) {
          fill_address_pattern(state.in_buf, address);
        } else {
          state.in_buf.fill(0);
        }
        state.out_buf.fill(0);
        issue_access(
            client, state.scratch, static_cast<std::int64_t>(address), cur_leaf, new_leaf, is_write, state.in_buf,
            state.out_buf
        );
      }
  );
}

template <typename traits_t, typename Client>
void run_stateful_batch_validate(
    Client& client, const run_plan& plan, leaf_table& leaves, shadow_state<traits_t::block_t::byte_size>& shadow,
    std::optional<sn::threads::thread_team>& workers, std::size_t access_count, seed_stream& seeds
) {
  run_stateful_batch_pass<traits_t>(client, plan, leaves, shadow, workers, access_count, true, seeds);
  run_stateful_batch_pass<traits_t>(client, plan, leaves, shadow, workers, access_count, false, seeds);
}

} // namespace sn::oram::harness::detail
