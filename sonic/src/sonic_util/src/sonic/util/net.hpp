#pragma once

#include "sonic/util/span.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <stdexcept>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace sn::util::net {

#if !defined(SN_UTIL_NET_USE_CANCELLATION_PIPE)
#if defined(_WIN32)
#define SN_UTIL_NET_USE_CANCELLATION_PIPE 0
#else
#define SN_UTIL_NET_USE_CANCELLATION_PIPE 1
#endif
#endif

#if defined(_WIN32)
inline constexpr IN6_ADDR win_in6_loopback = IN6ADDR_LOOPBACK_INIT;
inline constexpr IN6_ADDR win_in6_any = IN6ADDR_ANY_INIT;
#endif

class net_error : public std::system_error {
public:
  net_error(int code, const char* what_arg) : std::system_error(code, std::system_category(), what_arg) {}
  net_error(std::errc code, const char* what_arg) : std::system_error(std::make_error_code(code), what_arg) {}
};

enum class address_family {
  any,
  ipv4,
  ipv6,
};

enum class shutdown_mode { send, receive, both };

namespace detail {

#if defined(_WIN32)
using native_socket = SOCKET;
inline constexpr native_socket invalid_socket = INVALID_SOCKET;
inline constexpr int socket_error_value = SOCKET_ERROR;

class winsock_runtime {
public:
  winsock_runtime() {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      throw net_error(rc, "WSAStartup");
    }
  }

  ~winsock_runtime() { WSACleanup(); }
};

inline void ensure_runtime() {
  static const winsock_runtime runtime{};
  (void) runtime;
}

inline int last_error_code() noexcept { return WSAGetLastError(); }

inline void close_socket(native_socket sock) noexcept {
  if (sock != invalid_socket) {
    ::closesocket(sock);
  }
}

inline bool is_retryable(int code) noexcept {
  return code == WSAEINTR || code == WSAEWOULDBLOCK || code == WSAEALREADY;
}

inline bool is_not_connected(int code) noexcept { return code == WSAENOTCONN; }

inline int to_platform_shutdown(shutdown_mode mode) noexcept {
  switch (mode) {
  case shutdown_mode::send:
    return SD_SEND;
  case shutdown_mode::receive:
    return SD_RECEIVE;
  case shutdown_mode::both:
  default:
    return SD_BOTH;
  }
}

#else
using native_socket = int;
inline constexpr native_socket invalid_socket = -1;
inline constexpr int socket_error_value = -1;

inline void raise_fd_limit() noexcept {
#if defined(RLIMIT_NOFILE)
  static const bool adjusted = []() noexcept {
    struct rlimit lim;
    if (::getrlimit(RLIMIT_NOFILE, &lim) != 0) {
      return true;
    }
    constexpr rlim_t desired_min = 16384;
    rlim_t desired = desired_min;
    if (lim.rlim_max != RLIM_INFINITY && desired > lim.rlim_max) {
      desired = lim.rlim_max;
    }
    if (desired > lim.rlim_cur) {
      struct rlimit updated = lim;
      updated.rlim_cur = desired;
      (void) ::setrlimit(RLIMIT_NOFILE, &updated);
    }
    return true;
  }();
  (void) adjusted;
#endif
}

inline void ensure_runtime() noexcept { raise_fd_limit(); }

inline int last_error_code() noexcept { return errno; }

inline void close_socket(native_socket sock) noexcept {
  if (sock != invalid_socket) {
    ::close(sock);
  }
}

inline bool is_retryable(int code) noexcept { return code == EINTR || code == EAGAIN || code == EWOULDBLOCK; }

inline bool is_not_connected(int code) noexcept { return code == ENOTCONN; }

inline int to_platform_shutdown(shutdown_mode mode) noexcept {
  switch (mode) {
  case shutdown_mode::send:
    return SHUT_WR;
  case shutdown_mode::receive:
    return SHUT_RD;
  case shutdown_mode::both:
  default:
    return SHUT_RDWR;
  }
}

#endif

[[noreturn]] inline void throw_last_error(const char* what) {
  const int code = last_error_code();
  throw net_error(code, what);
}

inline int to_native_family(address_family family) noexcept {
  switch (family) {
  case address_family::any:
    return AF_UNSPEC;
  case address_family::ipv4:
    return AF_INET;
  case address_family::ipv6:
    return AF_INET6;
  }
  return AF_UNSPEC;
}

