
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/util/future.hpp"
#include "sonic/util/span.hpp"

namespace sn::dist {

struct runtime_config {

  int progress_sleep_ms = 0;

  std::size_t max_pending_commands = 0;

  std::size_t max_pending_messages = 0;

  std::size_t recv_slots = 4;

  std::size_t recv_slot_bytes = 256 * 1024 * 1024;
};

std::size_t shared_heap_bytes_required(const runtime_config& cfg);
std::size_t shared_heap_bytes_recommended(const runtime_config& cfg);

const char* dist_codemode();
const char* dist_threadmode();

void init(const runtime_config& cfg = {});

void finalize();

int rank();
int world_size();

using byte_span = sn::util::span<const std::uint8_t>;

struct message_view {
  int src_rank = -1;
  byte_span payload;
  std::uint32_t slot_id = 0;
};

sn::util::future<void> async_send(int dest_rank, byte_span payload);

sn::util::future<void> async_send(int dest_rank, const std::vector<std::uint8_t>& payload);

bool try_recv(message_view& out);

message_view recv();

bool recv_for(message_view& out, std::chrono::milliseconds timeout);

void release(const message_view& msg);

std::size_t pending_messages();

void barrier();
double allreduce_sum(double local_value);

}
