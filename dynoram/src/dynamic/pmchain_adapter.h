#ifndef DYNO_DYNAMIC_ORAM_PMCHAIN_ADAPTER_H_
#define DYNO_DYNAMIC_ORAM_PMCHAIN_ADAPTER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/static/oram.h"

namespace dyno::dynamic_stepping_path_oram {

class PMChainAdapter {
 public:
  PMChainAdapter(size_t capacity, size_t val_len, size_t max_batch_size = 100000);
  ~PMChainAdapter();

  struct AccessOp {
      static_path_oram::Key key;
      bool is_real;
      uint8_t op_type; // 1=Search, 2=Delete, 3=Update, 0=Insert
  };

  // Perform a full PMChain batch. Pads batch to max_batch_size if smaller.
  double SpinlockSonicBenchmark(int work_type, size_t batch_size, bool steady_state = false);
  double RawSonicBenchmark(int work_type, size_t batch_size);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  size_t capacity_;
  size_t val_len_;
  size_t max_batch_size_;
};

} // namespace dyno::dynamic_stepping_path_oram

#endif // DYNO_DYNAMIC_ORAM_PMCHAIN_ADAPTER_H_
