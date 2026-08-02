#ifndef DYNO_STORE_RAM_STORE_H_
#define DYNO_STORE_RAM_STORE_H_

#include "store.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

namespace dyno::store {

class RamStore : public Store {
 public:
  RamStore(size_t n, size_t entry_size)
      : n_(n), entry_size_(entry_size), data_(new uint8_t[n * entry_size]) {}

  uint8_t *Read(size_t i) {
    if (i >= n_)
      return {};
    return data_.get() + (i * entry_size_);
  }

  bool Write(size_t i, const uint8_t *d) {
    if ((i >= n_))
      return false;
    std::copy_n(d, entry_size_, data_.get() + (i * entry_size_));
    return true;
  }

 protected:
  size_t n_;
  size_t entry_size_;
  std::unique_ptr<uint8_t[]> data_;
};

} // namespace dyno::store

#endif //DYNO_STORE_RAM_STORE_H_
