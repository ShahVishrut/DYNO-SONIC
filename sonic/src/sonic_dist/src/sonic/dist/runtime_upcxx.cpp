
#include "runtime.hpp"

#include <upcxx/upcxx.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sonic/util/log.hpp"
#include "sonic/dist/upcxx_config.hpp"

namespace sn::dist {

namespace {

sn::util::log::logger& dist_logger() {
  static sn::util::log::logger log = sn::util::log::create("sonic.dist");
  return log;
}

struct recv_slot {
  upcxx::global_ptr<std::uint8_t> gptr;
  std::size_t capacity = 0;
  bool in_use = false;
};

struct command {
  enum class kind { send, barrier, allreduce_double } kind = kind::send;

  int dest_rank = -1;
  const std::uint8_t* send_src = nullptr;
  std::size_t send_size = 0;
  std::optional<sn::util::promise<void>> send_promise;

  std::optional<sn::util::promise<void>> barrier_promise;

  double local_value = 0.0;
  std::optional<sn::util::promise<double>> allreduce_promise;
};

struct pending_send {
  int dest_rank = -1;
  int src_rank = -1;
  std::uint32_t slot_id = 0;
  std::size_t size = 0;
  upcxx::future<> rput_future;
  std::optional<sn::util::promise<void>> send_promise;
};

struct slot_allocation {
  upcxx::global_ptr<std::uint8_t> gptr;
  std::uint32_t slot_id = 0;
};

struct runtime_state {
  runtime_config cfg;

  std::thread upc_thread;

  std::atomic<bool> stop{false};
  bool started = false;
  bool startup_error_ready = false;
  std::string startup_error_msg;
  std::mutex start_mutex;
  std::condition_variable start_cv;

  std::atomic<int> my_rank{-1};
  std::atomic<int> n_ranks{0};

  std::mutex cmd_mutex;
  std::condition_variable cmd_cv;
  std::deque<command> commands;

  std::vector<pending_send> pending_sends;

  std::mutex slot_mutex;
  std::vector<recv_slot> local_slots;

  std::mutex recv_mutex;
  std::condition_variable recv_cv;
  std::deque<message_view> incoming;
  bool shutdown_sent = false;

