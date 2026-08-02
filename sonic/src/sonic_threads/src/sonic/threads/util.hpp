#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace sn::threads {

inline std::pair<std::size_t, std::size_t> partition_evenly(
    std::size_t logical_index, std::size_t total_items, std::size_t partitions
) noexcept {
  if (partitions == 0 || logical_index >= partitions) {
    return {0, 0};
  }

#if defined(__SIZEOF_INT128__)
  const auto scaled_begin = static_cast<unsigned __int128>(logical_index) * static_cast<unsigned __int128>(total_items);
  const auto scaled_end =
      static_cast<unsigned __int128>(logical_index + 1) * static_cast<unsigned __int128>(total_items);
  const std::size_t chunk_begin = static_cast<std::size_t>(scaled_begin / partitions);
  const std::size_t chunk_end = static_cast<std::size_t>(scaled_end / partitions);
#else
  const std::size_t chunk_begin = (logical_index * total_items) / partitions;
  const std::size_t chunk_end = ((logical_index + 1) * total_items) / partitions;
#endif

  return {chunk_begin, chunk_end};
}

}
