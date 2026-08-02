#pragma once

#include <cstdint>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"

namespace sn::omap::ods {

// logical dummy address for oram (`sn::oram::tree::block<...>::dummy_address`)
static constexpr std::int64_t dummy_address = -1;

struct alignas(16) ptr {
  std::int64_t addr = dummy_address;
  std::uint64_t leaf = 0;
};

// a pending pointer stores the leaf we must read (`cur_leaf`)
// and the new leaf we will remap the block to (`new_leaf`)
struct alignas(16) pending_ptr {
  std::int64_t addr = dummy_address;
  std::uint64_t cur_leaf = 0;
  std::uint64_t new_leaf = 0;
};

// convert a persistent ptr (addr, leaf) to a pending access (addr, cur_leaf, new_leaf)
[[nodiscard]] inline pending_ptr make_pending(const ptr& p, std::uint64_t new_leaf) noexcept {
  pending_ptr out{};
  out.addr = p.addr;
  out.cur_leaf = p.leaf;
  out.new_leaf = new_leaf;
  return out;
}

// update a persistent ptr's leaf after oram access remaps it to new leaf
inline void update_ptr_leaf_after_access(ptr& p, std::uint64_t new_leaf, sn::obliv::choice did_access) noexcept {
  p.leaf = sn::obliv::ct_select<std::uint64_t>(new_leaf, p.leaf, did_access.unwrap());
}

[[nodiscard]] inline sn::obliv::choice is_real(const ptr& p) noexcept { return sn::obliv::choice(p.addr >= 0); }
[[nodiscard]] inline sn::obliv::choice is_dummy(const ptr& p) noexcept { return !is_real(p); }
[[nodiscard]] inline sn::obliv::choice is_real(const pending_ptr& p) noexcept { return sn::obliv::choice(p.addr >= 0); }
[[nodiscard]] inline sn::obliv::choice is_dummy(const pending_ptr& p) noexcept { return !is_real(p); }

[[nodiscard]] inline ptr ct_select_ptr(ptr a, ptr b, sn::obliv::choice cond) noexcept {
  ptr out{};
  out.addr = sn::obliv::ct_select<std::int64_t>(a.addr, b.addr, cond.unwrap());
  out.leaf = sn::obliv::ct_select<std::uint64_t>(a.leaf, b.leaf, cond.unwrap());
  return out;
}

[[nodiscard]] inline pending_ptr ct_select_pending(pending_ptr a, pending_ptr b, sn::obliv::choice cond) noexcept {
  return sn::obliv::ct_select_data<pending_ptr>(a, b, cond.unwrap());
}

static_assert(std::is_trivially_copyable_v<ptr>, "sn::omap::ods: ptr must be trivially copyable");
static_assert(std::is_trivially_copyable_v<pending_ptr>, "sn::omap::ods: pending_ptr must be trivially copyable");

} // namespace sn::omap::ods