  static runtime_state& instance() {
    static runtime_state inst;
    return inst;
  }
};

runtime_state& rt() { return runtime_state::instance(); }

[[noreturn]] void throw_runtime_shutdown() {
  dist_logger().err("dist shutdown");
  throw std::runtime_error("sn::dist runtime is shutting down");
}

void push_incoming(message_view&& msg) {
  auto& s = rt();
  {
    std::lock_guard<std::mutex> lock(s.recv_mutex);
    if (s.shutdown_sent) {
      dist_logger().wrn("dist drop");
      return;
    }
    s.incoming.push_back(std::move(msg));
    if (s.cfg.max_pending_messages > 0 && s.incoming.size() > s.cfg.max_pending_messages) {
      dist_logger().crt("dist queue");
      std::terminate();
    }
  }
  s.recv_cv.notify_one();
}

void signal_shutdown(runtime_state& s) {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(s.recv_mutex);
    if (s.shutdown_sent) {
      return;
    }
    if (!s.incoming.empty()) {
      dist_logger().wrn("dist finalize");
      s.incoming.clear();
    }
    s.shutdown_sent = true;
    notify = true;
  }
  if (notify) {
    s.recv_cv.notify_all();
  }
}

slot_allocation rpc_acquire_slot(std::size_t size) {
  auto& s = rt();
  std::lock_guard<std::mutex> lock(s.slot_mutex);

  for (std::uint32_t i = 0; i < s.local_slots.size(); ++i) {
    recv_slot& slot = s.local_slots[i];
    if (!slot.in_use && size <= slot.capacity) {
      slot.in_use = true;
      slot_allocation alloc;
      alloc.gptr = slot.gptr;
      alloc.slot_id = i;
      return alloc;
    }
  }

  dist_logger().crt("dist slot");
  std::terminate();
}

void rpc_large_complete(int src_rank, std::uint32_t slot_id, std::size_t size) {
  auto& s = rt();

  recv_slot* slot = nullptr;
  {
    std::lock_guard<std::mutex> lock(s.slot_mutex);
    if (slot_id >= s.local_slots.size()) {
      dist_logger().crt("dist slot");
      std::terminate();
    }
    slot = &s.local_slots[slot_id];
    if (!slot->in_use) {
      dist_logger().crt("dist slot");
      std::terminate();
    }
    if (size > slot->capacity) {
      dist_logger().crt("dist slot");
      std::terminate();
    }
  }

  std::uint8_t* data = slot->gptr.local();
  message_view msg;
  msg.src_rank = src_rank;
  msg.slot_id = slot_id;
  msg.payload = byte_span(data, size);
  push_incoming(std::move(msg));
}

void enqueue_command(command&& cmd) {
  auto& s = rt();

  auto has_capacity = [&]() {
    return s.cfg.max_pending_commands == 0 || s.commands.size() < s.cfg.max_pending_commands;
  };

  std::unique_lock<std::mutex> lock(s.cmd_mutex);
  if (s.stop.load(std::memory_order_acquire)) {
    throw_runtime_shutdown();
  }
  if (s.cfg.max_pending_commands > 0) {
    bool warned = false;
    while (!has_capacity()) {
      if (!warned) {
        dist_logger().wrn("dist queue");
        warned = true;
      }
      s.cmd_cv.wait(lock, [&] { return has_capacity() || s.stop.load(std::memory_order_acquire); });
      if (s.stop.load(std::memory_order_acquire)) {
        throw_runtime_shutdown();
      }
    }
  }

  s.commands.push_back(std::move(cmd));
  lock.unlock();
  s.cmd_cv.notify_one();
}

void execute_commands(std::deque<command>& local_cmds) {
  auto& s = rt();

  for (auto& cmd : local_cmds) {
    switch (cmd.kind) {
    case command::kind::send: {
      const int dest = cmd.dest_rank;
      const int src = s.my_rank.load(std::memory_order_acquire);
      const int n_ranks = s.n_ranks.load(std::memory_order_acquire);
      if (dest < 0 || dest >= n_ranks) {
        dist_logger().crtf("execute_commands: invalid dest_rank=%d (n_ranks=%d)", dest, n_ranks);
        std::terminate();
      }
      if (cmd.send_size == 0 || cmd.send_src == nullptr) {
        dist_logger().wrnf("execute_commands: ignoring empty send to rank %d", dest);
        if (cmd.send_promise) {
          cmd.send_promise->set_value();
        }
        break;
      }

      slot_allocation alloc = upcxx::rpc(dest, rpc_acquire_slot, cmd.send_size).wait();

      upcxx::future<> fut = upcxx::rput(cmd.send_src, alloc.gptr, cmd.send_size);

      pending_send ps;
      ps.dest_rank = dest;
      ps.src_rank = src;
      ps.slot_id = alloc.slot_id;
      ps.size = cmd.send_size;
      ps.rput_future = std::move(fut);
      if (cmd.send_promise) {
        ps.send_promise.emplace(std::move(*cmd.send_promise));
      }
      s.pending_sends.push_back(std::move(ps));
      break;
    }

    case command::kind::barrier: {
      upcxx::barrier();
      if (cmd.barrier_promise) {
        cmd.barrier_promise->set_value();
      }
      break;
    }

    case command::kind::allreduce_double: {
      double local = cmd.local_value;
      auto fut = upcxx::reduce_all(local, upcxx::op_fast_add);
      double result = fut.wait();
      if (cmd.allreduce_promise) {
        cmd.allreduce_promise->set_value(result);
      }
      break;
    }
    }
  }
}

void complete_pending_sends() {
  auto& s = rt();
  auto& pending = s.pending_sends;

  std::size_t idx = 0;
  while (idx < pending.size()) {
    pending_send& entry = pending[idx];
    if (!entry.rput_future.is_ready()) {
      ++idx;
      continue;
    }

    upcxx::rpc_ff(entry.dest_rank, rpc_large_complete, entry.src_rank, entry.slot_id, entry.size);
    if (entry.send_promise) {
      entry.send_promise->set_value();
    }

    const std::size_t last = pending.size() - 1;
    if (idx != last) {
      pending[idx] = std::move(pending[last]);
    }
    pending.pop_back();
  }
}

void upc_thread_main() {
  auto& s = rt();
  try {
    upcxx::init();

    const int my_rank = upcxx::rank_me();
    const int n_ranks = upcxx::rank_n();
    s.my_rank.store(my_rank, std::memory_order_release);
    s.n_ranks.store(n_ranks, std::memory_order_release);

    auto local_error = detail::shared_heap_error_message(s.cfg);
    int local_ok = local_error ? 0 : 1;
    int ok_sum = upcxx::reduce_all(local_ok, upcxx::op_fast_add).wait();
    if (local_error) {
      dist_logger().crtf("%s", local_error->c_str());
    }
    if (ok_sum != s.n_ranks.load(std::memory_order_acquire)) {
      std::string msg = local_error ? *local_error : detail::shared_heap_remote_failure_message(s.cfg);
      {
        std::lock_guard<std::mutex> lk(s.start_mutex);
        s.startup_error_msg = msg;
        s.startup_error_ready = true;
      }
      s.start_cv.notify_all();
      upcxx::finalize();
      return;
    }

    try {
      std::lock_guard<std::mutex> lock(s.slot_mutex);
      s.local_slots.resize(s.cfg.recv_slots);
      for (std::size_t i = 0; i < s.cfg.recv_slots; ++i) {
        s.local_slots[i].gptr = upcxx::new_array<std::uint8_t>(s.cfg.recv_slot_bytes);
        s.local_slots[i].capacity = s.cfg.recv_slot_bytes;
        s.local_slots[i].in_use = false;
      }
    } catch (const upcxx::bad_shared_alloc&) {
      dist_logger().crt("dist alloc");
      {
        std::lock_guard<std::mutex> lk(s.start_mutex);
        s.startup_error_msg = "dist alloc";
        s.startup_error_ready = true;
      }
      s.start_cv.notify_all();
      upcxx::finalize();
      return;
    }

    upcxx::barrier();

    {
      std::lock_guard<std::mutex> lk(s.start_mutex);
      s.started = true;
    }
    s.start_cv.notify_all();

    const int log_rank = s.my_rank.load(std::memory_order_acquire);
    if (log_rank == 0) {
      dist_logger().inf("dist ready");
    }

    while (true) {
      std::deque<command> local_cmds;
      bool drained_commands = false;

      {
        std::unique_lock<std::mutex> lock(s.cmd_mutex);
        auto wake_pred = [&] { return s.stop.load(std::memory_order_acquire) || !s.commands.empty(); };

        if (!wake_pred() && s.cfg.progress_sleep_ms > 0) {
          s.cmd_cv.wait_for(lock, std::chrono::milliseconds(s.cfg.progress_sleep_ms), wake_pred);
        }

        if (s.stop.load(std::memory_order_acquire) && s.commands.empty() && s.pending_sends.empty()) {
          break;
        }

        if (!s.commands.empty()) {
          local_cmds.swap(s.commands);
          drained_commands = true;
        }
      }

      if (drained_commands && s.cfg.max_pending_commands > 0) {
        s.cmd_cv.notify_all();
      }

      if (!local_cmds.empty()) {
        execute_commands(local_cmds);
      }

      upcxx::progress();
      complete_pending_sends();
    }

    upcxx::discharge();

    {
      std::lock_guard<std::mutex> lock(s.slot_mutex);
      for (auto& slot : s.local_slots) {
        if (slot.gptr) {
          upcxx::delete_array(slot.gptr);
          slot.gptr = nullptr;
        }
      }
      s.local_slots.clear();
    }

    upcxx::finalize();
  } catch (const std::exception&) {
    dist_logger().crt("dist exception");
    std::terminate();
  } catch (...) {
    dist_logger().crt("dist exception");
    std::terminate();
  }
}

void ensure_initialized() {
  auto& s = rt();
  std::unique_lock<std::mutex> lk(s.start_mutex);
  if (!s.started) {
    throw std::runtime_error("sn::dist runtime is not initialized; call sn::dist::init() first");
  }
}

}

