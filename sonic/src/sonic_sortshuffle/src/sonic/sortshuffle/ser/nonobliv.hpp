#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>

#include "sonic/sortshuffle/detail/util.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn {
namespace sortshuffle {
namespace ser {
namespace nonobliv {

namespace detail {

using ::sn::sortshuffle::detail::default_key;
using ::sn::sortshuffle::detail::key_result_t;

template <typename T, typename KeyExtractor, typename Compare> class key_compare {
public:
  using key_type = key_result_t<KeyExtractor, T>;

  key_compare(KeyExtractor key_extractor, Compare compare) : key_extractor_(key_extractor), compare_(compare) {}

  inline bool operator()(const T& lhs, const T& rhs) const {
    return compare_(key_extractor_(lhs), key_extractor_(rhs));
  }

private:
  KeyExtractor key_extractor_;
  Compare compare_;
};

template <typename T, typename Compare> inline void insertion_sort(T* data, std::size_t count, const Compare& compare) {
  for (std::size_t i = 1; i < count; ++i) {
    T value = data[i];
    std::size_t pos = i;
    while (pos > 0 && compare(value, data[pos - 1])) {
      data[pos] = data[pos - 1];
      --pos;
    }
    data[pos] = value;
  }
}

template <typename T, typename Compare>
inline void sift_down(T* data, std::size_t start, std::size_t count, const Compare& compare) {
  std::size_t root = start;
  while (true) {
    std::size_t left = root * 2 + 1;
    if (left >= count) {
      break;
    }
    std::size_t right = left + 1;
    std::size_t swap_idx = root;

    if (compare(data[swap_idx], data[left])) {
      swap_idx = left;
    }
    if (right < count && compare(data[swap_idx], data[right])) {
      swap_idx = right;
    }
    if (swap_idx == root) {
      return;
    }
    std::swap(data[root], data[swap_idx]);
    root = swap_idx;
  }
}

template <typename T, typename Compare> inline void heap_sort(T* data, std::size_t count, const Compare& compare) {
  if (count < 2) {
    return;
  }

  for (std::size_t i = (count / 2); i > 0; --i) {
    sift_down(data, i - 1, count, compare);
  }

  for (std::size_t end = count - 1; end > 0; --end) {
    std::swap(data[0], data[end]);
    sift_down(data, std::size_t{0}, end, compare);
  }
}

template <typename T, typename Compare> inline T* partition(T* begin, T* end, const Compare& compare) {
  T* pivot = end - 1;
  T* store = begin;
  for (T* iter = begin; iter < pivot; ++iter) {
    if (compare(*iter, *pivot)) {
      std::swap(*iter, *store);
      ++store;
    }
  }
  std::swap(*store, *pivot);
  return store;
}

template <typename T, typename Compare>
inline void introsort_loop(T* begin, T* end, std::size_t depth_limit, const Compare& compare) {
  constexpr std::size_t kInsertionSortThreshold = 24;
  while (static_cast<std::size_t>(end - begin) > kInsertionSortThreshold) {
    if (depth_limit == 0) {
      heap_sort(begin, static_cast<std::size_t>(end - begin), compare);
      return;
    }
    --depth_limit;

    T* mid = begin + (end - begin) / 2;
    T* last = end - 1;
    if (compare(*mid, *begin)) {
      std::swap(*mid, *begin);
    }
    if (compare(*last, *begin)) {
      std::swap(*last, *begin);
    }
    if (compare(*last, *mid)) {
      std::swap(*last, *mid);
    }
    std::swap(*mid, *(end - 1));

    T* cut = partition(begin, end, compare);
    introsort_loop(cut + 1, end, depth_limit, compare);
    end = cut;
  }
}

template <typename T, typename Compare> inline void introsort(T* data, std::size_t count, const Compare& compare) {
  if (count <= 1) {
    return;
  }
  const std::size_t depth_limit = static_cast<std::size_t>(std::floor(std::log2(static_cast<double>(count)))) * 2;
  introsort_loop(data, data + count, depth_limit, compare);
  insertion_sort(data, count, compare);
}

template <typename T, typename KeyExtractor, typename Compare>
inline void introsort_with_keys(T* data, std::size_t count, KeyExtractor key_extractor, Compare compare) {
  using comparator = key_compare<T, KeyExtractor, Compare>;
  comparator cmp(key_extractor, compare);
  introsort(data, count, cmp);
}

template <typename T, typename KeyExtractor>
inline void radix_sort_impl(T* data, std::size_t count, KeyExtractor key_extractor) {
  using key_type = key_result_t<KeyExtractor, T>;
  static_assert(std::is_integral_v<key_type>, "radix_sort requires integral keys");

  using unsigned_key = std::make_unsigned_t<key_type>;
  std::vector<T> buffer(count);
  std::array<std::size_t, 256> buckets{};

  constexpr std::size_t byte_count = sizeof(unsigned_key);
  constexpr std::size_t radix = 256;

  auto transform_key = [](key_type value) -> unsigned_key {
    auto converted = static_cast<unsigned_key>(value);
    if constexpr (std::is_signed_v<key_type>) {
      constexpr unsigned_key sign_bit = unsigned_key{1} << ((sizeof(unsigned_key) * 8) - 1);
      converted ^= sign_bit;
    }
    return converted;
  };

  for (std::size_t byte_index = 0; byte_index < byte_count; ++byte_index) {
    std::fill(buckets.begin(), buckets.end(), 0);

    for (std::size_t i = 0; i < count; ++i) {
      const unsigned_key key = transform_key(key_extractor(data[i]));
      const std::size_t bucket = (key >> (byte_index * 8)) & 0xFF;
      ++buckets[bucket];
    }

    std::size_t sum = 0;
    for (std::size_t bucket = 0; bucket < radix; ++bucket) {
      const std::size_t count_bucket = buckets[bucket];
      buckets[bucket] = sum;
      sum += count_bucket;
    }

    for (std::size_t i = 0; i < count; ++i) {
      const unsigned_key key = transform_key(key_extractor(data[i]));
      const std::size_t bucket = (key >> (byte_index * 8)) & 0xFF;
      buffer[buckets[bucket]++] = data[i];
    }

    sn::obliv::copy(buffer.begin(), buffer.begin() + count, data);
  }
}

}

template <
    typename T, typename KeyExtractor = detail::default_key<T>,
    typename Compare = std::less<detail::key_result_t<KeyExtractor, T>>>
inline void introsort(
    T* data, std::size_t count, KeyExtractor key_extractor = KeyExtractor{}, Compare compare = Compare{}
) {
  detail::introsort_with_keys(data, count, key_extractor, compare);
}

template <typename T, typename KeyExtractor = detail::default_key<T>>
inline void radix_sort(T* data, std::size_t count, KeyExtractor key_extractor = KeyExtractor{}) {
  detail::radix_sort_impl(data, count, key_extractor);
}

}
}
}
}
