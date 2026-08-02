#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"

namespace sn::omap::ods {

template <typename T> struct worklist_pop {
  T item{};
  sn::obliv::choice valid = sn::obliv::choice(false);
};

// fixed-capacity oblivious worklist with full-scan push/pop
// - push_if(cond, item) places item into the first invalid slot if cond is true
// - pop_first() returns the first valid slot and clears it
template <typename T> class worklist {
public:
  static_assert(std::is_trivially_copyable_v<T>, "ods::worklist requires trivially copyable item type");

  void configure(std::size_t cap) {
    items_.assign(cap, T{});
    valid_.assign(cap, 0);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return items_.size(); }

  void clear() { std::fill(valid_.begin(), valid_.end(), static_cast<std::uint8_t>(0)); }

  void push_if(const sn::obliv::choice& cond, const T& item) noexcept {
    sn::obliv::choice placed(false);
    const T local = item;

    for (std::size_t i = 0; i < items_.size(); ++i) {
      const sn::obliv::choice slot_invalid(valid_[i] == 0);
      // if cond, and we haven't already placed the item, and this slot is invalid, then place the item here
      const sn::obliv::choice should_place = cond && !placed && slot_invalid;

      // conditionally set slot
      sn::obliv::ct_set_data(&items_[i], local, should_place.unwrap());
      // set validity bit
      valid_[i] = sn::obliv::ct_select<std::uint8_t>(static_cast<std::uint8_t>(1), valid_[i], should_place.unwrap());
      placed = placed || should_place;
    }
  }

  [[nodiscard]] worklist_pop<T> pop_first() noexcept {
    worklist_pop<T> out{};
    sn::obliv::choice found(false);

    for (std::size_t i = 0; i < items_.size(); ++i) {
      const sn::obliv::choice slot_valid(valid_[i] != 0);
      // if we haven't already found an item, and this slot is valid, then take this item
      const sn::obliv::choice take = !found && slot_valid;

      // conditionally set output item
      sn::obliv::ct_set_data(&out.item, items_[i], take.unwrap());
      // clear validity bit
      valid_[i] = sn::obliv::ct_select<std::uint8_t>(static_cast<std::uint8_t>(0), valid_[i], take.unwrap());

      // clear slot
      const T zero{};
      sn::obliv::ct_set_data(&items_[i], zero, take.unwrap());

      found = found || take;
    }

    out.valid = found;
    return out;
  }

private:
  std::vector<T> items_{};
  std::vector<std::uint8_t> valid_{};
};

} // namespace sn::omap::ods
