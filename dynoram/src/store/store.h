#ifndef DYNO_STORE_STORE_H_
#define DYNO_STORE_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace dyno::store {

class Store {
 public:
  virtual ~Store() = default;
  virtual uint8_t *Read(size_t i) = 0;
  virtual bool Write(size_t i, const uint8_t *data) = 0;
};

} // namespace dyno::store

#endif //DYNO_STORE_STORE_H_
