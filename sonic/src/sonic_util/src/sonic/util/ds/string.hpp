#pragma once

#include "sonic/util/ds/vector.hpp"

#include <cstddef>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace sn::ds {

class string {
public:
  using size_type = std::size_t;

  string() { set_length(0); }

  string(const char* data, size_type length) { assign(data, length); }

  string(const char* c_str) {
    if (!c_str) {
      throw std::invalid_argument("sn::ds::string: null pointer");
    }
    assign(c_str, std::strlen(c_str));
  }

  string(const std::string& str) : string(str.data(), str.size()) {}

  string(const string&) = default;
  string(string&&) noexcept = default;

  string& operator=(const string&) = default;
  string& operator=(string&&) noexcept = default;

  [[nodiscard]] size_type size() const noexcept { return length_; }
  [[nodiscard]] bool empty() const noexcept { return length_ == 0; }

  [[nodiscard]] const char* data() const noexcept { return buffer_.data(); }
  [[nodiscard]] const char* c_str() const noexcept { return buffer_.data(); }

  void reserve(size_type capacity) { buffer_.reserve(capacity + 1); }

  string& clear() {
    set_length(0);
    return *this;
  }

  string& assign(const char* data, size_type length) {
    if (length == 0) {
      return clear();
    }
    if (!data) {
      throw std::invalid_argument("sn::ds::string: null data for non-zero length");
    }
    ensure_capacity(length);
    buffer_.resize(length + 1);
    std::memcpy(buffer_.data(), data, length);
    length_ = length;
    buffer_[length_] = '\0';
    return *this;
  }

  string& append(const char* data, size_type length) {
    if (length == 0) {
      return *this;
    }
    if (!data) {
      throw std::invalid_argument("sn::ds::string: null data for append");
    }
    const size_type new_length = length_ + length;
    ensure_capacity(new_length);
    buffer_.resize(new_length + 1);
    std::memcpy(buffer_.data() + length_, data, length);
    length_ = new_length;
    buffer_[length_] = '\0';
    return *this;
  }

  string& append(const string& rhs) { return append(rhs.data(), rhs.size()); }

  string& append(const char* c_str) {
    if (!c_str) {
      throw std::invalid_argument("sn::ds::string: null pointer for append");
    }
    return append(c_str, std::strlen(c_str));
  }

  string& append(char ch) { return append(&ch, 1); }

  std::string to_std() const { return std::string(data(), size()); }

  operator std::string() const { return to_std(); }

  bool operator==(const string& other) const noexcept {
    if (length_ != other.length_) {
      return false;
    }
    return std::memcmp(data(), other.data(), length_) == 0;
  }

  bool operator!=(const string& other) const noexcept { return !(*this == other); }

  bool operator==(const char* rhs) const noexcept {
    if (rhs == nullptr) {
      return length_ == 0;
    }
    return std::strcmp(data(), rhs) == 0;
  }

  bool operator!=(const char* rhs) const noexcept { return !(*this == rhs); }

  friend bool operator==(const char* lhs, const string& rhs) { return rhs == lhs; }
  friend bool operator!=(const char* lhs, const string& rhs) { return rhs != lhs; }

  friend std::ostream& operator<<(std::ostream& os, const string& str) {
    os << str.c_str();
    return os;
  }

  void swap(string& other) noexcept {
    buffer_.swap(other.buffer_);
    std::swap(length_, other.length_);
  }

private:
  vector<char> buffer_;
  size_type length_ = 0;

  void ensure_capacity(size_type required_length) {
    if (buffer_.capacity() < required_length + 1) {
      buffer_.reserve(required_length + 1);
    }
  }

  void set_length(size_type new_length) {
    length_ = new_length;
    buffer_.resize(length_ + 1);
    buffer_[length_] = '\0';
  }
};

}
