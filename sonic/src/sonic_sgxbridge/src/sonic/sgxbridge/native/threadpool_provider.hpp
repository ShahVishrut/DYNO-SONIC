#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/threads/tuning.hpp"

namespace sn::sgxbridge::native {

struct pthread_threadpool_state {
  std::mutex mutex;
  std::uint64_t next_id{1};
  std::unordered_map<std::uint64_t, std::unique_ptr<sn::threads::pthread_thread_pool>> pools;
  sn::threads::thread_context threads{};
};

inline tp::result pthread_acquire_threadpool(void* context_ptr, const tp::request& request, tp::descriptor& desc) {
  desc = {};
  if (context_ptr == nullptr) {
    return {tp::status::invalid_arguments, 0};
  }

  auto* state = static_cast<pthread_threadpool_state*>(context_ptr);
  std::unique_ptr<sn::threads::pthread_thread_pool> guard;
  try {
    guard = std::make_unique<sn::threads::pthread_thread_pool>(state->threads, request.workers.value, "native-tp");
  } catch (...) {
    return {tp::status::internal_error, 0};
  }

  sn::threads::thread_pool* pool_ptr = &guard->pool();
  std::uint64_t pool_id = 0;

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    pool_id = state->next_id++;
    try {
      auto inserted = state->pools.emplace(pool_id, std::move(guard));
      if (!inserted.second) {
        return {tp::status::internal_error, 1};
      }
    } catch (...) {
      return {tp::status::internal_error, 2};
    }
  }

  desc.id = pool_id;
  desc.pool = pool_ptr;
  return tp::result::ok();
}

inline void pthread_release_threadpool(void* context_ptr, tp::threadpool_id pool_id) {
  if (context_ptr == nullptr || pool_id == 0) {
    return;
  }

  auto* state = static_cast<pthread_threadpool_state*>(context_ptr);

  std::unique_ptr<sn::threads::pthread_thread_pool> guard;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    auto it = state->pools.find(pool_id);
    if (it == state->pools.end()) {
      return;
    }
    guard = std::move(it->second);
    state->pools.erase(it);
  }
}

inline tp::provider make_pthread_threadpool_provider(pthread_threadpool_state& state) {
  tp::provider prov{};
  prov.context = &state;
  prov.acquire = &pthread_acquire_threadpool;
  prov.release = &pthread_release_threadpool;
  return prov;
}

}
