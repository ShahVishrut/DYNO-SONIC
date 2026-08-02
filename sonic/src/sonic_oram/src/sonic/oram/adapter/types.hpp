#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/crypto/prf.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::adapter {

struct logical_options {
  std::size_t block_count = 0;             // logical block count
  std::uint64_t disjoint_epoch_window = 0; // logical disjoint window
};

struct logical_access_request {
  std::int64_t address = 0;
  std::uint32_t counter = 0;
  bool is_write = false;
  bool is_dummy = false;
  sn::util::span<std::uint8_t> in;
  sn::util::span<std::uint8_t> out;
};

template <typename BackingBlock, std::size_t LogicalBytes> struct logical_block {
  static constexpr std::size_t byte_size = LogicalBytes;
  static constexpr std::int64_t dummy_address = BackingBlock::dummy_address;
  std::array<std::uint8_t, byte_size> data{};
};

template <typename BackingAccessScratch> struct access_scratch {
  BackingAccessScratch backing{};
  sn::crypto::prf prf{};
  sn::crypto::buffered_prng<> prng{};
};

} // namespace sn::oram::adapter
