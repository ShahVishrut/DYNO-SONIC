#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sn::demo::types {

template <std::size_t Capacity> class string_buffer {
public:
  constexpr string_buffer() = default;

  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return length_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return length_ == 0; }

  void clear() noexcept {
    length_ = 0;
    storage_[0] = '\0';
  }

  [[nodiscard]] const char* c_str() const noexcept { return storage_; }
  [[nodiscard]] std::string_view view() const noexcept { return std::string_view(storage_, length_); }

  bool assign(std::string_view text) noexcept {
    if (text.size() > capacity()) {
      return false;
    }
    if (!text.empty()) {
      std::memcpy(storage_, text.data(), text.size());
    }
    storage_[text.size()] = '\0';
    length_ = static_cast<std::uint32_t>(text.size());
    return true;
  }

  bool append(std::string_view text) noexcept {
    if (text.empty()) {
      return true;
    }
    if (length_ + text.size() > capacity()) {
      return false;
    }
    std::memcpy(storage_ + length_, text.data(), text.size());
    length_ += static_cast<std::uint32_t>(text.size());
    storage_[length_] = '\0';
    return true;
  }

private:
  char storage_[Capacity + 1]{};
  std::uint32_t length_{0};
};

}
