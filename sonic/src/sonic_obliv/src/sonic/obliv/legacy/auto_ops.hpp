#pragma once

#include <type_traits>

#include "sonic/util/cpp20.hpp"
#include "sonic/obliv/legacy/ct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"

namespace sn::obliv::legacy {

using Choice = ::sn::obliv::choice;

template <typename T> __attribute__((always_inline)) inline Choice ct_eq(T a, T b) {
  static_assert(std::is_integral_v<T>, "ct_eq: T must be integral");
  return Choice(ct_eq_setc_num(a, b));
}

template <typename T>
__attribute__((always_inline)) inline Choice ct_eq_mem(const T* __restrict a, const T* __restrict b, size_t n) {
  return Choice(ct_eq_setc_mem_t<T>(a, b, n));
}

template <typename T>
__attribute__((always_inline)) inline Choice ct_eq_data(const T* __restrict a, const T* __restrict b) {
  static_assert(alignof(T) % 8 == 0, "ct_eq_data: alignof(T) must be 8-byte aligned");
  static_assert(sizeof(T) % 8 == 0, "ct_eq_data: sizeof(T) must be 8-byte aligned");
  auto* a64 = std::bit_cast<uint64_t*>(a);
  auto* b64 = std::bit_cast<uint64_t*>(b);
  constexpr size_t n64 = sizeof(T) / sizeof(uint64_t);
  return ct_eq_mem<uint64_t>(a64, b64, n64);
}

template <typename T>
__attribute__((always_inline)) inline Choice ct_eq_ref(const T& __restrict a, const T& __restrict b) {
  return ct_eq_data(&a, &b);
}

template <typename T> __attribute__((always_inline)) inline void ct_set(T* a, T b, Choice cond) {
  static_assert(std::is_integral_v<T>, "ct_set: T must be integral");
  ct_setc_num(a, b, cond.unwrap());
}

template <typename T> __attribute__((always_inline)) inline void ct_set_ref(T& a, T b, Choice cond) {
  ct_set(&a, b, cond);
}

template <typename T> __attribute__((always_inline)) inline T ct_select(T a, T b, Choice cond) {
  static_assert(std::is_integral_v<T>, "ct_select: T must be integral");
  return ct_select_setc_num(a, b, cond.unwrap());
}

template <typename T>
__attribute__((always_inline)) inline void ct_select_mem(T* out, const T* a, const T* b, size_t n, Choice cond) {
  ct_select_setc_mem_t<T>(out, a, b, n, cond.unwrap());
}

template <typename T>
__attribute__((always_inline)) inline void ct_select_mem_noalias(
    T* __restrict out, const T* __restrict a, const T* __restrict b, size_t n, Choice cond
) {
  ct_select_setc_mem_t_noalias<T>(out, a, b, n, cond.unwrap());
}

template <typename T>
__attribute__((always_inline)) inline void ct_select_data(
    T* out, const T* __restrict a, const T* __restrict b, Choice cond
) {
  static_assert(alignof(T) % 8 == 0, "ct_select_data: alignof(T) must be 8-byte aligned");
  static_assert(sizeof(T) % 8 == 0, "ct_select_data: sizeof(T) must be 8-byte aligned");
  auto* out64 = std::bit_cast<uint64_t*>(out);
  auto* a64 = std::bit_cast<uint64_t*>(a);
  auto* b64 = std::bit_cast<uint64_t*>(b);
  constexpr size_t n64 = sizeof(T) / sizeof(uint64_t);
  ct_select_mem<uint64_t>(out64, a64, b64, n64, cond);
}

template <typename T>
__attribute__((always_inline)) inline T ct_select_ref(const T& __restrict a, const T& __restrict b, Choice cond) {
  T out;
  ct_select_data(&out, &a, &b, cond);
  return out;
}

template <typename T>
__attribute__((always_inline)) inline void ct_swap(T* __restrict a, T* __restrict b, Choice cond) {
  static_assert(std::is_integral_v<T>, "ct_swap: T must be integral");
  ct_swap_setc_num(a, b, cond.unwrap());
}

template <typename T>
__attribute__((always_inline)) inline void ct_swap_mem(T* __restrict a, T* __restrict b, size_t n, Choice cond) {
  ct_swap_setc_mem_t<T>(a, b, n, cond.unwrap());
}

template <typename T>
__attribute__((always_inline)) inline void ct_swap_data(T* __restrict a, T* __restrict b, Choice cond) {
  static_assert(std::is_trivially_copyable_v<T>, "ct_swap_data: T must be trivially copyable");
  if constexpr (sizeof(T) < 8) {
    static_assert(!std::is_void_v<T> && sizeof(T) > 0, "ct_swap_data: invalid size");
    auto* a8 = reinterpret_cast<uint8_t*>(a);
    auto* b8 = reinterpret_cast<uint8_t*>(b);
    ct_swap_mem<uint8_t>(a8, b8, sizeof(T), cond);
  } else {
    static_assert(alignof(T) % 8 == 0, "ct_swap_data: alignof(T) must be 8-byte aligned");
    static_assert(sizeof(T) % 8 == 0, "ct_swap_data: sizeof(T) must be 8-byte aligned");
    auto* a64 = std::bit_cast<uint64_t*>(a);
    auto* b64 = std::bit_cast<uint64_t*>(b);
    constexpr size_t n64 = sizeof(T) / sizeof(uint64_t);
    ct_swap_mem<uint64_t>(a64, b64, n64, cond);
  }
}

template <typename T> __attribute__((always_inline)) inline Choice ct_gt(T a, T b) {
  static_assert(std::is_integral_v<T>, "ct_gt: T must be integral");
  return Choice(ct_cmp_gt_setc_num(a, b));
}

template <typename T> __attribute__((always_inline)) inline Choice ct_lt(T a, T b) {
  static_assert(std::is_integral_v<T>, "ct_lt: T must be integral");
  return Choice(ct_cmp_lt_setc_num(a, b));
}

template <typename T> __attribute__((always_inline)) inline Choice ct_le(const T& a, const T& b) {
  static_assert(std::is_integral_v<T>, "ct_le: T must be integral");
  return Choice(ct_cmp_le_setc_num(a, b));
}

template <typename T> __attribute__((always_inline)) inline int ct_cmp(T a, T b) {
  static_assert(std::is_integral_v<T>, "ct_cmp: T must be integral");
  return ct_cmp_setc_num(a, b);
}

template <typename T> __attribute__((always_inline)) inline T ct_div_pow2(T a, uint8_t log2_divisor) {
  static_assert(std::is_integral_v<T>, "ct_div_pow2: T must be integral");
  return a >> log2_divisor;
}

template <typename T> __attribute__((always_inline)) inline T ct_mod_pow2(T a, uint8_t log2_divisor) {
  static_assert(std::is_integral_v<T>, "ct_mod_pow2: T must be integral");
  const T mask = (T(1) << log2_divisor) - 1;
  return a & mask;
}

template <typename T> __attribute__((always_inline)) inline T ct_mul_pow2(T a, uint8_t log2_multiplier) {
  static_assert(std::is_integral_v<T>, "ct_mul_pow2: T must be integral");
  return a << log2_multiplier;
}

template <typename T> __attribute__((always_inline)) inline T ct_pow2(uint8_t exponent) {
  static_assert(std::is_integral_v<T>, "ct_pow2: T must be integral");
  return T(1) << exponent;
}

__attribute__((always_inline)) inline int ct_log2(uint64_t value) {
  if (value == 0) {
    return -1;
  }
  return 63 - __builtin_clzll(value);
}

__attribute__((always_inline)) inline uint64_t ct_madd(uint64_t a, uint64_t b, uint64_t c) { return a * b + c; }

}