inline void set_blocking(native_socket sock, bool blocking) {
#if defined(_WIN32)
  u_long mode = blocking ? 0UL : 1UL;
  if (::ioctlsocket(sock, FIONBIO, &mode) == SOCKET_ERROR) {
    throw_last_error("ioctlsocket");
  }
#else
  int flags = ::fcntl(sock, F_GETFL, 0);
  if (flags == -1) {
    throw_last_error("fcntl(F_GETFL)");
  }
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }
  if (::fcntl(sock, F_SETFL, flags) == -1) {
    throw_last_error("fcntl(F_SETFL)");
  }
#endif
}

inline bool connect_in_progress(int code) noexcept {
#if defined(_WIN32)
  return code == WSAEINPROGRESS || code == WSAEWOULDBLOCK || code == WSAEALREADY;
#else
  return code == EINPROGRESS || code == EALREADY || code == EWOULDBLOCK || code == EINTR;
#endif
}

inline bool wait_writable(native_socket sock, std::chrono::milliseconds timeout) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("connect timeout must be non-negative");
  }
  if (sock == invalid_socket) {
    throw std::invalid_argument("wait_writable: invalid socket");
  }

  fd_set write_set;
  FD_ZERO(&write_set);
  FD_SET(sock, &write_set);

  timeval tv{};
  timeval* tv_ptr = nullptr;
  if (timeout == std::chrono::milliseconds::max()) {
    tv_ptr = nullptr;
  } else {
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    tv_ptr = &tv;
  }

  for (;;) {
#if defined(_WIN32)
    const int rc = ::select(0, nullptr, &write_set, nullptr, tv_ptr);
#else
    const int rc = ::select(sock + 1, nullptr, &write_set, nullptr, tv_ptr);
#endif
    if (rc < 0) {
      const int err = last_error_code();
      if (is_retryable(err)) {
        continue;
      }
      throw_last_error("select");
    }
    return rc > 0;
  }
}

inline bool wait_readable(native_socket sock, std::chrono::milliseconds timeout) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("recv timeout must be non-negative");
  }
  if (sock == invalid_socket) {
    throw std::invalid_argument("wait_readable: invalid socket");
  }

  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(sock, &read_set);

  timeval tv{};
  timeval* tv_ptr = nullptr;
  if (timeout == std::chrono::milliseconds::max()) {
    tv_ptr = nullptr;
  } else {
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    tv_ptr = &tv;
  }

  for (;;) {
#if defined(_WIN32)
    const int rc = ::select(0, &read_set, nullptr, nullptr, tv_ptr);
#else
    const int rc = ::select(sock + 1, &read_set, nullptr, nullptr, tv_ptr);
#endif
    if (rc < 0) {
      const int err = last_error_code();
      if (is_retryable(err)) {
        continue;
      }
      throw_last_error("select");
    }
    return rc > 0;
  }
}

struct cancellation_handles {
  native_socket read_fd = invalid_socket;
  native_socket write_fd = invalid_socket;
};

inline void close_cancellation_handles(cancellation_handles& handles) noexcept {
#if !defined(_WIN32) && SN_UTIL_NET_USE_CANCELLATION_PIPE
  if (handles.read_fd != invalid_socket) {
    ::close(handles.read_fd);
    handles.read_fd = invalid_socket;
  }
  if (handles.write_fd != invalid_socket) {
    ::close(handles.write_fd);
    handles.write_fd = invalid_socket;
  }
#else
  (void) handles;
#endif
}

inline cancellation_handles make_cancellation_handles() {
  cancellation_handles handles;
#if !defined(_WIN32) && SN_UTIL_NET_USE_CANCELLATION_PIPE
  int fds[2] = {-1, -1};
  if (::pipe(fds) == -1) {
    throw_last_error("pipe");
  }
  try {
    if (::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == -1) {
      throw_last_error("fcntl(pipe, O_NONBLOCK)");
    }
    if (::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK) == -1) {
      throw_last_error("fcntl(pipe, O_NONBLOCK)");
    }
  } catch (...) {
    ::close(fds[0]);
    ::close(fds[1]);
    throw;
  }
  handles.read_fd = fds[0];
  handles.write_fd = fds[1];
#endif
  return handles;
}

