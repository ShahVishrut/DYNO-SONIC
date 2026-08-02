#pragma once

#include <memory>
#include <type_traits>
#include <cstring>

#if defined(__cpp_lib_bit_cast)
#include <bit>
#else

namespace std {
template <class T2, class T1> T2 bit_cast(T1 t1) {

  static_assert(sizeof(T1) == sizeof(T2), "T1 and T2 must have the same size");

  static_assert(
      std::is_trivial_v<T1> && std::is_standard_layout_v<T1>, "T1 must be trivially copyable with standard layout"
  );
  static_assert(
      std::is_trivial_v<T2> && std::is_standard_layout_v<T2>, "T2 must be trivially copyable with standard layout"
  );

  T2 t2;
  std::memcpy(std::addressof(t2), std::addressof(t1), sizeof(T1));
  return t2;
}
}

#endif
