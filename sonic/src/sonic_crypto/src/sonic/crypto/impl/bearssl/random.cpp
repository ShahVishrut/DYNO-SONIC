#include "sonic/crypto/random.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__APPLE__)
#include <stdlib.h>
#endif

#if defined(SONIC_CRYPTO_SGX) && SONIC_CRYPTO_SGX
#include <sgx_trts.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#include "sonic/crypto/error.hpp"

namespace sn::crypto {

namespace {

#if defined(SONIC_CRYPTO_SGX) && SONIC_CRYPTO_SGX

void read_entropy_impl(std::uint8_t* dst, std::size_t len) {
  if (len == 0) {
    return;
  }

  const sgx_status_t rc = sgx_read_rand(dst, len);
  if (rc != SGX_SUCCESS) {
    throw error("sgx_read_rand");
  }
}

#else

void read_from_fd(int fd, std::uint8_t* dst, std::size_t len) {
  std::size_t remaining = len;
  while (remaining > 0) {
    const ssize_t got = ::read(fd, dst, remaining);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw error("/dev/urandom read");
    }
    if (got == 0) {
      throw error("/dev/urandom eof");
    }
    dst += static_cast<std::size_t>(got);
    remaining -= static_cast<std::size_t>(got);
  }
}

void read_via_dev_urandom(std::uint8_t* dst, std::size_t len) {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throw error("open /dev/urandom");
  }

  read_from_fd(fd, dst, len);
  ::close(fd);
}

#if defined(__linux__)

void read_entropy_impl(std::uint8_t* dst, std::size_t len) {
  if (len == 0) {
    return;
  }

  std::size_t remaining = len;
  std::uint8_t* cursor = dst;

  while (remaining > 0) {
    const ssize_t got = ::getrandom(cursor, remaining, 0);
    if (got < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      if (errno == ENOSYS) {
        read_via_dev_urandom(cursor, remaining);
        return;
      }
      throw error("getrandom");
    }
    cursor += static_cast<std::size_t>(got);
    remaining -= static_cast<std::size_t>(got);
  }
}

#elif defined(__APPLE__)

void read_entropy_impl(std::uint8_t* dst, std::size_t len) {
  if (len == 0) {
    return;
  }
  ::arc4random_buf(dst, len);
}

#else

void read_entropy_impl(std::uint8_t* dst, std::size_t len) { read_via_dev_urandom(dst, len); }

#endif

#endif

}

void random_device::fill(sn::util::span<std::uint8_t> out) const { fill(out.data(), out.size()); }

void random_device::fill(std::uint8_t* dst, std::size_t len) const {
  if (len == 0) {
    return;
  }
  read_entropy_impl(dst, len);
}

}