inline void signal_cancellation(const cancellation_handles& handles) noexcept {
#if !defined(_WIN32) && SN_UTIL_NET_USE_CANCELLATION_PIPE
  if (handles.write_fd == invalid_socket) {
    return;
  }
  const std::uint8_t byte = 1;
  ::write(handles.write_fd, &byte, sizeof(byte));
#else
  (void) handles;
#endif
}

struct cancellation_state {
  cancellation_state() : handles(make_cancellation_handles()) {}

  ~cancellation_state() { close_cancellation_handles(handles); }

  void request() noexcept {
    if (!cancelled.exchange(true, std::memory_order_acq_rel)) {
      signal_cancellation(handles);
    }
  }

  std::atomic<bool> cancelled{false};
  cancellation_handles handles;
};

inline std::size_t max_transfer_size() noexcept {
#if defined(_WIN32)
  return static_cast<std::size_t>(std::numeric_limits<int>::max());
#else
  return static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
#endif
}

inline native_socket create_socket(int family, int type, int protocol) {
  ensure_runtime();
  native_socket sock = ::socket(family, type, protocol);
  if (sock == invalid_socket) {
    throw_last_error("socket");
  }
  return sock;
}

inline void set_reuseaddr(native_socket sock) {
  constexpr int opt = 1;
#if defined(_WIN32)
  if (::setsockopt(
          sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), static_cast<int>(sizeof(opt))
      ) == socket_error_value) {
    throw_last_error("setsockopt(SO_REUSEADDR)");
  }
#else
  if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == socket_error_value) {
    throw_last_error("setsockopt(SO_REUSEADDR)");
  }
#if defined(SO_REUSEPORT)
  if (::setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == socket_error_value) {
    throw_last_error("setsockopt(SO_REUSEPORT)");
  }
#endif
#endif
}

inline void set_nodelay(native_socket sock, bool enable) {
#if defined(IPPROTO_TCP) && defined(TCP_NODELAY)
  int opt = enable ? 1 : 0;
#if defined(_WIN32)
  if (::setsockopt(
          sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), static_cast<int>(sizeof(opt))
      ) == socket_error_value) {
    throw_last_error("setsockopt(TCP_NODELAY)");
  }
#else
  if (::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == socket_error_value) {
    throw_last_error("setsockopt(TCP_NODELAY)");
  }
#endif
#else
  (void) sock;
  (void) enable;
#endif
}

inline std::string addrinfo_error_to_string(int code) {
#if defined(_WIN32)
  const char* message = gai_strerrorA(code);
#else
  const char* message = gai_strerror(code);
#endif
  return message ? std::string(message) : std::string("unknown addrinfo error");
}

inline void throw_addrinfo_error(const char* what, int code) {
  throw std::runtime_error(std::string(what) + ": " + addrinfo_error_to_string(code));
}

inline void assign_sockaddr(sockaddr_storage& storage, socklen_t& len, const sockaddr* addr, socklen_t addr_len) {
  std::memcpy(&storage, addr, static_cast<size_t>(addr_len));
  len = addr_len;
}

}

class socket_handle {
public:
  socket_handle() noexcept = default;
  explicit socket_handle(detail::native_socket sock) noexcept : sock_(sock) {}

  socket_handle(const socket_handle&) = delete;
  socket_handle& operator=(const socket_handle&) = delete;

  socket_handle(socket_handle&& other) noexcept : sock_(other.release()) {}

  socket_handle& operator=(socket_handle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  ~socket_handle() { reset(); }

  [[nodiscard]] detail::native_socket get() const noexcept { return sock_; }

  [[nodiscard]] detail::native_socket release() noexcept {
    const detail::native_socket tmp = sock_;
    sock_ = detail::invalid_socket;
    return tmp;
  }

  void reset(detail::native_socket sock = detail::invalid_socket) noexcept {
    if (sock_ != detail::invalid_socket) {
      detail::close_socket(sock_);
    }
    sock_ = sock;
  }

  explicit operator bool() const noexcept { return sock_ != detail::invalid_socket; }

private:
  detail::native_socket sock_ = detail::invalid_socket;
};

class endpoint {
public:
  endpoint() = default;

  endpoint(const sockaddr* addr, socklen_t len) { detail::assign_sockaddr(storage_, length_, addr, len); }

