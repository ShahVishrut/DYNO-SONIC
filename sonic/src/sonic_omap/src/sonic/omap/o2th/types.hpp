#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/omap/detail/block_data_buffer.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/omap/util/maybe_dummy.hpp"

namespace sn::omap::o2th {

// oblivious two-tier hashtable (O2TH) variant for a read-write (RW) key-value (KV) store
// here, the build set is the kv queries, and the query set is the dataset
// because accessing queries against data is symmetric to accessing data against queries
template <typename Key, std::size_t BlockSize> struct table_types {
  using key_type = Key;
  using bucket_index = std::uint32_t;
  static constexpr std::size_t block_size = BlockSize;
  static constexpr std::size_t block_word_count = block_size / sizeof(std::uint64_t);

  static_assert(std::is_integral_v<key_type>, "o2th_rwkv requires integral key type");
  static_assert(block_size % sizeof(std::uint64_t) == 0, "o2th_rwkv: block_size must be divisible by 8 bytes");

  using data_buffer = detail::block_data_buffer<block_size>;

  template <typename T> using maybe_dummy = sn::omap::util::maybe_dummy<T>;

  // configuration parameters
  struct config {
    std::size_t block_count = 0;
    std::size_t bucket_size = 0;
  };

  // read or write operation request, supplied for construction
  struct op_request {
    // logical key
    key_type key{};
    // data block
    data_buffer data{};
    // operation type (read/write)
    bool is_write = false;
    // caller-supplied metadata
    std::uint32_t extra_data = 0;
  };

  static_assert(
      offsetof(op_request, data) % alignof(std::uint64_t) == 0, "o2th_rwkv: op_request::data must be 64-bit aligned"
  );

  // a data item, issued as a query into the hashtable
  struct data_query {
    enum class flag : std::uint8_t {
      // no flags set
      none = 0,
      // this is a read query
      query_read = 1U << 0,
      // this is a write query
      query_write = 1U << 1,
      // query result was found (set during access)
      result_ok = 1U << 2,
    };

    // logical key
    key_type key{};
    // data block
    data_buffer data{};
    // flags
    std::uint8_t flags = static_cast<std::uint8_t>(flag::none);

    // assign a query with key, data payload, and read/write flag (constant-time)
    void assign(key_type key_in, sn::util::span<const std::uint8_t> payload, bool is_write_flag) noexcept {
#if defined(ORAM_DEBUG)
      sn::util::log::ensure(payload.size() == block_size, "o2th_rwkv: data_query payload size mismatch");
#endif
      key = key_in;
      sn::obliv::memcpy(data.data(), payload.data(), block_size);
      const std::uint8_t write_bit = static_cast<std::uint8_t>(flag::query_write);
      const std::uint8_t read_bit = static_cast<std::uint8_t>(flag::query_read);
      flags = sn::obliv::ct_select<std::uint8_t>(write_bit, read_bit, is_write_flag);
    }

    [[nodiscard]] bool is_write() const noexcept { return (flags & static_cast<std::uint8_t>(flag::query_write)) != 0; }

    [[nodiscard]] bool result_ok() const noexcept { return (flags & static_cast<std::uint8_t>(flag::result_ok)) != 0; }

    // conditoinally set result_ok (constant-time)
    void set_ok_cond(sn::obliv::choice cond) noexcept {
      const std::uint8_t new_flags = flags | static_cast<std::uint8_t>(flag::result_ok);
      flags = sn::obliv::ct_select<std::uint8_t>(new_flags, flags, cond.unwrap());
    }
  };

  static_assert(
      offsetof(data_query, data) % alignof(std::uint64_t) == 0, "o2th_rwkv: data_query::data must be 64-bit aligned"
  );

  // default mutator that does nothing
  struct mutator_none {
    void operator()(std::uint8_t*, const std::uint8_t*, sn::obliv::choice) const noexcept {}
  };

  // build set block flags
  enum class block_flag : std::uint8_t {
    // no flags
    none = 0,
    // excess
    excess = 1U << 0,
    // dummy
    dummy = 1U << 1,
    // filler
    filler = 1U << 2,
    // real
    real = 1U << 3,
    // read operation
    op_read = 1U << 4,
    // write operation
    op_write = 1U << 5,
  };

  // get mask for a specific flag
  static constexpr std::uint8_t flag_mask(block_flag flag) noexcept { return static_cast<std::uint8_t>(flag); }

  // check if a flag is set (constant time)
  [[nodiscard]] static sn::obliv::choice has_flag(std::uint8_t flags, block_flag flag) noexcept {
    const std::uint8_t mask = flag_mask(flag);
    return sn::obliv::choice(sn::obliv::ct_eq<std::uint8_t>(flags & mask, mask));
  }

  // conditionally set a flag (constant time)
  [[nodiscard]] static std::uint8_t set_flag_cond(
      std::uint8_t flags, block_flag flag, sn::obliv::choice cond
  ) noexcept {
    // if cond is true, set the flag; otherwise leave flags unchanged
    const std::uint8_t with_flag = flags | flag_mask(flag);
    return sn::obliv::ct_select<std::uint8_t>(with_flag, flags, cond.unwrap());
  }

  // conditionally clear a flag (constant time)
  [[nodiscard]] static std::uint8_t clear_flag_cond(
      std::uint8_t flags, block_flag flag, sn::obliv::choice cond
  ) noexcept {
    // if cond is true, clear the flag; otherwise leave flags unchanged
    const std::uint8_t without_flag = static_cast<std::uint8_t>(flags & ~flag_mask(flag));
    return sn::obliv::ct_select<std::uint8_t>(without_flag, flags, cond.unwrap());
  }

  // represents a block in the build set
  struct alignas(16) op_block {
    // key
    key_type key = invalid_key_value();
    // data
    data_buffer data{};
    // caller-supplied metadata
    std::uint32_t extra_data = 0;
    // level 1 sort tag
    bucket_index tag_l1 = 0;
    // level 2 sort tag
    bucket_index tag_l2 = 0;
    // flags
    std::uint8_t flags = flag_mask(block_flag::dummy);

    void reset() noexcept {
      key = invalid_key_value();
      data.fill(0);
      extra_data = 0;
      tag_l1 = 0;
      tag_l2 = 0;
      flags = flag_mask(block_flag::dummy);
    }

    static void swap_cond(op_block* a, op_block* b, bool cond) noexcept {
      sn::obliv::ct_swap_data<op_block>(a, b, cond);
    }

    [[nodiscard]] sn::obliv::choice is_real() const noexcept { return has_flag(flags, block_flag::real); }

    [[nodiscard]] sn::obliv::choice is_dummy() const noexcept { return has_flag(flags, block_flag::dummy); }

    [[nodiscard]] sn::obliv::choice is_filler() const noexcept { return has_flag(flags, block_flag::filler); }

    [[nodiscard]] sn::obliv::choice is_op_read() const noexcept { return has_flag(flags, block_flag::op_read); }

    [[nodiscard]] sn::obliv::choice is_op_write() const noexcept { return has_flag(flags, block_flag::op_write); }

    // set flags based on conditions (constant time)
    void set_flags(sn::obliv::choice is_real_block, sn::obliv::choice is_read, sn::obliv::choice is_write) noexcept {
#if defined(ORAM_DEBUG)
      if (is_real_block.unwrap()) {
        sn::util::log::ensure(
            !(is_read.unwrap() && is_write.unwrap()), "o2th_rwkv: real blocks cannot have both read and write ops"
        );
      } else {
        sn::util::log::ensure(
            !is_read.unwrap() && !is_write.unwrap(), "o2th_rwkv: non-real blocks cannot have read/write ops"
        );
      }
#endif
      std::uint8_t acc = sn::obliv::ct_select<std::uint8_t>(
          flag_mask(block_flag::real), flag_mask(block_flag::dummy), is_real_block.unwrap()
      );
      acc = set_flag_cond(acc, block_flag::op_read, is_read);
      acc = set_flag_cond(acc, block_flag::op_write, is_write);
      flags = acc;
    }

    static constexpr key_type invalid_key_value() noexcept {
      return static_cast<key_type>(static_cast<std::make_signed_t<key_type>>(-1));
    }
  };

  static_assert(
      offsetof(op_block, data) % alignof(std::uint64_t) == 0, "o2th_rwkv: op_block::data must be 64-bit aligned"
  );

  static constexpr key_type invalid_key_value() noexcept { return op_block::invalid_key_value(); }
};

template <typename Key, std::size_t BlockSize> using types = table_types<Key, BlockSize>;

} // namespace sn::omap::o2th
