#ifndef DYNO_STORE_HYBRID_STORE_H_
#define DYNO_STORE_HYBRID_STORE_H_

#include "store.h"

#include <memory>
#include <utility>

#include "ram_store.h"
#include "posix_single_file_store.h"

namespace dyno::store {

class HybridStore : public Store {
 public:
  HybridStore(std::vector<std::unique_ptr<Store>> stores,
              std::vector<size_t> bounds)
      : stores_(std::move(stores)), bounds_(std::move(bounds)) {
    assert(!stores_.empty());
    assert(stores_.size() == bounds_.size());
  }

  uint8_t *Read(size_t i) override {
    auto s_idx = FindStore(i);
    if (s_idx == -1)
      return nullptr;
    return stores_[s_idx]->Read(AddressInStore(i, s_idx));
  }

  bool Write(size_t i, const uint8_t *d) override {
    auto s_idx = FindStore(i);
    if (s_idx == -1)
      return false;
    return stores_[s_idx]->Write(AddressInStore(i, s_idx), d);
  }

 protected:
  std::vector<std::unique_ptr<Store>> stores_;
  std::vector<size_t> bounds_;

 private:
  int FindStore(size_t i) {
    for (int idx = 0; idx < bounds_.size(); ++idx)
      if (i < bounds_[idx])
        return idx;
    return -1;
  }

  size_t AddressInStore(size_t i, int idx) {
    if (idx == 0)
      return i;
    return i - bounds_[idx - 1];
  }
};

} // namespace dyno::store

#endif //DYNO_STORE_HYBRID_STORE_H_
