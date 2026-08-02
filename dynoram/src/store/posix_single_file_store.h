#ifndef DYNO_STORE_POSIX_SINGLE_FILE_STORE_H_
#define DYNO_STORE_POSIX_SINGLE_FILE_STORE_H_

#include "store.h"

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <iostream>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dyno::store {

static void Uncache() {
  sync();
  int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  write(fd, "3", 1);
  close(fd);
}

const int kFileFlags = O_CREAT | O_RDWR;
const int kFilePerms = 0600;
constexpr size_t kReadBuffSize = 1UL << 30;
constexpr size_t kWriteBuffSize = 1UL << 30;

class PosixSingleFileStore : public Store {
 public:
  static std::optional<PosixSingleFileStore *> Construct(
      size_t n, size_t entry_size,
      const std::filesystem::path &p, bool truncate = false) {
    auto res = new PosixSingleFileStore(n, entry_size, p, truncate);
    if (!res->setup_successful_) return std::nullopt;
    return res;
  }

  // The return value is valid until the next Read call.
  uint8_t *Read(size_t i) override {
    size_t read_bytes = 0;
    while (read_bytes < entry_size_) {
      size_t to_read = kReadBuffSize;
      if (read_bytes + to_read > entry_size_) {
        to_read = entry_size_ - read_bytes;
      }
      size_t file_offset = (i * entry_size_) + read_bytes;
      auto read_res = ::pread(
          file_, read_buff_.get() + read_bytes, to_read, file_offset);
      if (read_res == -1) {
        std::clog << "Couldn't read file; fd=" << file_ << ", path=" << path_
                  << ", prev offset=" << read_bytes
                  << ", tried to read " << to_read << " more"
                  << "; errno=" << errno << std::endl;
        return {};
      }
      read_bytes += to_read;
    }
    return read_buff_.get();
  }

  bool Write(size_t i, const uint8_t *d) override {
    size_t written_bytes = 0;
    while (written_bytes < entry_size_) {
      size_t to_write = kWriteBuffSize;
      if (written_bytes + to_write > entry_size_) {
        to_write = entry_size_ - written_bytes;
      }
      size_t file_offset = (i * entry_size_) + written_bytes;
      auto write_res = ::pwrite(
          file_, d + written_bytes, to_write, file_offset);
      if (write_res == -1) {
        std::clog << "Couldn't write to file; fd=" << file_
                  << ", path=" << path_
                  << ", prev offset=" << written_bytes
                  << ", tried to write " << to_write << " more"
                  << "; errno=" << errno << std::endl;
        return false;
      }
      written_bytes += to_write;
    }
    return true;
  }

  ~PosixSingleFileStore() override {
    ::close(file_);
  }

 private:
  PosixSingleFileStore(size_t n, size_t entry_size,
                       const std::filesystem::path &p, bool truncate = false)
      : n_(n), entry_size_(entry_size) {

    std::error_code ec;
    auto wc = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
      std::clog << "Failed to make path [" << p << "] "
                << "weakly canonical."
                << " Error code: " << ec.value() << " - " << ec.message()
                << std::endl;
      return;
    }
    path_ = wc;

    bool exists = std::filesystem::exists(path_, ec);
    if (ec) {
      std::clog << "Failed to check whether path [" << path_ << "] "
                << "exists."
                << " Error code: " << ec.value() << " - " << ec.message()
                << std::endl;
      return;
    }
    if (exists) {
      bool is_reg_file = std::filesystem::is_regular_file(path_, ec);
      if (ec) {
        std::clog << "Failed to check whether path [" << path_ << "] "
                  << "is a regular file."
                  << " Error code: " << ec.value() << " - " << ec.message()
                  << std::endl;
        return;
      }
      if (!is_reg_file) {
        std::clog << "Requested path exists and isn't a regular file."
                  << std::endl;
        return;
      }
    }
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
      std::clog << "Failed to create parent path [" << path_.parent_path()
                << "]."
                << " Error code: " << ec.value() << " - " << ec.message()
                << std::endl;
      return;
    }

    int flags = kFileFlags;
    if (exists) {
      size_t curr_size = std::filesystem::file_size(path_, ec);
      if (ec) {
        std::clog << "Failed to check the size of [" << path_ << "]."
                  << " Error code: " << ec.value() << " - " << ec.message()
                  << std::endl;
        return;
      }
      if (curr_size != TotalSize() || truncate) {
        flags |= O_TRUNC;
      }
    }

    file_ = ::open(path_.c_str(), flags, kFilePerms);
    if (file_ == -1) {
      std::clog << "Couldn't open file; errno=" << errno << std::endl;
      return;
    }

    read_buff_ = std::make_unique<uint8_t[]>(entry_size_);

    setup_successful_ = true;
  }

  size_t n_;
  size_t entry_size_;
  std::unique_ptr<uint8_t[]> read_buff_;
  std::filesystem::path path_;
  int file_;
  bool setup_successful_ = false;
  size_t TotalSize() { return n_ * entry_size_; }
};
} // namespace dyno::store

#endif //DYNO_STORE_POSIX_SINGLE_FILE_STORE_H_
