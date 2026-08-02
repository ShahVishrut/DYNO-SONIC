
#include "runtime.hpp"

#include <mpi.h>

#include <algorithm>
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
#include <functional>
#include <utility>
#include <vector>

#include "sonic/util/log.hpp"

namespace sn::dist {

namespace {

constexpr int kDataTag = 0;

sn::util::log::logger& dist_logger() {
  static sn::util::log::logger log = sn::util::log::create("sonic.dist");
  return log;
}

struct recv_slot {
  std::vector<std::uint8_t> buffer;
  MPI_Request request = MPI_REQUEST_NULL;
  bool posted = false;
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
  MPI_Request request = MPI_REQUEST_NULL;
  std::optional<sn::util::promise<void>> send_promise;
};

struct runtime_state {
  runtime_config cfg;

  std::thread mpi_thread;

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
  std::atomic<bool> recvs_need_posting{true};

  std::mutex recv_mutex;
  std::condition_variable recv_cv;
  std::deque<message_view> incoming;
  bool shutdown_sent = false;

  MPI_Comm data_comm = MPI_COMM_WORLD;
  MPI_Comm ctrl_comm = MPI_COMM_NULL;
  int recv_count = 0;

  int provided_thread_level = MPI_THREAD_SINGLE;

  std::size_t max_inflight_sends = 0;

  MPI_Request active_barrier_req = MPI_REQUEST_NULL;
  std::optional<sn::util::promise<void>> active_barrier_promise;

  std::vector<MPI_Request> scratch_reqs;
  std::vector<int> scratch_indices;
  std::vector<MPI_Status> scratch_statuses;
  std::vector<int> scratch_map;

