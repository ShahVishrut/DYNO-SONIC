#include <chrono>
#include <iostream>
#include <memory>

#include "src/static/omap.h"
#include "src/store/posix_single_file_store.h"
#include "src/utils/crypto.h"
#include "src/utils/measurements.h"

using namespace dyno::crypto;
using namespace dyno::measurement;
using namespace dyno::static_path_omap;
using namespace dyno::store;

const static std::string test_name = "somap";

int main(int argc, char **argv) {
  Config conf(argc, argv);
  if (!conf.is_valid_)
    return 1;

  auto enc_key = GenerateKey();
  for (const auto &bs : conf.block_sizes_) {
    for (const auto &po2 : conf.po2s_) {
      Run total(test_name, po2, bs, conf.max_mem_level_);
      size_t size = 1UL << po2;
      for (int r = 0; r < conf.num_runs_; ++r) {
        Measurement prev;
        Run run(test_name, po2, bs);

        auto omap = std::make_unique<OMap>(
            size, bs, conf.store_path_, conf.max_mem_level_);
        if (omap->IsOnDisk())
          Uncache();
        run.alloc_.time_ = run.Elapsed();
        prev = {run.Elapsed(),
                omap->MemoryAccessCount(),
                omap->MemoryBytesMovedTotal()};

        omap->Insert(1, {}, enc_key);
        if (omap->IsOnDisk())
          Uncache();
        run.insert_.time_ = run.Elapsed() - prev.time_;
        run.insert_.accesses_ = omap->MemoryAccessCount() - prev.accesses_;
        run.insert_.bytes = omap->MemoryBytesMovedTotal() - prev.bytes;
        prev = {run.Elapsed(),
                omap->MemoryAccessCount(),
                omap->MemoryBytesMovedTotal()};

        omap->Read(1, enc_key);
        if (omap->IsOnDisk())
          Uncache();
        run.search_.time_ = run.Elapsed() - prev.time_;
        run.search_.accesses_ = omap->MemoryAccessCount() - prev.accesses_;
        run.search_.bytes = omap->MemoryBytesMovedTotal() - prev.bytes;
        prev = {run.Elapsed(),
                omap->MemoryAccessCount(),
                omap->MemoryBytesMovedTotal()};

        omap->ReadAndRemove(1, enc_key);
        if (omap->IsOnDisk())
          Uncache();
        run.delete_.time_ = run.Elapsed() - prev.time_;
        run.delete_.accesses_ = omap->MemoryAccessCount() - prev.accesses_;
        run.delete_.bytes = omap->MemoryBytesMovedTotal() - prev.bytes;

        run.is_on_disk_ = omap->IsOnDisk();
        if (r == 0)
          total.is_on_disk_ = run.is_on_disk_;
        total = total + run;
        omap.reset(); // cleanup
      }
      std::cout << (total / conf.num_runs_) << std::endl;
    }
  }
  return 0;
}