const char* dist_codemode() {
#if defined(UPCXX_CODEMODE) && UPCXX_CODEMODE
  return "opt";
#elif defined(UPCXX_CODEMODE)
  return "debug";
#else
  return "debug";
#endif
}

const char* dist_threadmode() {
#if defined(UPCXX_THREADMODE) && UPCXX_THREADMODE
  return "par";
#elif defined(UPCXX_THREADMODE)
  return "seq";
#else
  return "seq";
#endif
}

void init(const runtime_config& cfg) {
  auto& s = rt();

  if (cfg.recv_slots == 0) {
    throw std::runtime_error("sn::dist::init(): recv_slots must be > 0");
  }
  if (cfg.recv_slot_bytes == 0) {
    throw std::runtime_error("sn::dist::init(): recv_slot_bytes must be > 0");
  }
  (void) shared_heap_bytes_required(cfg);

  {
    std::lock_guard<std::mutex> lk(s.start_mutex);
    if (s.started) {
      throw std::runtime_error("sn::dist::init() called more than once");
    }
    s.cfg = cfg;
    s.stop.store(false, std::memory_order_release);
    s.startup_error_ready = false;
    s.startup_error_msg.clear();
  }

  {
    std::lock_guard<std::mutex> lock(s.recv_mutex);
    s.shutdown_sent = false;
    s.incoming.clear();
  }
  {
    std::lock_guard<std::mutex> lock(s.cmd_mutex);
    s.commands.clear();
  }
  s.pending_sends.clear();

  s.upc_thread = std::thread(upc_thread_main);

  std::unique_lock<std::mutex> lk(s.start_mutex);
  s.start_cv.wait(lk, [&] { return s.started || s.startup_error_ready; });
  if (s.startup_error_ready) {
    lk.unlock();
    if (s.upc_thread.joinable()) {
      s.upc_thread.join();
    }
    throw std::runtime_error(s.startup_error_msg);
  }
}

