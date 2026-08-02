#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <string>

#include "sonic/util/demo/block_size_dispatch.hpp"
#include "sonic/util/log.hpp"

namespace sn::scooby::omap {

template <std::size_t PayloadBytes> struct payload_tag {
  static constexpr std::size_t value = PayloadBytes;
};

using supported_payload_sizes = sn::util::demo::block_size_list<64>;

inline bool payload_supported(std::size_t bytes) noexcept {
  return sn::util::demo::is_supported_size(supported_payload_sizes{}, bytes);
}

inline std::string format_supported_payload_sizes() {
  return sn::util::demo::format_supported_sizes(supported_payload_sizes::values);
}

template <typename Fn> auto dispatch_payload(std::size_t bytes, Fn&& fn) {
  auto fail = [&](void) -> void {
    const auto supported = format_supported_payload_sizes();
    sn::util::log::failf("scooby-omap: unsupported payload size=%zu (supported %s)", bytes, supported.c_str());
    throw std::logic_error("scooby-omap: unsupported payload size");
  };

  using result_type = decltype(std::forward<Fn>(fn)(payload_tag<64>{}));
  if constexpr (std::is_void_v<result_type>) {
    const bool dispatched = sn::util::demo::dispatch_block_size<supported_payload_sizes>(bytes, [&](auto size_tag) {
      std::forward<Fn>(fn)(payload_tag<size_tag.value>{});
    });
    if (!dispatched) {
      fail();
    }
    return;
  } else {
    using stored_type = std::remove_cvref_t<result_type>;
    std::optional<stored_type> result{};
    const bool dispatched = sn::util::demo::dispatch_block_size<supported_payload_sizes>(bytes, [&](auto size_tag) {
      result.emplace(std::forward<Fn>(fn)(payload_tag<size_tag.value>{}));
    });
    if (!dispatched) {
      fail();
    }
    return std::move(*result);
  }
}

}
