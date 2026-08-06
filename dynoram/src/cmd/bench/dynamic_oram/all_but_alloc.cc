#include <chrono>
#include <iostream>
#include <string>

#include "src/dynamic/oram.h"
#include "src/utils/crypto.h"
#include "src/utils/measurements.h"

using namespace dyno::crypto;
using namespace dyno::measurement;
using namespace dyno::dynamic_stepping_path_oram;

const static std::string test_name = "doram";

int main(int argc, char **argv) {
  try {
    Config conf(argc, argv);
    if (!conf.is_valid_)
      return 1;

    auto enc_key = GenerateKey();
    for (const auto &bs : conf.block_sizes_) {
      for (const auto &po2 : conf.po2s_) {
        Run total(test_name, po2, bs);
        size_t size = 1UL << po2;
        for (int r = 0; r < conf.num_runs_; ++r) {
          Measurement prev;
          Run run(test_name, po2, bs);

          auto oram = std::make_unique<ORam>(po2, bs);
          run.alloc_.time_ = run.Elapsed();
          prev = {run.Elapsed(),
                  oram->MemoryAccessCount(),
                  oram->MemoryBytesMovedTotal()};

          oram->Grow(enc_key);
          run.init_.time_ = run.Elapsed() - prev.time_;
          run.init_.accesses_ = oram->MemoryAccessCount() - prev.accesses_;
          run.init_.bytes = oram->MemoryBytesMovedTotal() - prev.bytes;
          prev = {run.Elapsed(),
                  oram->MemoryAccessCount(),
                  oram->MemoryBytesMovedTotal()};

          oram->Insert(1, {}, enc_key);
          run.insert_.time_ = run.Elapsed() - prev.time_;
          run.insert_.accesses_ = oram->MemoryAccessCount() - prev.accesses_;
          run.insert_.bytes = oram->MemoryBytesMovedTotal() - prev.bytes;
          prev = {run.Elapsed(),
                  oram->MemoryAccessCount(),
                  oram->MemoryBytesMovedTotal()};

          oram->Read(1, enc_key);
          run.search_.time_ = run.Elapsed() - prev.time_;
          run.search_.accesses_ = oram->MemoryAccessCount() - prev.accesses_;
          run.search_.bytes = oram->MemoryBytesMovedTotal() - prev.bytes;
          prev = {run.Elapsed(),
                  oram->MemoryAccessCount(),
                  oram->MemoryBytesMovedTotal()};

          oram->ReadAndRemove(1, enc_key);
          run.delete_.time_ = run.Elapsed() - prev.time_;
          run.delete_.accesses_ = oram->MemoryAccessCount() - prev.accesses_;
          run.delete_.bytes = oram->MemoryBytesMovedTotal() - prev.bytes;

          total = total + run;
          oram.reset(); // cleanup
        }
        std::cout << (total / conf.num_runs_) << std::endl;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "EXCEPTION CAUGHT: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
