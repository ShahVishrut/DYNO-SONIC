#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace sn::scooby::omap {

enum class epoch_event {
  client_batch_in,
  client_batch_out,
  bin_dispatch_out,
  bin_dispatch_in,
  bin_response_out,
  bin_response_in,
  client_response_out,
  client_response_in,
};

struct epoch_counts {
  std::uint32_t client_batches_in{0};
  std::uint32_t client_batches_out{0};
  std::uint32_t bin_dispatches_out{0};
  std::uint32_t bin_dispatches_in{0};
  std::uint32_t bin_responses_out{0};
  std::uint32_t bin_responses_in{0};
  std::uint32_t client_responses_out{0};
  std::uint32_t client_responses_in{0};
};

inline void add_count(epoch_counts& counts, epoch_event event, std::uint32_t amount) {
  switch (event) {
  case epoch_event::client_batch_in:
    counts.client_batches_in += amount;
    break;
  case epoch_event::client_batch_out:
    counts.client_batches_out += amount;
    break;
  case epoch_event::bin_dispatch_out:
    counts.bin_dispatches_out += amount;
    break;
  case epoch_event::bin_dispatch_in:
    counts.bin_dispatches_in += amount;
    break;
  case epoch_event::bin_response_out:
    counts.bin_responses_out += amount;
    break;
  case epoch_event::bin_response_in:
    counts.bin_responses_in += amount;
    break;
  case epoch_event::client_response_out:
    counts.client_responses_out += amount;
    break;
  case epoch_event::client_response_in:
    counts.client_responses_in += amount;
    break;
  }
}

inline bool counters_complete(const epoch_counts& have, const epoch_counts& want) {
  return have.client_batches_in >= want.client_batches_in && have.client_batches_out >= want.client_batches_out &&
         have.bin_dispatches_out >= want.bin_dispatches_out && have.bin_dispatches_in >= want.bin_dispatches_in &&
         have.bin_responses_out >= want.bin_responses_out && have.bin_responses_in >= want.bin_responses_in &&
         have.client_responses_out >= want.client_responses_out && have.client_responses_in >= want.client_responses_in;
}

struct epoch_entry {
  std::uint64_t epoch_id{0};
  epoch_counts expected{};
  epoch_counts actual{};
};

class epoch_table {
public:
  explicit epoch_table(epoch_counts defaults) : defaults_(defaults) {}

  epoch_entry& ensure(std::uint64_t epoch) {
    auto [it, inserted] = table_.try_emplace(epoch);
    if (inserted) {
      it->second.epoch_id = epoch;
      it->second.expected = defaults_;
    }
    return it->second;
  }

  void set_expected(std::uint64_t epoch, epoch_counts counts) {
    auto& entry = ensure(epoch);
    entry.expected = counts;
  }

  void mark(std::uint64_t epoch, epoch_event event, std::uint32_t amount = 1) {
    auto& entry = ensure(epoch);
    add_count(entry.actual, event, amount);
  }

  bool complete(std::uint64_t epoch) {
    auto it = table_.find(epoch);
    if (it == table_.end()) {
      return false;
    }
    return counters_complete(it->second.actual, it->second.expected);
  }

  std::string describe(std::uint64_t epoch) const {
    auto it = table_.find(epoch);
    if (it == table_.end()) {
      return {};
    }
    std::ostringstream oss;
    const auto& have = it->second.actual;
    const auto& want = it->second.expected;
    oss << "epoch=" << epoch << " client_batches_in=" << have.client_batches_in << '/' << want.client_batches_in
        << " client_batches_out=" << have.client_batches_out << '/' << want.client_batches_out
        << " bin_out=" << have.bin_dispatches_out << '/' << want.bin_dispatches_out
        << " bin_in=" << have.bin_dispatches_in << '/' << want.bin_dispatches_in
        << " bin_resp_out=" << have.bin_responses_out << '/' << want.bin_responses_out
        << " bin_resp_in=" << have.bin_responses_in << '/' << want.bin_responses_in
        << " client_resp_out=" << have.client_responses_out << '/' << want.client_responses_out
        << " client_resp_in=" << have.client_responses_in << '/' << want.client_responses_in;
    return oss.str();
  }

  void erase(std::uint64_t epoch) { table_.erase(epoch); }

private:
  epoch_counts defaults_{};
  std::unordered_map<std::uint64_t, epoch_entry> table_{};
};

}