  [[nodiscard]] static endpoint loopback(address_family family, uint16_t port) noexcept {
    endpoint ep;
    switch (family) {
    case address_family::ipv4: {
      auto* addr = reinterpret_cast<sockaddr_in*>(&ep.storage_);
      addr->sin_family = AF_INET;
      addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr->sin_port = htons(port);
      ep.length_ = static_cast<socklen_t>(sizeof(sockaddr_in));
      break;
    }
    case address_family::ipv6: {
      auto* addr6 = reinterpret_cast<sockaddr_in6*>(&ep.storage_);
      addr6->sin6_family = AF_INET6;
#if defined(_WIN32)
      addr6->sin6_addr = win_in6_loopback;
#else
      addr6->sin6_addr = in6addr_loopback;
#endif
      addr6->sin6_port = htons(port);
      ep.length_ = static_cast<socklen_t>(sizeof(sockaddr_in6));
      break;
    }
    case address_family::any:
      return loopback(address_family::ipv4, port);
    }
    return ep;
  }

  [[nodiscard]] static endpoint any(address_family family, uint16_t port) noexcept {
    endpoint ep;
    switch (family) {
    case address_family::ipv4: {
      auto* addr = reinterpret_cast<sockaddr_in*>(&ep.storage_);
      addr->sin_family = AF_INET;
      addr->sin_addr.s_addr = htonl(INADDR_ANY);
      addr->sin_port = htons(port);
      ep.length_ = static_cast<socklen_t>(sizeof(sockaddr_in));
      break;
    }
    case address_family::ipv6: {
      auto* addr6 = reinterpret_cast<sockaddr_in6*>(&ep.storage_);
      addr6->sin6_family = AF_INET6;
#if defined(_WIN32)
      addr6->sin6_addr = win_in6_any;
#else
      addr6->sin6_addr = in6addr_any;
#endif
      addr6->sin6_port = htons(port);
      ep.length_ = static_cast<socklen_t>(sizeof(sockaddr_in6));
      break;
    }
    case address_family::any:
      return any(address_family::ipv6, port);
    }
    return ep;
  }

  [[nodiscard]] static endpoint from_string(
      std::string_view host, uint16_t port, address_family family = address_family::any
  ) {
    detail::ensure_runtime();
    std::array<char, 6> service{};
    const int written = std::snprintf(service.data(), service.size(), "%u", static_cast<unsigned>(port));
    if (written <= 0 || written >= static_cast<int>(service.size())) {
      throw std::runtime_error("port string overflow");
    }

    std::string host_copy(host);
    addrinfo hints{};
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_family = detail::to_native_family(family);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host_copy.c_str(), service.data(), &hints, &result);
    if (rc != 0) {
      detail::throw_addrinfo_error("getaddrinfo", rc);
    }

    endpoint ep;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
      ep = endpoint(it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
      break;
    }
    ::freeaddrinfo(result);
    if (ep.length_ == 0) {
      throw std::runtime_error("getaddrinfo returned no results");
    }
    return ep;
  }

  [[nodiscard]] const sockaddr* addr() const noexcept { return reinterpret_cast<const sockaddr*>(&storage_); }

  [[nodiscard]] socklen_t length() const noexcept { return length_; }

  [[nodiscard]] sa_family_t family() const noexcept { return storage_.ss_family; }

  [[nodiscard]] uint16_t port() const noexcept {
    switch (storage_.ss_family) {
    case AF_INET:
      return ntohs(reinterpret_cast<const sockaddr_in*>(&storage_)->sin_port);
    case AF_INET6:
      return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_port);
    default:
      return 0;
    }
  }

private:
  sockaddr_storage storage_{};
  socklen_t length_ = 0;
};

class cancellation_token {
public:
  cancellation_token() = default;

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

  [[nodiscard]] bool is_cancellation_requested() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
  }

  [[nodiscard]] detail::native_socket wake_handle() const noexcept {
#if !defined(_WIN32)
    return state_ ? state_->handles.read_fd : detail::invalid_socket;
#else
    return detail::invalid_socket;
#endif
  }

private:
  explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::cancellation_state> state_;

  friend class cancellation_source;
};

class cancellation_source {
public:
  cancellation_source() : state_(std::make_shared<detail::cancellation_state>()) {}

  [[nodiscard]] cancellation_token token() const noexcept { return cancellation_token(state_); }

