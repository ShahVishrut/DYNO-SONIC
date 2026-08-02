#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/detail/bitonic_common.hpp"
#include "sonic/sortshuffle/detail/util.hpp"

namespace sn {
namespace sortshuffle {
namespace ser {
namespace smallarray {

namespace detail {

using ::sn::sortshuffle::detail::default_key;
using ::sn::sortshuffle::detail::key_result_t;
using ::sn::sortshuffle::detail::swap_element;
using ::sn::sortshuffle::detail::bitonic::bitonic_adjust_lane;
using ::sn::sortshuffle::detail::bitonic::bitonic_network_ops;
using ::sn::sortshuffle::detail::bitonic::bitonic_should_swap;

template <typename T> inline void swap_payload(T* left, T* right, bool cond) { swap_element(left, right, cond); }

template <std::size_t N, typename T, typename KeyExtractor, typename Compare>
inline void bitonic_sort_dispatch(T* data, bool sort_ascending, KeyExtractor& key, Compare& comp) {
  static_assert((N & (N - 1)) == 0, "bitonic_sort_dispatch requires power-of-two input size");
  static_assert(N >= 2, "bitonic_sort_dispatch expects N >= 2");

  for (const auto& op : bitonic_network_ops<N>::table) {
    T* left = data + op.left;
    T* right = data + op.right;
    const auto left_key = key(*left);
    const auto right_key = key(*right);
    const bool lane_ascending = bitonic_adjust_lane(sort_ascending, op.ascending);
    const bool should_swap = bitonic_should_swap(left_key, right_key, lane_ascending, comp);

    swap_payload(left, right, should_swap);
  }
}

}

template <
    typename T, typename KeyExtractor = detail::default_key<T>,
    typename Compare = std::less<detail::key_result_t<KeyExtractor, T>>>
inline bool bitonic_sort_small(
    T* data, std::size_t count, bool sort_ascending = true, KeyExtractor key = {}, Compare comp = {}
) {
  if (count <= 1) {
    return true;
  }

  KeyExtractor key_extractor = key;
  Compare comparator = comp;

  switch (count) {
  case 2:
    detail::bitonic_sort_dispatch<2>(data, sort_ascending, key_extractor, comparator);
    return true;
  case 4:
    detail::bitonic_sort_dispatch<4>(data, sort_ascending, key_extractor, comparator);
    return true;
  case 8:
    detail::bitonic_sort_dispatch<8>(data, sort_ascending, key_extractor, comparator);
    return true;
  case 16:
    detail::bitonic_sort_dispatch<16>(data, sort_ascending, key_extractor, comparator);
    return true;
  case 32:
    detail::bitonic_sort_dispatch<32>(data, sort_ascending, key_extractor, comparator);
    return true;
  default:
    return false;
  }
}

}
}
}
}
