#pragma once

#include "sonic/omap/pmchain/access.hpp"
#include "sonic/omap/pmchain/state.hpp"
#include "sonic/omap/pmchain/types.hpp"
#include "sonic/util/log.hpp"

namespace sn::omap::pmchain {

template <typename O2THClient, typename OramClient> class client {
public:
  using types = pmchain::types<O2THClient, OramClient>;
  using state_type = pmchain::state<O2THClient, OramClient>;
  using o2th_client_type = typename types::o2th_client_type;
  using oram_client_type = typename types::oram_client_type;
  using key_type = typename types::key_type;
  using req_type = typename types::req_type;
  template <typename T> using maybe_dummy = typename types::template maybe_dummy<T>;
  using data_query = typename types::data_query;
  using bucket_index = typename types::bucket_index;
  using operation = typename types::operation;

  client(
      config cfg, o2th_client_type& posmap, oram_client_type& oram, sn::threads::thread_team access_team,
      sn::util::log::logger log = sn::util::log::create("pmchain:client")
  ) :
      state_(cfg, posmap, oram, std::move(access_team), std::move(log)) {}

  void initialize() { pmchain::initialize(state_); }

  [[nodiscard]] std::size_t block_size() const noexcept { return state_.block_size(); }
  [[nodiscard]] std::size_t block_count() const noexcept { return state_.block_count(); }
  [[nodiscard]] std::size_t batch_size() const noexcept { return state_.batch_size(); }

  [[nodiscard]] sn::util::span<std::uint8_t> request_buffer(std::size_t slot) { return state_.request_buffer(slot); }
  [[nodiscard]] sn::util::span<const std::uint8_t> request_buffer(std::size_t slot) const {
    return state_.request_buffer(slot);
  }

  void populate_requests(sn::util::span<const operation> ops) { pmchain::populate_requests(state_, ops); }

  void execute_o2th_chains() { pmchain::execute_o2th_chains(state_); }

  void sort_o2th_chains() { pmchain::sort_o2th_chains(state_); }

  void execute_oram_queries() { pmchain::execute_oram_queries(state_); }

  void flush_pending() { pmchain::flush_pending(state_); }

  [[nodiscard]] sn::util::span<maybe_dummy<req_type>> retrieved_requests() {
    return pmchain::retrieved_requests(state_);
  }

  [[nodiscard]] sn::util::span<const maybe_dummy<req_type>> retrieved_requests() const {
    return pmchain::retrieved_requests(state_);
  }

  void shutdown() { pmchain::shutdown(state_); }

private:
  state_type state_;
};

} // namespace sn::omap::pmchain