  void request_cancel() noexcept {
    if (state_) {
      state_->request();
    }
  }

private:
  std::shared_ptr<detail::cancellation_state> state_;
};

class tcp_stream {
public:
  tcp_stream() = default;
  explicit tcp_stream(socket_handle sock) noexcept : sock_(std::move(sock)) {}

  tcp_stream(const tcp_stream&) = delete;
  tcp_stream& operator=(const tcp_stream&) = delete;

  tcp_stream(tcp_stream&&) noexcept = default;
  tcp_stream& operator=(tcp_stream&&) noexcept = default;

  ~tcp_stream() = default;

  static tcp_stream connect(std::string_view host, uint16_t port, address_family family = address_family::any) {
    return connect(host, port, std::chrono::milliseconds::max(), family);
  }

  static tcp_stream connect(
      std::string_view host, uint16_t port, std::chrono::milliseconds timeout,
      address_family family = address_family::any
  ) {
    detail::ensure_runtime();

    if (timeout.count() < 0) {
      throw std::invalid_argument("connect timeout must be non-negative");
    }

    std::array<char, 6> service{};
    const int written = std::snprintf(service.data(), service.size(), "%u", static_cast<unsigned>(port));
    if (written <= 0 || written >= static_cast<int>(service.size())) {
      throw std::runtime_error("port string overflow");
    }

    std::string host_copy(host);
    addrinfo hints{};
    hints.ai_family = detail::to_native_family(family);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host_copy.c_str(), service.data(), &hints, &result);
    if (rc != 0) {
      detail::throw_addrinfo_error("getaddrinfo", rc);
    }

    const bool bounded = timeout != std::chrono::milliseconds::max();
    const auto deadline =
        bounded ? std::chrono::steady_clock::now() + timeout : std::chrono::steady_clock::time_point::max();

    socket_handle connected;
    int last_error = 0;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
      std::chrono::milliseconds remaining = std::chrono::milliseconds::max();
      if (bounded) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          remaining = std::chrono::milliseconds(0);
        } else {
          remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        }
      }
      try {
        connected = connect_addrinfo(*it, remaining);
        if (connected) {
          break;
        }
      } catch (const net_error& ex) {
        last_error = ex.code().value();
        if (bounded && ex.code() == std::make_error_code(std::errc::timed_out)) {
          ::freeaddrinfo(result);
          throw;
        }
      }
    }

    ::freeaddrinfo(result);
    if (!connected) {
      if (bounded && std::chrono::steady_clock::now() >= deadline) {
        throw net_error(std::errc::timed_out, "connect");
      }
      throw net_error(last_error ? last_error : static_cast<int>(std::errc::host_unreachable), "connect");
    }
    return tcp_stream(std::move(connected));
  }

  static tcp_stream from_native(detail::native_socket sock) noexcept { return tcp_stream(socket_handle(sock)); }

  [[nodiscard]] bool is_open() const noexcept { return static_cast<bool>(sock_); }

  [[nodiscard]] detail::native_socket native_handle() const noexcept { return sock_.get(); }

  [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) const {
    if (timeout.count() < 0) {
      throw std::invalid_argument("recv timeout must be non-negative");
    }
    if (!sock_) {
      return false;
    }
    return detail::wait_readable(sock_.get(), timeout);
  }

  void shutdown(shutdown_mode mode) {
    if (!sock_) {
      return;
    }
    if (::shutdown(sock_.get(), detail::to_platform_shutdown(mode)) == detail::socket_error_value) {
      const int err = detail::last_error_code();
      if (detail::is_not_connected(err)) {
        return;
      }
      throw net_error(err, "shutdown");
    }
  }

  void close() noexcept { sock_.reset(); }

  void send_all(sn::util::span<const std::uint8_t> buffer) {
    const std::uint8_t* data = buffer.data();
    std::size_t remaining = buffer.size();
    while (remaining > 0) {
      const std::size_t chunk = std::min(remaining, detail::max_transfer_size());
      const auto sent = send_some_internal(sn::util::span<const std::uint8_t>(data, chunk));
      data += sent;
      remaining -= sent;
    }
  }

  [[nodiscard]] std::size_t send_some(sn::util::span<const std::uint8_t> buffer) { return send_some_internal(buffer); }

  [[nodiscard]] std::size_t write(const std::uint8_t* data, std::size_t len) {
    if (len == 0) {
      return 0;
    }
    const std::size_t chunk = std::min(len, detail::max_transfer_size());
    return send_some(sn::util::span<const std::uint8_t>(data, chunk));
  }

  void recv_all(sn::util::span<std::uint8_t> buffer) {
    std::uint8_t* data = buffer.data();
    std::size_t remaining = buffer.size();
    while (remaining > 0) {
      const std::size_t chunk = std::min(remaining, detail::max_transfer_size());
      const auto received = recv_some_internal(sn::util::span<std::uint8_t>(data, chunk));
      if (received == 0) {
        throw net_error(std::errc::connection_reset, "recv");
      }
      data += received;
      remaining -= received;
    }
  }

  [[nodiscard]] std::size_t recv_some(sn::util::span<std::uint8_t> buffer) { return recv_some_internal(buffer); }

  void set_cancellation_token(cancellation_token token) { cancellation_ = std::move(token); }

  [[nodiscard]] std::size_t read(std::uint8_t* data, std::size_t len) {
    if (len == 0) {
      return 0;
    }
    const std::size_t chunk = std::min(len, detail::max_transfer_size());
    if (!cancellation_.valid()) {
      return recv_some(sn::util::span<std::uint8_t>(data, chunk));
    }
    return read_with_cancellation(data, chunk);
  }

  [[nodiscard]] endpoint local_endpoint() const {
    sockaddr_storage storage{};
    socklen_t len = static_cast<socklen_t>(sizeof(storage));
    if (::getsockname(sock_.get(), reinterpret_cast<sockaddr*>(&storage), &len) == detail::socket_error_value) {
      detail::throw_last_error("getsockname");
    }
    return endpoint(reinterpret_cast<sockaddr*>(&storage), len);
  }

  [[nodiscard]] endpoint remote_endpoint() const {
    sockaddr_storage storage{};
    socklen_t len = static_cast<socklen_t>(sizeof(storage));
    if (::getpeername(sock_.get(), reinterpret_cast<sockaddr*>(&storage), &len) == detail::socket_error_value) {
      detail::throw_last_error("getpeername");
    }
    return endpoint(reinterpret_cast<sockaddr*>(&storage), len);
  }

