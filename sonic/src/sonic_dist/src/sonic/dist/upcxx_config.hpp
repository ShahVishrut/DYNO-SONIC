
#pragma once

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

#include "sonic/dist/runtime.hpp"

namespace sn::dist::detail {

constexpr std::size_t kSharedHeapGuardMin = 128ull * 1024 * 1024;
constexpr std::size_t kSharedHeapDefault = 128ull * 1024 * 1024;

inline std::string format_bytes(std::size_t bytes) {
  const double kb = bytes / 1024.0;
  const double mb = kb / 1024.0;
  const double gb = mb / 1024.0;
  char buf[64];
  if (gb >= 1.0) {
    std::snprintf(buf, sizeof(buf), "%.2f GB", gb);
    return std::string(buf);
  }
  if (mb >= 1.0) {
    std::snprintf(buf, sizeof(buf), "%.2f MB", mb);
    return std::string(buf);
  }
  if (kb >= 1.0) {
    std::snprintf(buf, sizeof(buf), "%.2f KB", kb);
    return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "%zu bytes", bytes);
  return std::string(buf);
}

inline std::optional<std::size_t> parse_heap_size(const char* raw) {
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  errno = 0;
  char* end = nullptr;
  double base = std::strtod(raw, &end);
  if (end == raw || errno == ERANGE || base < 0.0) {
    return std::nullopt;
  }
  while (end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end == '\0') {
    if (base > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(base);
  }
  const char suffix = *end++;
  while (end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != '\0') {
    if ((*end == 'B' || *end == 'b') && *(end + 1) == '\0') {

    } else {
      return std::nullopt;
    }
  }

  std::size_t multiplier = 1;
  switch (suffix) {
  case 'k':
  case 'K':
    multiplier = 1024ull;
    break;
  case 'm':
  case 'M':
    multiplier = 1024ull * 1024ull;
    break;
  case 'g':
  case 'G':
    multiplier = 1024ull * 1024ull * 1024ull;
    break;
  case 't':
  case 'T':
    multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
    break;
  default:
    return std::nullopt;
  }

  const double scaled = base * static_cast<double>(multiplier);
  if (scaled > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(scaled);
}

inline std::optional<std::string> shared_heap_error_message(const runtime_config& cfg) {
  const std::size_t required = shared_heap_bytes_required(cfg);
  if (required == 0) {
    return std::nullopt;
  }
  const std::size_t recommended = shared_heap_bytes_recommended(cfg);

  const char* upc_env = std::getenv("UPCXX_SHARED_HEAP_SIZE");
  const char* pgas_env = upc_env ? nullptr : std::getenv("UPCXX_PGAS_SIZE");
  auto parsed = parse_heap_size(upc_env ? upc_env : pgas_env);
  const std::size_t available = parsed.value_or(kSharedHeapDefault);
  const std::string env_desc = upc_env ? upc_env : (pgas_env ? pgas_env : "default 128MB");

  if (cfg.recv_slot_bytes > available) {
    std::string msg = "sn::dist requires recv_slot_bytes=" + std::to_string(cfg.recv_slot_bytes) + " (" +
                      format_bytes(cfg.recv_slot_bytes) + ") but upcxx shared heap is " + env_desc + " (" +
                      format_bytes(available) +
                      "). launch with `upcxx-run "
                      "-shared-heap " +
                      std::to_string(recommended) + " ...` or reduce recv_slot_bytes.";
    return msg;
  }

  if (required > available) {
    std::string msg = "sn::dist needs " + format_bytes(required) + " (" + std::to_string(required) +
                      " bytes) of shared heap but upcxx shared heap is " + env_desc + " (" + format_bytes(available) +
                      "). launch with `upcxx-run -shared-heap " + std::to_string(recommended) +
                      " ...` or lower recv_slots/recv_slot_bytes.";
    return msg;
  }

  return std::nullopt;
}

inline std::string shared_heap_remote_failure_message(const runtime_config& cfg) {
  const std::size_t required = shared_heap_bytes_required(cfg);
  const std::size_t recommended = shared_heap_bytes_recommended(cfg);
  return "one or more ranks lack enough upcxx shared heap for sn::dist. Each rank needs " + format_bytes(required) +
         " (" + std::to_string(required) + " bytes); launch with `upcxx-run -shared-heap " +
         std::to_string(recommended) + " ...` or reduce recv_slots/recv_slot_bytes.";
}

}