void finalize() {
  auto& s = rt();

  {
    std::lock_guard<std::mutex> lk(s.start_mutex);
    if (!s.started) {
      return;
    }
  }

  s.stop.store(true, std::memory_order_release);
  s.cmd_cv.notify_all();
  signal_shutdown(s);

  if (s.upc_thread.joinable()) {
    s.upc_thread.join();
  }

  {
    std::lock_guard<std::mutex> lk(s.start_mutex);
    s.started = false;
    s.my_rank.store(-1, std::memory_order_release);
    s.n_ranks.store(0, std::memory_order_release);
  }

}

int rank() {
  ensure_initialized();
  return rt().my_rank.load(std::memory_order_acquire);
}

int world_size() {
  ensure_initialized();
  return rt().n_ranks.load(std::memory_order_acquire);
}

sn::util::future<void> async_send(int dest_rank, byte_span payload) {
  ensure_initialized();
  auto& s = rt();
  if (payload.size() > s.cfg.recv_slot_bytes) {
    throw std::runtime_error("async_send: payload exceeds configured recv_slot_bytes");
  }
  const int n_ranks = s.n_ranks.load(std::memory_order_acquire);
  if (dest_rank < 0 || dest_rank >= n_ranks) {
    throw std::runtime_error("async_send: invalid destination rank");
  }

  command cmd;
  cmd.kind = command::kind::send;
  cmd.dest_rank = dest_rank;
  cmd.send_src = payload.data();
  cmd.send_size = payload.size();
  cmd.send_promise.emplace();
  auto fut = cmd.send_promise->get_future();

  enqueue_command(std::move(cmd));
  return fut;
}

