

#pragma once

#include <cstddef>
#include <cstdint>

#include <type_traits>

#if defined(__clang__)
#define OBLIV_LEGACY_VECTORIZER_HINT _Pragma("clang loop vectorize(enable) interleave(enable)")
#elif defined(__GNUC__)
#define OBLIV_LEGACY_VECTORIZER_HINT _Pragma("GCC ivdep")
#else
#define OBLIV_LEGACY_VECTORIZER_HINT
#endif

namespace sn::obliv::legacy {

inline void ct_setc_byte(uint8_t* dest, uint8_t src, bool cond) {
  const uint8_t mask = ~((uint8_t) cond - 1);
  *dest ^= (src ^ *dest) & mask;
}

template <typename T> inline void ct_setc_num(T* out, T src, bool cond) {
  static_assert(std::is_integral_v<T>, "ct_setc: T must be integral");
  using U = std::make_unsigned_t<T>;
  auto* uout = reinterpret_cast<U*>(out);
  const U usrc = static_cast<U>(src);
  const U mask = ~((U) cond - 1);
  *uout ^= (usrc ^ *uout) & mask;
}

inline void ct_swapc_byte(uint8_t* __restrict a, uint8_t* __restrict b, bool cond) {
  const uint8_t mask = ~((uint8_t) cond - 1);
  *a ^= *b;
  *b ^= *a & mask;
  *a ^= *b;
}

template <typename T> inline void ct_swapc_num(T* __restrict a, T* __restrict b, bool cond) {
  static_assert(std::is_integral_v<T>, "ct_swapc: T must be integral");
  using U = std::make_unsigned_t<T>;
  auto* ua = reinterpret_cast<U*>(a);
  auto* ub = reinterpret_cast<U*>(b);
  const U mask = ~((U) cond - 1);
  *ua ^= *ub;
  *ub ^= *ua & mask;
  *ua ^= *ub;
}

template <typename T> inline void ct_swapc_mem_t(T* __restrict a, T* __restrict b, size_t n_words, bool cond) {
  static_assert(std::is_integral_v<T>, "ct_swapc_mem_t: T must be integral");
  OBLIV_LEGACY_VECTORIZER_HINT for (size_t i = 0; i < n_words; ++i) { ct_swapc_num(&a[i], &b[i], cond); }
}

template <typename T> inline bool ct_eq_setc_num(T a, T b) {
  static_assert(std::is_integral_v<T>, "ct_eq: T must be integral");
  uint8_t result = 0;
  ct_setc_byte(&result, 1, a == b);
  return static_cast<bool>(result);
}

template <typename T> inline bool ct_eq_setc_mem_t(const void* __restrict a, const void* __restrict b, size_t n_words) {
  static_assert(std::is_integral_v<T>, "ct_eq: T must be integral");
  const auto* pa = static_cast<const T*>(a);
  const auto* pb = static_cast<const T*>(b);
  uint8_t result = 1;
  OBLIV_LEGACY_VECTORIZER_HINT for (size_t i = 0; i < n_words; ++i) {
    uint8_t cmp = 0;
    ct_setc_byte(&cmp, 1, pa[i] == pb[i]);
    result &= cmp;
  }
  return static_cast<bool>(result);
}

template <typename T>
inline void ct_select_setc_mem_t(void* res, const void* a, const void* b, size_t n_words, bool cond) {
  static_assert(std::is_integral_v<T>, "ct_select: T must be integral");
  auto* pres = static_cast<T*>(res);
  const auto* pa = static_cast<const T*>(a);
  const auto* pb = static_cast<const T*>(b);
  OBLIV_LEGACY_VECTORIZER_HINT for (size_t i = 0; i < n_words; ++i) {
    pres[i] = pb[i];
    ct_setc_num(&pres[i], pa[i], cond);
  }
}

template <typename T>
inline void ct_select_setc_mem_t_noalias(
    void* __restrict res, const void* __restrict a, const void* __restrict b, size_t n_words, bool cond
) {
  static_assert(std::is_integral_v<T>, "ct_select: T must be integral");
  auto* pres = static_cast<T*>(res);
  const auto* pa = static_cast<const T*>(a);
  const auto* pb = static_cast<const T*>(b);
  OBLIV_LEGACY_VECTORIZER_HINT for (size_t i = 0; i < n_words; ++i) {
    pres[i] = pb[i];
    ct_setc_num(&pres[i], pa[i], cond);
  }
}

template <typename T> inline T ct_select_setc_num(const T& a, const T& b, bool cond) {
  T res = b;
  ct_setc_num(&res, a, cond);
  return res;
}

template <typename T>
inline void ct_swap_setc_mem_t(void* __restrict a, void* __restrict b, size_t n_words, bool cond) {
  ct_swapc_mem_t(static_cast<T*>(a), static_cast<T*>(b), n_words, cond);
}

template <typename T> inline void ct_swap_setc_num(T* a, T* b, bool cond) { ct_swapc_num(a, b, cond); }

template <typename T> inline int ct_cmp_setc_num(T a, T b) {
  int res = 0;
  ct_setc_num(&res, 1, a > b);
  ct_setc_num(&res, -1, a < b);
  return res;
}

template <typename T> inline bool ct_cmp_gt_setc_num(T a, T b) {
  uint8_t res = 0;
  ct_setc_byte(&res, 1, a > b);
  return static_cast<bool>(res);
}

template <typename T> inline bool ct_cmp_lt_setc_num(T a, T b) {
  uint8_t res = 0;
  ct_setc_byte(&res, 1, a < b);
  return static_cast<bool>(res);
}

template <typename T> inline bool ct_cmp_le_setc_num(const T& a, const T& b) {
  uint8_t res = 0;
  ct_setc_byte(&res, 1, a <= b);
  return static_cast<bool>(res);
}

inline uint64_t ct_count_leading_zeros(uint64_t x) {
  constexpr uint64_t zero = 0;
  const bool is_zero = x == zero;
  const uint64_t safe_input = ct_select_setc_num(~zero, x, is_zero);
  uint64_t clz = __builtin_clzll(safe_input);
  clz = ct_select_setc_num(uint64_t(64), clz, is_zero);
  return clz;
}

}

#undef OBLIV_LEGACY_VECTORIZER_HINT
