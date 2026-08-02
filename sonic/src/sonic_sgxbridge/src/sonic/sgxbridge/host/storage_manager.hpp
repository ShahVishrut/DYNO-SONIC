#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>

#include "sonic/storage/io/posix_file_backend.hpp"
#include "sonic/util/log.hpp"
#include "sonic/threads/sync.hpp"

namespace sn::sgxbridge::storage {

enum class status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  not_found = 2,
  io_error = 3,
};

struct result {
  status code{status::io_error};
  std::uint32_t detail{0};

  [[nodiscard]] bool succeeded() const noexcept { return code == status::ok; }
  static constexpr result ok() noexcept { return result{status::ok, 0}; }
};

using handle_t = std::uint64_t;

struct open_config {
  std::string data_path;
  std::string meta_path;
  bool create{true};
  bool truncate{true};
};

struct page_batch_view {
  std::uint64_t page_id{0};
  const void* data{nullptr};
  const void* meta{nullptr};
};

class manager {
public:
  manager() = default;
  ~manager() = default;

  result open(open_config cfg, handle_t& out_handle);
  status close(handle_t handle);

  result resize(handle_t handle, std::uint64_t pages, std::size_t data_bytes, std::size_t meta_bytes);
  result read_data(handle_t handle, std::uint64_t page_id, void* dst, std::size_t bytes);
  result write_data(handle_t handle, std::uint64_t page_id, const void* src, std::size_t bytes);
  result read_meta(handle_t handle, std::uint64_t page_id, void* dst, std::size_t bytes);
  result write_meta(handle_t handle, std::uint64_t page_id, const void* src, std::size_t bytes);
  result write_pages(
      handle_t handle, const page_batch_view* pages, std::size_t count, std::size_t data_bytes, std::size_t meta_bytes
  );
  status flush(handle_t handle);

  void shutdown();

private:
  struct entry {
    sn::storage::io::posix_file_backend data;
    sn::storage::io::posix_file_backend meta;
  };

  std::shared_ptr<entry> lookup(handle_t handle);
  result read_impl(const std::shared_ptr<entry>& e, std::uint64_t page_id, void* dst, std::size_t bytes, bool meta);
  result write_impl(
      const std::shared_ptr<entry>& e, std::uint64_t page_id, const void* src, std::size_t bytes, bool meta
  );

  sn::threads::mutex mutex_{};
  std::unordered_map<handle_t, std::shared_ptr<entry>> entries_{};
  handle_t next_handle_{1};
};

}
