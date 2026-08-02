#pragma once

namespace sn::omap::util {

template <typename T> struct alignas(16) maybe_dummy {
  T value{};
  bool is_dummy = true;
};

} // namespace sn::omap::util
