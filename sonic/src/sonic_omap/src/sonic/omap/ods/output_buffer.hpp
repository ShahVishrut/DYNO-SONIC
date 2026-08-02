#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/omap/util/maybe_dummy.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::ods {

// fixed-size output buffer
// - buffer stores maybe_dummy<T> items, initialized to all dummy
// - push_if(emit, value) writes value into the first dummy slot if emit is true
template <typename T> class output_buffer {
public:
  using item_type = sn::omap::util::maybe_dummy<T>;
  static_assert(std::is_trivially_copyable_v<T>, "ods::output_buffer requires trivially copyable value type");
  static_assert(std::is_trivially_copyable_v<item_type>, "ods::output_buffer: item_type must be trivially copyable");

  void configure(std::size_t n) { out_.assign(n, item_type{.value = T{}, .is_dummy = true}); }

  [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

  void clear() {
    for (auto& slot : out_) {
      slot = item_type{.value = T{}, .is_dummy = true};
    }
  }

  // conditionally push a value into the first dummy slot
  void push_if(const sn::obliv::choice& emit, const T& value) noexcept {
    sn::obliv::choice placed(false);
    const item_type candidate{.value = value, .is_dummy = false};

    for (std::size_t i = 0; i < out_.size(); ++i) {
      const sn::obliv::choice slot_is_dummy(out_[i].is_dummy);
      const sn::obliv::choice should_place = emit && !placed && slot_is_dummy;
      sn::obliv::ct_set_data(&out_[i], candidate, should_place.unwrap());
      placed = placed || should_place;
    }
  }

  [[nodiscard]] sn::util::span<item_type> span() noexcept {
    return sn::util::span<item_type>(out_.data(), out_.size());
  }
  [[nodiscard]] sn::util::span<const item_type> span() const noexcept {
    return sn::util::span<const item_type>(out_.data(), out_.size());
  }

private:
  std::vector<item_type> out_{};
};

} // namespace sn::omap::ods