  bool ever_started = false;

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

void complete_pending_sends() {
  auto& s = rt();
  auto& pending = s.pending_sends;

  if (pending.empty()) {
    return;
  }

  s.scratch_reqs.clear();
  s.scratch_reqs.reserve(pending.size());
  for (auto& p : pending) {
    s.scratch_reqs.push_back(p.request);
  }

  int outcount = 0;
  s.scratch_indices.resize(pending.size());
  int rc = MPI_Testsome(
      static_cast<int>(s.scratch_reqs.size()), s.scratch_reqs.data(), &outcount, s.scratch_indices.data(),
      MPI_STATUSES_IGNORE
  );
  if (rc != MPI_SUCCESS) {
    dist_logger().crtf("MPI_Testsome for sends failed rc=%d", rc);
    std::terminate();
  }

  if (outcount <= 0) {
    return;
  }

  std::sort(s.scratch_indices.begin(), s.scratch_indices.begin() + outcount, std::greater<int>());
  for (int k = 0; k < outcount; ++k) {
    int idx = s.scratch_indices[k];
    if (idx < 0 || static_cast<std::size_t>(idx) >= pending.size()) {
      continue;
    }
    pending_send& entry = pending[static_cast<std::size_t>(idx)];
    if (entry.send_promise) {
      entry.send_promise->set_value();
    }
    const std::size_t last = pending.size() - 1;
    if (static_cast<std::size_t>(idx) != last) {
      pending[static_cast<std::size_t>(idx)] = std::move(pending[last]);
    }
    pending.pop_back();
  }
}

void ensure_recvs_posted() {
  auto& s = rt();
  if (s.stop.load(std::memory_order_acquire)) {
    return;
  }

  if (!s.recvs_need_posting.load(std::memory_order_acquire)) {
    return;
  }

  std::lock_guard<std::mutex> lock(s.slot_mutex);
  for (std::size_t i = 0; i < s.local_slots.size(); ++i) {
    recv_slot& slot = s.local_slots[i];
    if (slot.in_use || slot.posted) {
      continue;
    }
    int rc =
        MPI_Irecv(slot.buffer.data(), s.recv_count, MPI_BYTE, MPI_ANY_SOURCE, kDataTag, s.data_comm, &slot.request);
    if (rc != MPI_SUCCESS) {
      dist_logger().crtf("MPI_Irecv failed rc=%d for slot=%zu", rc, i);
      std::terminate();
    }
    slot.posted = true;
  }

  s.recvs_need_posting.store(false, std::memory_order_release);
}

void poll_completed_recvs() {
  auto& s = rt();
  s.scratch_reqs.clear();
  s.scratch_map.clear();

  {
    std::lock_guard<std::mutex> lock(s.slot_mutex);
    for (std::size_t i = 0; i < s.local_slots.size(); ++i) {
      recv_slot& slot = s.local_slots[i];
      if (slot.posted && !slot.in_use) {
        s.scratch_reqs.push_back(slot.request);
        s.scratch_map.push_back(static_cast<int>(i));
      }
    }
  }

  if (s.scratch_reqs.empty()) {
    return;
  }

  int outcount = 0;
  s.scratch_indices.resize(s.scratch_reqs.size());
  s.scratch_statuses.resize(s.scratch_reqs.size());

  int rc = MPI_Testsome(
      static_cast<int>(s.scratch_reqs.size()), s.scratch_reqs.data(), &outcount, s.scratch_indices.data(),
      s.scratch_statuses.data()
  );
  if (rc != MPI_SUCCESS) {
    dist_logger().crtf("MPI_Testsome for recvs failed rc=%d", rc);
    std::terminate();
  }
  if (outcount <= 0) {
    return;
  }

  std::vector<message_view> ready;
  ready.reserve(static_cast<std::size_t>(outcount));

  {
    std::lock_guard<std::mutex> lock(s.slot_mutex);
    for (int k = 0; k < outcount; ++k) {
      int idx = s.scratch_indices[k];
      if (idx < 0 || static_cast<std::size_t>(idx) >= s.scratch_map.size()) {
        continue;
      }
      const int slot_id = s.scratch_map[static_cast<std::size_t>(idx)];
      if (slot_id < 0 || static_cast<std::size_t>(slot_id) >= s.local_slots.size()) {
        continue;
      }
      recv_slot& slot = s.local_slots[static_cast<std::size_t>(slot_id)];
      slot.posted = false;
      slot.request = MPI_REQUEST_NULL;
      slot.in_use = true;

      int count = 0;
      rc = MPI_Get_count(&s.scratch_statuses[static_cast<std::size_t>(k)], MPI_BYTE, &count);
      if (rc != MPI_SUCCESS) {
        dist_logger().crtf("MPI_Get_count failed rc=%d", rc);
        std::terminate();
      }
      if (count < 0 || static_cast<std::size_t>(count) > slot.buffer.size()) {
        dist_logger().crtf("MPI_Get_count returned invalid count=%d for slot capacity=%zu", count, slot.buffer.size());
        std::terminate();
      }

      message_view msg;
      msg.src_rank = s.scratch_statuses[static_cast<std::size_t>(k)].MPI_SOURCE;
      msg.slot_id = static_cast<std::uint32_t>(slot_id);
      msg.payload = byte_span(slot.buffer.data(), static_cast<std::size_t>(count));
      ready.push_back(msg);
    }
  }

  for (auto& msg : ready) {
    push_incoming(std::move(msg));
  }
}

void execute_commands(std::deque<command>& local_cmds) {
  auto& s = rt();

  for (auto& cmd : local_cmds) {
    switch (cmd.kind) {
    case command::kind::send: {
      const int dest = cmd.dest_rank;
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
      if (cmd.send_size > static_cast<std::size_t>(s.recv_count)) {
        dist_logger().crtf(
            "execute_commands: payload exceeds recv_slot_bytes (size=%zu limit=%d)", cmd.send_size, s.recv_count
        );
        std::terminate();
      }

      if (s.max_inflight_sends > 0) {
        while (s.pending_sends.size() >= s.max_inflight_sends) {

          complete_pending_sends();
          std::this_thread::yield();
        }
      }

      MPI_Request req = MPI_REQUEST_NULL;
      int rc = MPI_Isend(cmd.send_src, static_cast<int>(cmd.send_size), MPI_BYTE, dest, kDataTag, s.data_comm, &req);
      if (rc != MPI_SUCCESS) {
        dist_logger().crtf("MPI_Isend failed rc=%d dest=%d bytes=%zu", rc, dest, cmd.send_size);
        std::terminate();
      }

      pending_send ps;
      ps.request = req;
      if (cmd.send_promise) {
        ps.send_promise.emplace(std::move(*cmd.send_promise));
      }
      s.pending_sends.push_back(std::move(ps));
      break;
    }

    case command::kind::barrier: {
      if (s.active_barrier_req != MPI_REQUEST_NULL) {
        dist_logger().crtf("overlapping barriers are not supported");
        std::terminate();
      }

      int rc = MPI_Ibarrier(s.ctrl_comm, &s.active_barrier_req);
      if (rc != MPI_SUCCESS) {
        dist_logger().crtf("MPI_Ibarrier failed rc=%d", rc);
        std::terminate();
      }

      if (cmd.barrier_promise) {
        s.active_barrier_promise = std::move(cmd.barrier_promise);
      }
      break;
    }

    case command::kind::allreduce_double: {
      double local = cmd.local_value;
      double result = 0.0;
      int rc = MPI_Allreduce(&local, &result, 1, MPI_DOUBLE, MPI_SUM, s.ctrl_comm);
      if (rc != MPI_SUCCESS) {
        dist_logger().crtf("MPI_Allreduce failed rc=%d", rc);
        std::terminate();
      }
      if (cmd.allreduce_promise) {
        cmd.allreduce_promise->set_value(result);
      }
      break;
    }
    }
  }
}

void cancel_outstanding_recvs() {
  auto& s = rt();
  std::lock_guard<std::mutex> lock(s.slot_mutex);
  for (auto& slot : s.local_slots) {
    if (slot.posted && slot.request != MPI_REQUEST_NULL) {
      int rc = MPI_Cancel(&slot.request);
      if (rc != MPI_SUCCESS) {
        dist_logger().wrnf("MPI_Cancel(recv) failed rc=%d during shutdown", rc);
      }
      rc = MPI_Request_free(&slot.request);
      if (rc != MPI_SUCCESS) {
        dist_logger().wrnf("MPI_Request_free(recv) failed rc=%d during shutdown", rc);
      }
      slot.request = MPI_REQUEST_NULL;
      slot.posted = false;
    }
  }
}

void cancel_outstanding_sends() {
  auto& s = rt();
  for (auto& entry : s.pending_sends) {
    if (entry.request != MPI_REQUEST_NULL) {
      int rc = MPI_Cancel(&entry.request);
      if (rc != MPI_SUCCESS) {
        dist_logger().wrnf("MPI_Cancel(send) failed rc=%d during shutdown", rc);
      }
      rc = MPI_Request_free(&entry.request);
      if (rc != MPI_SUCCESS) {
        dist_logger().wrnf("MPI_Request_free(send) failed rc=%d during shutdown", rc);
      }
      entry.request = MPI_REQUEST_NULL;
    }
  }
  s.pending_sends.clear();
}

void check_active_barrier() {
  auto& s = rt();
  if (s.active_barrier_req == MPI_REQUEST_NULL) {
    return;
  }

  int done = 0;
  int rc = MPI_Test(&s.active_barrier_req, &done, MPI_STATUS_IGNORE);
  if (rc != MPI_SUCCESS) {
    dist_logger().crtf("MPI_Test for barrier failed rc=%d", rc);
    std::terminate();
  }

  if (done) {
    if (s.active_barrier_promise) {
      s.active_barrier_promise->set_value();
      s.active_barrier_promise.reset();
    }
    s.active_barrier_req = MPI_REQUEST_NULL;
  }
}

void mpi_thread_main() {
  auto& s = rt();

  auto set_startup_error = [&](std::string msg) {
    std::lock_guard<std::mutex> lk(s.start_mutex);
    s.startup_error_msg = std::move(msg);
    s.startup_error_ready = true;
  };

  try {
    int provided = MPI_THREAD_SINGLE;
    int rc = MPI_Init_thread(nullptr, nullptr, MPI_THREAD_FUNNELED, &provided);
    if (rc != MPI_SUCCESS) {
      set_startup_error("MPI_Init_thread failed rc=" + std::to_string(rc));
      s.start_cv.notify_all();
      return;
    }
    s.provided_thread_level = provided;

    if (provided < MPI_THREAD_FUNNELED) {
      set_startup_error("MPI_Init_thread did not provide MPI_THREAD_FUNNELED support");
      s.start_cv.notify_all();
      MPI_Finalize();
      return;
    }

    rc = MPI_Comm_dup(MPI_COMM_WORLD, &s.ctrl_comm);
    if (rc != MPI_SUCCESS) {
      set_startup_error("MPI_Comm_dup failed rc=" + std::to_string(rc));
      s.start_cv.notify_all();
      MPI_Finalize();
      return;
    }

    int my_rank = 0;
    int n_ranks = 0;
    MPI_Comm_rank(s.data_comm, &my_rank);
    MPI_Comm_size(s.data_comm, &n_ranks);
    s.my_rank.store(my_rank, std::memory_order_release);
    s.n_ranks.store(n_ranks, std::memory_order_release);

    {
      std::lock_guard<std::mutex> lock(s.slot_mutex);
      s.local_slots.resize(s.cfg.recv_slots);
      for (auto& slot : s.local_slots) {
        slot.buffer.resize(s.cfg.recv_slot_bytes);
        slot.request = MPI_REQUEST_NULL;
        slot.posted = false;
        slot.in_use = false;
      }
    }

    ensure_recvs_posted();

    rc = MPI_Barrier(s.ctrl_comm);
    if (rc != MPI_SUCCESS) {
      set_startup_error("MPI_Barrier failed during startup rc=" + std::to_string(rc));
      s.start_cv.notify_all();
      MPI_Finalize();
      return;
    }

    {
      std::lock_guard<std::mutex> lk(s.start_mutex);
      s.started = true;
      s.ever_started = true;
    }
    s.start_cv.notify_all();

    if (my_rank == 0) {
      dist_logger().inf("dist ready");
    }

    while (true) {
      ensure_recvs_posted();

      std::deque<command> local_cmds;
      bool drained_commands = false;

      {
        std::unique_lock<std::mutex> lock(s.cmd_mutex);
        auto wake_pred = [&] {
          return s.stop.load(std::memory_order_acquire) || !s.commands.empty() ||
                 s.recvs_need_posting.load(std::memory_order_acquire) || s.active_barrier_req != MPI_REQUEST_NULL;
        };

        if (!wake_pred() && s.cfg.progress_sleep_ms > 0) {
          s.cmd_cv.wait_for(lock, std::chrono::milliseconds(s.cfg.progress_sleep_ms), wake_pred);
        }

        if (s.stop.load(std::memory_order_acquire) && s.commands.empty() && s.pending_sends.empty() &&
            s.active_barrier_req == MPI_REQUEST_NULL) {
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

      poll_completed_recvs();
      complete_pending_sends();
      check_active_barrier();
    }

    cancel_outstanding_recvs();
    cancel_outstanding_sends();

    if (s.ctrl_comm != MPI_COMM_NULL) {
      MPI_Comm_free(&s.ctrl_comm);
      s.ctrl_comm = MPI_COMM_NULL;
    }

    MPI_Finalize();
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

const char* dist_codemode() { return "mpi"; }

const char* dist_threadmode() {
  auto& s = rt();
  switch (s.provided_thread_level) {
  case MPI_THREAD_MULTIPLE:
    return "multiple";
  case MPI_THREAD_SERIALIZED:
    return "serialized";
  case MPI_THREAD_FUNNELED:
    return "funneled";
  default:
    return "single";
  }
}

void init(const runtime_config& cfg) {
  auto& s = rt();

  if (cfg.recv_slots == 0) {
    throw std::runtime_error("sn::dist::init(): recv_slots must be > 0");
  }
  if (cfg.recv_slot_bytes == 0) {
    throw std::runtime_error("sn::dist::init(): recv_slot_bytes must be > 0");
  }
  if (cfg.recv_slot_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("sn::dist::init(): recv_slot_bytes exceeds MPI count limit");
  }

  {
    std::lock_guard<std::mutex> lk(s.start_mutex);
    if (s.started || s.ever_started) {
      throw std::runtime_error("sn::dist::init() may only be called once per process");
    }
    s.cfg = cfg;
    s.stop.store(false, std::memory_order_release);
    s.startup_error_ready = false;
    s.startup_error_msg.clear();
    s.provided_thread_level = MPI_THREAD_SINGLE;
    s.recv_count = static_cast<int>(cfg.recv_slot_bytes);
    s.max_inflight_sends = cfg.max_pending_commands;
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
  {
    std::lock_guard<std::mutex> lock(s.slot_mutex);
    s.local_slots.clear();
  }
  s.pending_sends.clear();
  s.scratch_reqs.clear();
  s.scratch_indices.clear();
  s.scratch_statuses.clear();
  s.scratch_map.clear();
  s.recvs_need_posting.store(true, std::memory_order_release);
  s.active_barrier_req = MPI_REQUEST_NULL;
  s.active_barrier_promise.reset();

  s.mpi_thread = std::thread(mpi_thread_main);

  std::unique_lock<std::mutex> lk(s.start_mutex);
  s.start_cv.wait(lk, [&] { return s.started || s.startup_error_ready; });
  if (s.startup_error_ready) {
    lk.unlock();
    if (s.mpi_thread.joinable()) {
      s.mpi_thread.join();
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

  if (s.mpi_thread.joinable()) {
    s.mpi_thread.join();
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
  if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("async_send: payload exceeds MPI count limit");
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
  s.recvs_need_posting.store(true, std::memory_order_release);
  s.cmd_cv.notify_one();
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

std::size_t shared_heap_bytes_required(const runtime_config&) { return 0; }

std::size_t shared_heap_bytes_recommended(const runtime_config&) { return 0; }

}
