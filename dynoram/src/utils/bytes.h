#ifndef DYNO_UTILS_BYTES_H_
#define DYNO_UTILS_BYTES_H_

#include <array>

namespace dyno::bytes {

template<typename T>
inline std::array<uint8_t, sizeof(T)> ToBytes(const T &object) {
  std::array<uint8_t, sizeof(T)> bytes;

  const auto begin = reinterpret_cast<const uint8_t *> (std::addressof(object));
  const auto end = begin + sizeof(T);
  std::copy(begin, end, bytes.begin());

  return bytes;
}

template<typename T>
T &FromBytes(const std::array<uint8_t, sizeof(T)> &bytes, T &object) {
  auto begin_object = reinterpret_cast<uint8_t *> (std::addressof(object));
  std::copy(std::begin(bytes), std::end(bytes), begin_object);

  return object;
}

template<typename T>
T &FromBytes(const uint8_t *bytes, T &object) {
  auto begin_object = reinterpret_cast<uint8_t *> (std::addressof(object));
  std::copy(bytes, bytes + sizeof(object), begin_object);

  return object;
}

} // namespace dyno::bytes

#endif //DYNO_UTILS_BYTES_H_
