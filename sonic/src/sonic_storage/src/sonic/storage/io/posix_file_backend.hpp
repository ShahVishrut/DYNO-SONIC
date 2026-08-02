#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/uio.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "sonic/util/log.hpp"

#include "sonic/storage/io/backend.hpp"

namespace sn::storage::io {

struct posix_file_config {
  std::string path;
  bool create = true;
  bool truncate = true;
  bool direct_io = false;
  bool drop_cache_after_flush = false;

  bool allow_prefetch = true;
};

class posix_file_backend : public backend {
public:
  explicit posix_file_backend(posix_file_config cfg) : cfg_(std::move(cfg)) {
    int flags = O_RDWR;
    if (cfg_.create) {
      flags |= O_CREAT;
    }
    if (cfg_.truncate) {
      flags |= O_TRUNC;
    }
    if (cfg_.direct_io) {
#ifdef O_DIRECT
      flags |= O_DIRECT;
#endif
    }
    fd_ = ::open(cfg_.path.c_str(), flags, 0666);
    sn::util::log::ensure(fd_ >= 0, "posix_file_backend: failed to open file");
#if defined(POSIX_FADV_RANDOM)
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
#endif
  }

  ~posix_file_backend() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  posix_file_backend(const posix_file_backend&) = delete;
  posix_file_backend& operator=(const posix_file_backend&) = delete;

  posix_file_backend(posix_file_backend&& other) noexcept : cfg_(std::move(other.cfg_)), fd_(other.fd_) {
    other.fd_ = -1;
  }

  posix_file_backend& operator=(posix_file_backend&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        ::close(fd_);
      }
      cfg_ = std::move(other.cfg_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  void read_page(std::uint64_t page_id, void* dst, std::size_t bytes) override {
    const off_t offset = static_cast<off_t>(page_id * bytes);
    ssize_t rc = ::pread(fd_, dst, bytes, offset);
    const int err = errno;
    if (rc == static_cast<ssize_t>(bytes)) {
      return;
    }
    if (rc >= 0) {
      const std::size_t got = static_cast<std::size_t>(rc);
      std::memset(static_cast<std::byte*>(dst) + got, 0, bytes - got);
      const off_t last = offset + static_cast<off_t>(bytes) - 1;
      std::byte z{};
      ssize_t wr = ::pwrite(fd_, &z, 1, last);
      const int werr = errno;
      sn::util::log::ensuref(wr == 1, "file io", wr, static_cast<intmax_t>(last), werr, std::strerror(werr));
      return;
    }
    sn::util::log::failf("file read", rc, bytes, static_cast<intmax_t>(offset), err, std::strerror(err));
  }

  void write_page(std::uint64_t page_id, const void* src, std::size_t bytes) override {
    const off_t offset = static_cast<off_t>(page_id * bytes);
    ssize_t rc = ::pwrite(fd_, src, bytes, offset);
    const int err = errno;
    sn::util::log::ensuref(
        rc == static_cast<ssize_t>(bytes), "file write", rc, bytes, static_cast<intmax_t>(offset), err, std::strerror(err)
    );
  }

  void hint_prefetch(std::uint64_t page_id, std::size_t bytes) override {
#if defined(POSIX_FADV_WILLNEED)
    if (!cfg_.allow_prefetch) {
      return;
    }
    const off_t offset = static_cast<off_t>(page_id * bytes);
    ::posix_fadvise(fd_, offset, static_cast<off_t>(bytes), POSIX_FADV_WILLNEED);
#else
    (void) page_id;
    (void) bytes;
#endif
  }

  void flush() override {
#if defined(__APPLE__)
    int rc = ::fcntl(fd_, F_FULLFSYNC);
#else
    int rc = ::fdatasync(fd_);
#endif
    if (rc != 0) {
      const int err = errno;
      sn::util::log::failf("file flush", err, std::strerror(err));
    }
#if defined(POSIX_FADV_DONTNEED)
    if (cfg_.drop_cache_after_flush) {
      ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
    }
#endif
  }

  void drop_cache() override {
#if defined(POSIX_FADV_DONTNEED)
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
#endif
  }

  void resize(std::uint64_t pages, std::size_t bytes_per_page) override {
    if (fd_ < 0) {
      return;
    }
    const off_t desired = static_cast<off_t>(pages * bytes_per_page);
    int rc = ::ftruncate(fd_, desired);
    const int err = errno;
    sn::util::log::ensuref(rc == 0, "file resize", static_cast<intmax_t>(desired), err, std::strerror(err));
#if defined(FALLOC_FL_KEEP_SIZE)

    ::fallocate(fd_, 0, 0, desired);
#endif
  }

  void write_pages(sn::util::span<const page_view> pages) override {
    if (pages.empty()) {
      return;
    }

    std::size_t run_start = 0;
    while (run_start < pages.size()) {
      const std::uint64_t start_page = pages[run_start].page_id;
      const std::size_t bytes = pages[run_start].bytes;
      std::size_t run_end = run_start + 1;
      while (run_end < pages.size() && pages[run_end].bytes == bytes &&
             pages[run_end].page_id == pages[run_end - 1].page_id + 1) {
        ++run_end;
      }
      const std::size_t run_len = run_end - run_start;
      const off_t offset = static_cast<off_t>(start_page * bytes);

      if (run_len == 1) {
        ssize_t rc = ::pwrite(fd_, pages[run_start].src, bytes, offset);
        const int err = errno;
        sn::util::log::ensuref(
            rc == static_cast<ssize_t>(bytes), "file write", rc, bytes, static_cast<intmax_t>(offset), err,
            std::strerror(err)
        );
      } else {
        std::vector<struct iovec> iovecs;
        iovecs.reserve(run_len);
        for (std::size_t i = run_start; i < run_end; ++i) {
          iovecs.push_back({const_cast<void*>(pages[i].src), bytes});
        }
        ssize_t rc = ::pwritev(fd_, iovecs.data(), static_cast<int>(iovecs.size()), offset);
        const int err = errno;
        sn::util::log::ensuref(
            rc == static_cast<ssize_t>(bytes * iovecs.size()), "file write", rc, bytes * iovecs.size(),
            static_cast<intmax_t>(offset), err, std::strerror(err)
        );
      }

      run_start = run_end;
    }
  }

private:
  posix_file_config cfg_{};
  int fd_ = -1;
};

}
