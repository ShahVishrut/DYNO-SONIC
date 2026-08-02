#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <utility>
#include <vector>

#include "sonic/omap/pmchain/types.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::pmchain {

template <typename O2THClient, typename OramClient> struct state {
  using types = pmchain::types<O2THClient, OramClient>;
  using o2th_client_type = typename types::o2th_client_type;
  using oram_client_type = typename types::oram_client_type;
  using key_type = typename types::key_type;
  using req_type = typename types::req_type;
  template <typename T> using maybe_dummy = typename types::template maybe_dummy<T>;
  using data_query = typename types::data_query;
  using bucket_index = typename types::bucket_index;
  using operation = typename types::operation;
  using worker_state = typename types::worker_state;
  using oram_state_type = typename types::oram_state_type;
  using oram_block_type = typename types::oram_block_type;

  state(
      config cfg_in, o2th_client_type& posmap_in, oram_client_type& oram_in, sn::threads::thread_team access_team_in,
      sn::util::log::logger log_in
  ) :
      cfg(cfg_in),
      posmap(posmap_in),
      oram(oram_in),
      access_team(std::move(access_team_in)),
      oram_team(access_team.limited_to(resolve_oram_parallelism(cfg_in, access_team.logical_threads()))),
      log(std::move(log_in)) {
    sn::util::log::ensure(cfg.block_count > 0, "pmchain::state: block_count must be positive");
    sn::util::log::ensure(cfg.batch_size > 0, "pmchain::state: batch_size must be positive");
    sn::util::log::ensure(cfg.oram_block_bytes > 0, "pmchain::state: oram_block_bytes must be positive");
    sn::util::log::ensure(cfg.batch_size <= cfg.block_count, "pmchain::state: batch_size cannot exceed block_count");
    const auto& oram_opts = oram.options();
    sn::util::log::ensure(oram_opts.block_count == cfg.block_count, "pmchain::state: oram block count mismatch");
    sn::util::log::ensure(
        oram_block_type::byte_size == cfg.oram_block_bytes, "pmchain::state: oram block size mismatch"
    );

    const std::size_t available_workers = access_team.logical_threads();
    sn::util::log::ensure(
        cfg.oram_parallelism == 0 || cfg.oram_parallelism <= available_workers,
        "pmchain::state: oram_parallelism exceeds available access workers"
    );
  }

  static std::size_t resolve_oram_parallelism(const config& cfg, std::size_t available_workers) {
    return cfg.oram_parallelism == 0 ? available_workers : std::min(cfg.oram_parallelism, available_workers);
  }

  [[nodiscard]] std::size_t block_size() const noexcept { return cfg.oram_block_bytes; }
  [[nodiscard]] std::size_t block_count() const noexcept { return cfg.block_count; }
  [[nodiscard]] std::size_t batch_size() const noexcept { return cfg.batch_size; }
  [[nodiscard]] std::size_t oram_parallelism() const noexcept { return cfg.oram_parallelism; }

  [[nodiscard]] sn::util::span<std::uint8_t> request_buffer(std::size_t slot) {
    return sn::util::span<std::uint8_t>(buffer_ptr(slot), cfg.oram_block_bytes);
  }

  [[nodiscard]] sn::util::span<const std::uint8_t> request_buffer(std::size_t slot) const {
    return sn::util::span<const std::uint8_t>(buffer_ptr(slot), cfg.oram_block_bytes);
  }

  [[nodiscard]] std::size_t buffer_offset(std::size_t slot) const {
    sn::util::log::ensure(slot < cfg.batch_size, "pmchain::state: buffer slot out of range");
    return slot * cfg.oram_block_bytes;
  }

  [[nodiscard]] std::uint8_t* buffer_ptr(std::size_t slot) { return oram_buffers.data() + buffer_offset(slot); }

  [[nodiscard]] const std::uint8_t* buffer_ptr(std::size_t slot) const {
    return oram_buffers.data() + buffer_offset(slot);
  }

  config cfg{};
  o2th_client_type& posmap;
  oram_client_type& oram;
  sn::threads::thread_team access_team;
  sn::threads::thread_team oram_team;
  bool pending_flush = false;
  bool retrieval_ready = false;

  std::vector<data_query> posmap_data{};
  std::vector<maybe_dummy<req_type>> posmap_reqs{};
  std::vector<bucket_index> pos_buf_l1{};
  std::vector<bucket_index> pos_buf_l2{};

  std::vector<maybe_dummy<req_type>> retrieve_buffer{};
  std::vector<std::uint8_t> compact_marks{};
  std::vector<std::size_t> compact_prefix{};
  std::vector<std::uint8_t> oram_buffers{};

  std::vector<worker_state> worker_states{};
  sn::util::log::logger log;
};

} // namespace sn::omap::pmchain
