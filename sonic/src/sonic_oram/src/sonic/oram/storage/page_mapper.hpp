#pragma once

#include <cstdint>

namespace sn::oram::zingoram::storage {

// maps bucket node_id to page_id and bucket offset within page
class consecutive_page_mapper {
public:
  explicit constexpr consecutive_page_mapper(std::uint32_t buckets_per_page = 1) noexcept :
      buckets_per_page_(buckets_per_page ? buckets_per_page : 1) {}

  [[nodiscard]] constexpr std::uint64_t page_id(std::uint64_t node_id) const noexcept {
    return node_id / static_cast<std::uint64_t>(buckets_per_page_);
  }

  [[nodiscard]] constexpr std::uint32_t bucket_offset(std::uint64_t node_id) const noexcept {
    return static_cast<std::uint32_t>(node_id % buckets_per_page_);
  }

  [[nodiscard]] constexpr std::uint32_t buckets_per_page() const noexcept { return buckets_per_page_; }

private:
  std::uint32_t buckets_per_page_;
};

// pack complete subtrees in "triangles"; of depth levels_per_pack into one page
class triangle_page_mapper {
public:
  using node_id_t = std::uint64_t;
  using page_id_t = std::uint64_t;
  using offset_t = std::uint32_t;

  constexpr triangle_page_mapper() noexcept = default;

  // cold_start_level = first cold level (equals n_hot_levels)
  constexpr triangle_page_mapper(std::uint32_t levels_per_pack, std::uint32_t cold_start_level) noexcept :
      levels_per_pack_(levels_per_pack ? levels_per_pack : 1), cold_start_level_(cold_start_level) {}

  [[nodiscard]] constexpr std::uint32_t levels_per_pack() const noexcept { return levels_per_pack_; }
  [[nodiscard]] constexpr std::uint32_t cold_start_level() const noexcept { return cold_start_level_; }

  // buckets per page (2^L - 1)
  [[nodiscard]] constexpr std::uint32_t buckets_per_page() const noexcept {
    return (std::uint32_t(1) << levels_per_pack_) - 1U;
  }

  // page that holds node_id (only for cold nodes: level >= cold_root_level)
  [[nodiscard]] constexpr page_id_t page_id(node_id_t node_id) const noexcept {
    const std::uint32_t level = floor_log2(node_id);
    sn::util::log::ensure(level >= cold_start_level_, "triangle_page_mapper::page_id: node_id is not cold");

    const node_id_t level_first = node_id_t(1) << level;
    const node_id_t idx_in_level = node_id - level_first;

    const std::uint32_t cold_depth = level - cold_start_level_;
    const std::uint32_t tri_level = cold_depth / levels_per_pack_;
    const std::uint32_t depth_in_tri = cold_depth % levels_per_pack_;

    const node_id_t stride = node_id_t(1) << depth_in_tri;
    const node_id_t root_index = idx_in_level / stride;

    node_id_t triangles_before = 0;
    node_id_t triangles_this = node_id_t(1) << cold_start_level_; // 2^S
    for (std::uint32_t r = 0; r < tri_level; ++r) {
      triangles_before += triangles_this;
      triangles_this <<= levels_per_pack_;
    }

    return triangles_before + root_index;
  }

  // offset of node_id within its page (0-based)
  [[nodiscard]] constexpr offset_t bucket_offset(node_id_t node_id) const noexcept {
    const std::uint32_t level = floor_log2(node_id);
    if (level < cold_start_level_) {
      return 0;
    }

    const node_id_t level_first = node_id_t(1) << level;
    const node_id_t idx_in_level = node_id - level_first;

    // depth in the cold region
    const std::uint32_t cold_depth = level - cold_start_level_;
    // depth within the triangle
    const std::uint32_t depth_in_tri = cold_depth % levels_per_pack_;
    // stride at this triangle depth
    const node_id_t stride = node_id_t(1) << depth_in_tri; // 2^d

    const node_id_t local_level_first = node_id_t(1) << depth_in_tri;
    const node_id_t idx_within_stride = idx_in_level % stride;
    const node_id_t local_id = local_level_first + idx_within_stride; // 1-based inside triangle
    return static_cast<offset_t>(local_id - 1U);
  }

private:
  static constexpr std::uint32_t floor_log2(std::uint64_t x) noexcept {
    std::uint32_t r = 0;
    while (x > 1U) {
      x >>= 1U;
      ++r;
    }
    return r;
  }

  std::uint32_t levels_per_pack_ = 1;
  std::uint32_t cold_start_level_ = 0;
};

} // namespace sn::oram::zingoram::storage