private:
  static socket_handle connect_addrinfo(const addrinfo& info, std::chrono::milliseconds timeout) {
    socket_handle candidate(detail::create_socket(info.ai_family, info.ai_socktype, info.ai_protocol));
    detail::set_blocking(candidate.get(), false);

    const int rc = ::connect(candidate.get(), info.ai_addr, static_cast<socklen_t>(info.ai_addrlen));
    if (rc == 0) {
      detail::set_blocking(candidate.get(), true);
      detail::set_nodelay(candidate.get(), true);
      return candidate;
    }

    const int error = detail::last_error_code();
    if (!detail::connect_in_progress(error)) {
      throw net_error(error, "connect");
    }

    if (!detail::wait_writable(candidate.get(), timeout)) {
      throw net_error(std::errc::timed_out, "connect");
    }

    int pending_error = 0;
#if defined(_WIN32)
    int opt_len = static_cast<int>(sizeof(pending_error));
    if (::getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending_error), &opt_len) ==
        SOCKET_ERROR) {
      detail::throw_last_error("getsockopt(SO_ERROR)");
    }
#else
    socklen_t opt_len = static_cast<socklen_t>(sizeof(pending_error));
    if (::getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, &pending_error, &opt_len) == -1) {
      detail::throw_last_error("getsockopt(SO_ERROR)");
    }
