#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "sonic/omap/ods/pod_codec.hpp"
#include "sonic/omap/ods/ptr.hpp"

#include "sonic/oram/core/access.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::ods {

// buffers for oram request
template <typename OramClient> struct oram_record_io {
  using oram_client_type = OramClient;
  using oram_state_type = typename oram_client_type::state_type;
  using block_t = typename oram_state_type::block_t;
  static constexpr std::size_t block_bytes = block_t::byte_size;

  std::array<std::uint8_t, block_bytes> in{};
  std::array<std::uint8_t, block_bytes> out{};
};

// conditionally access a record in oram, with caller-provided mutator
template <typename OramClient, typename Record, typename Fn>
inline void access_record(
    OramClient& oram, const pending_ptr& p, sn::obliv::choice cond, std::uint64_t dummy_cur_leaf,
    std::uint64_t dummy_new_leaf, typename OramClient::access_scratch& oram_scratch, oram_record_io<OramClient>& io,
    Fn&& fn
) {
  using io_type = oram_record_io<OramClient>;
  using block_t = typename io_type::block_t;
  static constexpr std::size_t block_bytes = io_type::block_bytes;
  static_assert(std::is_trivially_copyable_v<Record>, "ods::access_record requires trivially copyable Record");
  static_assert(sizeof(Record) <= block_bytes, "ods::access_record Record does not fit in ORAM block");

  // dummify if it's a dummy access
  const std::int64_t addr_eff = sn::obliv::ct_select<std::int64_t>(p.addr, dummy_address, cond.unwrap());
  const std::uint64_t cur_eff = sn::obliv::ct_select<std::uint64_t>(p.cur_leaf, dummy_cur_leaf, cond.unwrap());
  const std::uint64_t new_eff = sn::obliv::ct_select<std::uint64_t>(p.new_leaf, dummy_new_leaf, cond.unwrap());

  sn::oram::access_request req{};
  req.address = addr_eff;
  req.cur_leaf = static_cast<std::int64_t>(cur_eff);
  req.new_leaf = static_cast<std::int64_t>(new_eff);
  req.is_write = true; // ignored by our mutator
  req.in = sn::util::span<std::uint8_t>(io.in.data(), io.in.size());
  req.out = sn::util::span<std::uint8_t>(io.out.data(), io.out.size());

  // mutator:
  // - decode record from the accessed block
  // - invoke caller-provided `fn(record)`
  // - encode the record back to the block
  auto mutator = [&](block_t& block, const sn::oram::access_request&) noexcept {
    Record rec = decode<Record, block_bytes>(block.data.data());
    std::forward<Fn>(fn)(rec);
    encode<Record, block_bytes>(block.data.data(), rec);
  };

  oram.access(req, oram_scratch, mutator);
}

} // namespace sn::omap::ods
