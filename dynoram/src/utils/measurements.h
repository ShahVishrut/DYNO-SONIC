#ifndef DYNO_UTILS_MEASUREMENTS_H_
#define DYNO_UTILS_MEASUREMENTS_H_

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

namespace dyno::measurement {

static std::vector<std::string> split(const std::string &s, char delim);
using klock = std::chrono::high_resolution_clock; // name clock exists in time.h

class Measurement {
 public:
  std::chrono::duration<double> time_{0};
  uint64_t accesses_ = 0;
  uint64_t bytes = 0;

  friend Measurement operator+(const Measurement &x, const Measurement &y) {
    return {x.time_ + y.time_,
            x.accesses_ + y.accesses_,
            x.bytes + y.bytes};
  }

  friend Measurement operator/(const Measurement &x, const uint64_t &d) {
    return {x.time_ / d, x.accesses_ / d, x.bytes / d};
  }

  friend std::ostream &operator<<(std::ostream &os, const Measurement &m) {
    os << m.time_.count() << "," << m.accesses_ << "," << m.bytes;
    return os;
  }
};

static const std::string kCsvHeaders =
    "test,"
    "bs,"
    "n,"
    "alloc,"
    "init,init_accesses,init_bytes,"
    "insert,insert_accesses,insert_bytes,"
    "search,search_accesses,search_bytes,"
    "delete,delete_accesses,delete_bytes,"
    "disk,"
    "max_mem_level";

class Run {
 public:
  std::string name_;
  uint8_t po2_;
  uint64_t bs_;
  Measurement alloc_;
  Measurement init_;
  Measurement insert_;
  Measurement search_;
  Measurement delete_;
  bool is_on_disk_ = false;
  uint8_t max_mem_level_ = 255;
  std::chrono::time_point<klock> start_time_ = klock::now();

  Run(std::string name, uint8_t po2, uint64_t bs, uint8_t mml = 255)
      : name_(std::move(name)), po2_(po2), bs_(bs), max_mem_level_(mml) {}
  Run(std::string name,
      uint8_t po_2,
      uint64_t bs,
      const Measurement &a,
      const Measurement &init,
      const Measurement &insert,
      const Measurement &s,
      const Measurement &d,
      bool is_on_disk,
      uint8_t mml)
      : name_(std::move(name)),
        po2_(po_2),
        bs_(bs),
        alloc_(a),
        init_(init),
        insert_(insert),
        search_(s),
        delete_(d),
        is_on_disk_(is_on_disk),
        max_mem_level_(mml) {}

  [[nodiscard]] std::chrono::duration<double> Elapsed() const {
    return klock::now() - start_time_;
  }

  friend Run operator+(const Run &x, const Run &y) {
    return {x.name_,
            x.po2_,
            x.bs_,
            x.alloc_ + y.alloc_,
            x.init_ + y.init_,
            x.insert_ + y.insert_,
            x.search_ + y.search_,
            x.delete_ + y.delete_,
            x.is_on_disk_ && y.is_on_disk_,
            x.max_mem_level_};
  }

  friend Run operator/(const Run &x, const uint64_t &d) {
    return {x.name_,
            x.po2_,
            x.bs_,
            x.alloc_ / d,
            x.init_ / d,
            x.insert_ / d,
            x.search_ / d,
            x.delete_ / d,
            x.is_on_disk_,
            x.max_mem_level_};
  }

  friend std::ostream &operator<<(std::ostream &os, const Run &r) {
    return os
        << r.name_ << ","
        << r.bs_ << ","
        << (int) (r.po2_) << ","
        << r.alloc_.time_.count() << ","
        << r.init_ << ","
        << r.insert_ << ","
        << r.search_ << ","
        << r.delete_ << ","
        << (r.is_on_disk_ ? 1 : 0) << ","
        << (int) r.max_mem_level_;
  }
};

class Config {
 public:
  std::vector<uint8_t> po2s_;
  std::vector<uint64_t> block_sizes_;
  std::string store_path_;
  uint8_t num_runs_ = 0;
  uint8_t max_mem_level_ = 0;
  bool is_valid_ = false;

  Config(int argc, char **argv) {
    if (argc < 5 || argc > 7) {
      LogHelp(argv[0]);
      return;
    }
    num_runs_ = std::stoi(argv[1]);
    uint8_t min_po2 = std::stoi(argv[2]);
    uint8_t max_po2 = std::stoi(argv[3]);
    std::string block_sizes = {argv[4]};
    if (argc >= 6)
      store_path_ = argv[5];
    if (argc >= 7)
      max_mem_level_ = std::stoi(argv[6]);

    if (min_po2 > max_po2) {
      LogHelp(argv[0]);
      return;
    }
    for (auto x = min_po2; x <= max_po2; ++x)
      po2s_.push_back(x);

    for (const auto &bs : split(block_sizes, ','))
      block_sizes_.push_back(std::stoi(bs));
    if (block_sizes.empty()) {
      LogHelp(argv[0]);
      return;
    }

    is_valid_ = true;
  }

 private:
  static void LogHelp(const std::string &app_name);
};

void Config::LogHelp(const std::string &app_name) {
  std::clog << "Usage: "
            << app_name << " "
            << "number_of_runs "
            << "min_size_power_of_2 "
            << "max_size_power_of_2 "
            << "block_size[,block_size...] "
            << "[file_store_path]"
            << std::endl;

}

static std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> result;
  std::stringstream ss(s);
  std::string item;
  while (getline(ss, item, delim))
    result.push_back(item);
  return result;
}

} // namespace dyno::measurement
#endif //DYNO_UTILS_MEASUREMENTS_H_
