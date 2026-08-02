#pragma once
#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace sn::util::demo {

template <typename T, T... Values> struct static_param_list {
  using value_type = T;
  static constexpr std::size_t count = sizeof...(Values);
  static constexpr std::array<value_type, count> values = {Values...};
};

template <typename T, T N> using static_param_tag = std::integral_constant<T, N>;

template <typename T, T... Values, typename F>
bool dispatch_static_param_impl(static_param_list<T, Values...>, T runtime_value, F&& func) {
  return ((runtime_value == Values && (func(static_param_tag<T, Values>{}), true)) || ...);
}

template <typename ParamList, typename F>
bool dispatch_static_param(typename ParamList::value_type runtime_value, F&& func) {
  return dispatch_static_param_impl(ParamList{}, runtime_value, std::forward<F>(func));
}

template <typename T, T... Values> constexpr bool is_supported_param(static_param_list<T, Values...>, T value) {
  return ((value == Values) || ...);
}

template <typename T, std::size_t N> std::string format_supported_params(const std::array<T, N>& params) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < N; ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << params[i];
  }
  return oss.str();
}

template <std::size_t... Sizes> using block_size_list = static_param_list<std::size_t, Sizes...>;

template <std::size_t N> using block_size_tag = static_param_tag<std::size_t, N>;

template <std::size_t... Sizes, typename F>
bool dispatch_block_size_impl(block_size_list<Sizes...> list, std::size_t runtime_size, F&& func) {
  return dispatch_static_param_impl(list, runtime_size, std::forward<F>(func));
}

template <typename SizeList, typename F> bool dispatch_block_size(std::size_t runtime_size, F&& func) {
  return dispatch_block_size_impl(SizeList{}, runtime_size, std::forward<F>(func));
}

template <std::size_t... Sizes> constexpr bool is_supported_size(block_size_list<Sizes...> list, std::size_t size) {
  return is_supported_param(list, size);
}

template <std::size_t N> std::string format_supported_sizes(const std::array<std::size_t, N>& sizes) {
  return format_supported_params(sizes);
}

}
