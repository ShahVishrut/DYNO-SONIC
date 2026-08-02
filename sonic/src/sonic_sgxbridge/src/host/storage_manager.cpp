#include "sonic/sgxbridge/host/storage_manager.hpp"

namespace sn::sgxbridge::storage {

result manager::open(open_config cfg, handle_t& out_handle) {
  static sn::util::log::logger log(sn::util::log::level::info);
  if (cfg.data_path.empty() || cfg.meta_path.empty()) {
    return {status::invalid_arguments, 0};
  }

  sn::storage::io::posix_file_config data_cfg{};
  data_cfg.path = std::move(cfg.data_path);
  data_cfg.create = cfg.create;
  data_cfg.truncate = cfg.truncate;
  data_cfg.drop_cache_after_flush = false;

  sn::storage::io::posix_file_config meta_cfg{};
  meta_cfg.path = std::move(cfg.meta_path);
  meta_cfg.create = cfg.create;
  meta_cfg.truncate = cfg.truncate;
  meta_cfg.drop_cache_after_flush = false;

  try {
    auto e = std::make_shared<entry>(entry{
        sn::storage::io::posix_file_backend(std::move(data_cfg)),
        sn::storage::io::posix_file_backend(std::move(meta_cfg))
    });
    sn::threads::unique_lock lock(mutex_);
    const handle_t handle = next_handle_++;
    entries_.emplace(handle, std::move(e));
    out_handle = handle;
    return result::ok();
  } catch (const std::exception&) {
    log.err("storage open");
    return {status::io_error, 0};
  } catch (...) {
    log.err("storage open");
    return {status::io_error, 0};
  }
}

status manager::close(handle_t handle) {
  sn::threads::unique_lock lock(mutex_);
  auto it = entries_.find(handle);
  if (it == entries_.end()) {
    return status::not_found;
  }
  entries_.erase(it);
  return status::ok;
}

result manager::resize(handle_t handle, std::uint64_t pages, std::size_t data_bytes, std::size_t meta_bytes) {
  static sn::util::log::logger log(sn::util::log::level::info);
  if (data_bytes == 0 || meta_bytes == 0) {
    return {status::invalid_arguments, 0};
  }
  auto e = lookup(handle);
  if (!e) {
    return {status::not_found, 0};
  }
  try {
    e->data.resize(pages, data_bytes);
    e->meta.resize(pages, meta_bytes);
    return result::ok();
  } catch (const std::exception&) {
    log.err("storage resize");
    return {status::io_error, 0};
  } catch (...) {
    log.err("storage resize");
    return {status::io_error, 0};
  }
}

result manager::read_data(handle_t handle, std::uint64_t page_id, void* dst, std::size_t bytes) {
  auto e = lookup(handle);
  if (!e) {
    return {status::not_found, 0};
  }
  return read_impl(e, page_id, dst, bytes, false);
}

result manager::write_data(handle_t handle, std::uint64_t page_id, const void* src, std::size_t bytes) {
  auto e = lookup(handle);
  if (!e) {
    return {status::not_found, 0};
  }
  return write_impl(e, page_id, src, bytes, false);
}

result manager::read_meta(handle_t handle, std::uint64_t page_id, void* dst, std::size_t bytes) {
  auto e = lookup(handle);
  if (!e) {
    return {status::not_found, 0};
  }
  return read_impl(e, page_id, dst, bytes, true);
}

result manager::write_meta(handle_t handle, std::uint64_t page_id, const void* src, std::size_t bytes) {
  auto e = lookup(handle);
  if (!e) {
    return {status::not_found, 0};
  }
  return write_impl(e, page_id, src, bytes, true);
}

result manager::write_pages(
    handle_t handle, const page_batch_view* pages, std::size_t count, std::size_t data_bytes, std::size_t meta_bytes
) {
  if (pages == nullptr || count == 0 || data_bytes == 0 || meta_bytes == 0) {
    return {status::invalid_arguments, 0};
  }
  auto e = lookup(handle);
  if (!e) {
    return {status::not_found, 0};
  }
  try {
    for (std::size_t idx = 0; idx < count; ++idx) {
      const auto& p = pages[idx];
      if (p.data == nullptr || p.meta == nullptr) {
        return {status::invalid_arguments, 0};
      }
      e->data.write_page(p.page_id, p.data, data_bytes);
      e->meta.write_page(p.page_id, p.meta, meta_bytes);
    }
    return result::ok();
  } catch (...) {
    return {status::io_error, 0};
  }
}

status manager::flush(handle_t handle) {
  auto e = lookup(handle);
  if (!e) {
    return status::not_found;
  }
  try {
    e->data.flush();
    e->meta.flush();
    return status::ok;
  } catch (...) {
    return status::io_error;
  }
}

void manager::shutdown() {
  sn::threads::unique_lock lock(mutex_);
  entries_.clear();
  next_handle_ = 1;
}

std::shared_ptr<manager::entry> manager::lookup(handle_t handle) {
  sn::threads::unique_lock lock(mutex_);
  auto it = entries_.find(handle);
  if (it == entries_.end()) {
    return nullptr;
  }
  return it->second;
}

result manager::read_impl(
    const std::shared_ptr<entry>& e, std::uint64_t page_id, void* dst, std::size_t bytes, bool meta
) {
  if (dst == nullptr || bytes == 0) {
    return {status::invalid_arguments, 0};
  }
  try {
    if (meta) {
      e->meta.read_page(page_id, dst, bytes);
    } else {
      e->data.read_page(page_id, dst, bytes);
    }
    return result::ok();
  } catch (...) {
    return {status::io_error, 0};
  }
}

result manager::write_impl(
    const std::shared_ptr<entry>& e, std::uint64_t page_id, const void* src, std::size_t bytes, bool meta
) {
  if (src == nullptr || bytes == 0) {
    return {status::invalid_arguments, 0};
  }
  try {
    if (meta) {
      e->meta.write_page(page_id, src, bytes);
    } else {
      e->data.write_page(page_id, src, bytes);
    }
    return result::ok();
  } catch (...) {
    return {status::io_error, 0};
  }
}

}
