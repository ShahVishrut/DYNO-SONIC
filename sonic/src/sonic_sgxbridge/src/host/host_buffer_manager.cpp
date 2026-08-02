#include "sonic/sgxbridge/host/host_buffer_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace sn::sgxbridge::hostbuf {

namespace {
bool exceeds_limit(std::size_t amount, std::size_t limit) { return limit != 0 && amount > limit; }
}

host_buffer_manager::host_buffer_manager() = default;

host_buffer_manager::host_buffer_manager(config cfg) : config_(cfg) {}

void host_buffer_manager::configure(config cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = cfg;
}

result host_buffer_manager::insert_locked(std::vector<std::uint8_t>&& storage, descriptor& out) {
  const std::size_t bytes = storage.size();
  if (exceeds_limit(bytes, config_.max_single_allocation)) {
    return {status::limit_reached, static_cast<std::uint32_t>(bytes)};
  }
  if (exceeds_limit(total_bytes_ + bytes, config_.max_total_bytes)) {
    return {status::limit_reached, static_cast<std::uint32_t>(bytes)};
  }

  const buffer_id id = next_id_++;
  record slot{};
  slot.storage = std::move(storage);
  slot.desc.id = id;
  slot.desc.data = slot.storage.data();
  slot.desc.size = slot.storage.size();
  slot.refs = 1;
  slot.external = false;
  records_[id] = std::move(slot);
  out = records_[id].desc;
  total_bytes_ += records_[id].desc.size;
  return result::ok();
}

result host_buffer_manager::insert_external_locked(
    std::uint8_t* data, std::size_t size, std::function<void()> on_release, descriptor& out
) {
  if (data == nullptr && size != 0) {
    return {status::invalid_arguments, 0};
  }
  if (exceeds_limit(size, config_.max_single_allocation)) {
    return {status::limit_reached, static_cast<std::uint32_t>(size)};
  }
  if (exceeds_limit(total_bytes_ + size, config_.max_total_bytes)) {
    return {status::limit_reached, static_cast<std::uint32_t>(size)};
  }

  const buffer_id id = next_id_++;
  record slot{};
  slot.desc.id = id;
  slot.desc.data = data;
  slot.desc.size = size;
  slot.refs = 1;
  slot.external = true;
  slot.release_cb = std::move(on_release);
  records_[id] = std::move(slot);
  out = records_[id].desc;
  total_bytes_ += out.size;
  return result::ok();
}

result host_buffer_manager::allocate(std::size_t bytes, descriptor& out) {
  std::vector<std::uint8_t> storage;
  storage.resize(bytes);
  std::lock_guard<std::mutex> lock(mutex_);
  return insert_locked(std::move(storage), out);
}

result host_buffer_manager::adopt(std::vector<std::uint8_t>&& payload, descriptor& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  return insert_locked(std::move(payload), out);
}

result host_buffer_manager::wrap_external(
    std::uint8_t* data, std::size_t size, std::function<void()> on_release, descriptor& out
) {
  std::lock_guard<std::mutex> lock(mutex_);
  return insert_external_locked(data, size, std::move(on_release), out);
}

result host_buffer_manager::view(buffer_id id, descriptor& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(id);
  if (it == records_.end()) {
    return {status::not_found, 0};
  }
  out = it->second.desc;
  return result::ok();
}

result host_buffer_manager::pin(buffer_id id, descriptor& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(id);
  if (it == records_.end()) {
    return {status::not_found, 0};
  }
  auto& slot = it->second;
  if (slot.refs == std::numeric_limits<std::uint32_t>::max()) {
    return {status::limit_reached, 0};
  }
  ++slot.refs;
  out = slot.desc;
  return result::ok();
}

status host_buffer_manager::release(buffer_id id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(id);
  if (it == records_.end()) {
    return status::not_found;
  }
  auto& slot = it->second;
  if (slot.refs == 0) {
    return status::internal_error;
  }
  --slot.refs;
  if (slot.refs == 0) {
    total_bytes_ -= slot.desc.size;
    if (slot.external && slot.release_cb) {
      try {
        slot.release_cb();
      } catch (...) {
        std::abort();
      }
    }
    records_.erase(it);
  }
  return status::ok;
}

status host_buffer_manager::retain(buffer_id id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(id);
  if (it == records_.end()) {
    return status::not_found;
  }
  if (it->second.refs == std::numeric_limits<std::uint32_t>::max()) {
    return status::limit_reached;
  }
  ++it->second.refs;
  return status::ok;
}

void host_buffer_manager::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : records_) {
    auto& slot = entry.second;
    if (slot.refs > 0 && slot.external && slot.release_cb) {
      try {
        slot.release_cb();
      } catch (...) {
        std::abort();
      }
    }
  }
  records_.clear();
  total_bytes_ = 0;
  next_id_ = 1;
}

}