#endif

    if (pending_error != 0) {
      throw net_error(pending_error, "connect");
    }

    detail::set_blocking(candidate.get(), true);
    detail::set_nodelay(candidate.get(), true);
    return candidate;
  }

  std::size_t send_some_internal(sn::util::span<const std::uint8_t> buffer) {
    if (buffer.empty()) {
      return 0;
    }
    for (;;) {
#if defined(_WIN32)
      const auto result =
          ::send(sock_.get(), reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
      const auto result = ::send(sock_.get(), buffer.data(), buffer.size(), 0);
#endif
      if (result == detail::socket_error_value) {
        const int err = detail::last_error_code();
        if (detail::is_retryable(err)) {
          continue;
        }
        throw net_error(err, "send");
      }
      if (result == 0) {
        throw net_error(std::errc::connection_reset, "send");
      }
      return static_cast<std::size_t>(result);
    }
  }

  std::size_t recv_some_internal(sn::util::span<std::uint8_t> buffer) {
    if (buffer.empty()) {
      return 0;
    }
    for (;;) {
#if defined(_WIN32)
      const auto result =
          ::recv(sock_.get(), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
      const auto result = ::recv(sock_.get(), buffer.data(), buffer.size(), 0);
#endif
      if (result == 0) {
        return 0;
      }
      if (result == detail::socket_error_value) {
        const int err = detail::last_error_code();
        if (detail::is_retryable(err)) {
          continue;
        }
        throw net_error(err, "recv");
      }
      return static_cast<std::size_t>(result);
    }
  }

  std::size_t read_with_cancellation(std::uint8_t* data, std::size_t len) {
#if defined(_WIN32)
    if (cancellation_.is_cancellation_requested()) {
      return 0;
    }
    return recv_some_internal(sn::util::span<std::uint8_t>(data, len));
#else
    while (true) {
      if (cancellation_.is_cancellation_requested()) {
        return 0;
      }
      struct pollfd fds[2];
      nfds_t count = 1;
      fds[0].fd = sock_.get();
      fds[0].events = POLLIN;
      const auto wake = cancellation_.wake_handle();
      if (wake != detail::invalid_socket) {
        fds[1].fd = wake;
        fds[1].events = POLLIN;
        count = 2;
      }
      const int rc = ::poll(fds, count, -1);
      if (rc < 0) {
        const int err = detail::last_error_code();
        if (detail::is_retryable(err)) {
          continue;
        }
        detail::throw_last_error("poll");
      }
      if (count == 2 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
        return 0;
      }
      if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        return recv_some_internal(sn::util::span<std::uint8_t>(data, len));
      }
    }
#endif
  }

  socket_handle sock_{};
  cancellation_token cancellation_{};
};

class tcp_listener {
public:
  tcp_listener() = default;
  explicit tcp_listener(const endpoint& ep, bool reuse_address = true) {
    open(ep.family(), reuse_address);
    bind(ep);
  }

  tcp_listener(const tcp_listener&) = delete;
  tcp_listener& operator=(const tcp_listener&) = delete;

  tcp_listener(tcp_listener&&) noexcept = default;
  tcp_listener& operator=(tcp_listener&&) noexcept = default;

  ~tcp_listener() = default;

  void open(sa_family_t family, bool reuse_address = true) {
    detail::ensure_runtime();
    socket_handle fresh(detail::create_socket(family, SOCK_STREAM, IPPROTO_TCP));
    if (reuse_address) {
      detail::set_reuseaddr(fresh.get());
    }
    sock_ = std::move(fresh);
  }

  void bind(const endpoint& ep) {
    if (!sock_) {
      open(ep.family());
    }
    if (::bind(sock_.get(), ep.addr(), ep.length()) == detail::socket_error_value) {
      detail::throw_last_error("bind");
    }
  }

  void listen(int backlog = SOMAXCONN) {
    if (::listen(sock_.get(), backlog) == detail::socket_error_value) {
      detail::throw_last_error("listen");
    }
  }

  [[nodiscard]] tcp_stream accept() {
    sockaddr_storage storage{};
    socklen_t len = static_cast<socklen_t>(sizeof(storage));
    detail::native_socket client = ::accept(sock_.get(), reinterpret_cast<sockaddr*>(&storage), &len);
    if (client == detail::invalid_socket) {
      detail::throw_last_error("accept");
    }
    detail::set_nodelay(client, true);
    return tcp_stream::from_native(client);
  }

  [[nodiscard]] endpoint local_endpoint() const {
    sockaddr_storage storage{};
    socklen_t len = static_cast<socklen_t>(sizeof(storage));
    if (::getsockname(sock_.get(), reinterpret_cast<sockaddr*>(&storage), &len) == detail::socket_error_value) {
      detail::throw_last_error("getsockname");
    }
    return endpoint(reinterpret_cast<sockaddr*>(&storage), len);
  }

  [[nodiscard]] bool is_open() const noexcept { return static_cast<bool>(sock_); }

  void close() noexcept { sock_.reset(); }

  [[nodiscard]] detail::native_socket native_handle() const noexcept { return sock_.get(); }

private:
  socket_handle sock_{};
};

}