sn::util::future<void> async_send(int dest_rank, const std::vector<std::uint8_t>& payload) {
  byte_span view(payload.data(), payload.size());
  return async_send(dest_rank, view);
}

bool try_recv(message_view& out) {
  ensure_initialized();
  auto& s = rt();

  std::lock_guard<std::mutex> lock(s.recv_mutex);
  if (s.shutdown_sent) {
    throw_runtime_shutdown();
  }
  if (s.incoming.empty()) {
    return false;
  }

  out = s.incoming.front();
  s.incoming.pop_front();
  return true;
}

message_view recv() {
  ensure_initialized();
  auto& s = rt();

  std::unique_lock<std::mutex> lock(s.recv_mutex);
  s.recv_cv.wait(lock, [&] { return s.shutdown_sent || !s.incoming.empty(); });

  if (s.shutdown_sent) {
    throw_runtime_shutdown();
  }

  message_view msg = s.incoming.front();
  s.incoming.pop_front();
  return msg;
}

bool recv_for(message_view& out, std::chrono::milliseconds timeout) {
  ensure_initialized();
  auto& s = rt();

  std::unique_lock<std::mutex> lock(s.recv_mutex);
  if (!s.recv_cv.wait_for(lock, timeout, [&] { return s.shutdown_sent || !s.incoming.empty(); })) {
    return false;
  }

  if (s.shutdown_sent) {
    throw_runtime_shutdown();
  }

  out = s.incoming.front();
  s.incoming.pop_front();
  return true;
}

void release(const message_view& msg) {
  ensure_initialized();
  auto& s = rt();

  std::lock_guard<std::mutex> lock(s.slot_mutex);
  if (msg.slot_id >= s.local_slots.size()) {
    dist_logger().crtf("release: slot_id=%u out of range (slots=%zu)", msg.slot_id, s.local_slots.size());
    std::terminate();
  }

  recv_slot& slot = s.local_slots[msg.slot_id];
  if (!slot.in_use) {
    dist_logger().crtf("release: slot %u not currently in use", msg.slot_id);
    std::terminate();
  }
  slot.in_use = false;
}

std::size_t pending_messages() {
  ensure_initialized();
  auto& s = rt();

  std::lock_guard<std::mutex> lock(s.recv_mutex);
  if (s.shutdown_sent) {
    return 0;
  }
  return s.incoming.size();
}

void barrier() {
  ensure_initialized();

  command cmd;
  cmd.kind = command::kind::barrier;
  cmd.barrier_promise.emplace();
  auto fut = cmd.barrier_promise->get_future();

  enqueue_command(std::move(cmd));
  fut.get();
}

double allreduce_sum(double local_value) {
  ensure_initialized();

  command cmd;
  cmd.kind = command::kind::allreduce_double;
  cmd.local_value = local_value;
  cmd.allreduce_promise.emplace();
  auto fut = cmd.allreduce_promise->get_future();

  enqueue_command(std::move(cmd));
  return fut.get();
}

std::size_t shared_heap_bytes_required(const runtime_config& cfg) {
  if (cfg.recv_slots == 0 || cfg.recv_slot_bytes == 0) {
    return 0;
  }
  if (cfg.recv_slot_bytes > std::numeric_limits<std::size_t>::max() / cfg.recv_slots) {
    throw std::overflow_error("sn::dist::runtime_config requires more shared heap than size_t can express");
  }
  return cfg.recv_slots * cfg.recv_slot_bytes;
}

std::size_t shared_heap_bytes_recommended(const runtime_config& cfg) {
  const std::size_t required = shared_heap_bytes_required(cfg);
  if (required == 0) {
    return 0;
  }
  std::size_t guard = required / 4;
  if (guard < detail::kSharedHeapGuardMin) {
    guard = detail::kSharedHeapGuardMin;
  }
  if (guard > std::numeric_limits<std::size_t>::max() - required) {
    return std::numeric_limits<std::size_t>::max();
  }
  return required + guard;
}

}
