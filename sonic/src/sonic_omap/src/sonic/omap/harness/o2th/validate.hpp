#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sonic/oram/harness/detail/random.hpp"
#include "sonic/omap/harness/o2th/config.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::omap::harness::o2th {

using sn::oram::harness::detail::fill_random_buffer;
using sn::oram::harness::detail::make_prng;
using sn::oram::harness::detail::seed_generator;

namespace detail {

template <typename Table> using key_type_t = typename Table::key_type;
template <typename Table> using request_type_t = typename Table::op_request;
template <typename Table> using maybe_request_t = typename Table::template maybe_dummy<request_type_t<Table>>;
template <typename Table> using block_data_t = typename Table::data_buffer;

} // namespace detail

template <typename Table> validate_result validate(Table& table, const validate_options& opts) {
  using key_type = detail::key_type_t<Table>;
  using maybe_request = detail::maybe_request_t<Table>;
  using block_data = detail::block_data_t<Table>;
  using bucket_index = typename Table::bucket_index;
  using data_query = typename Table::data_query;
  constexpr std::size_t block_bytes = Table::block_size;

  validate_result overall_result{};
  auto log = sn::util::log::create("omap:validate");

  const std::size_t block_count = table.block_count();
  sn::util::log::ensure(block_count > 0, "omap::validate: block_count must be positive");
  log.trcf("omap::validate: begin block_count=%zu block_bytes=%zu", block_count, block_bytes);

  const std::size_t iterations = std::max<std::size_t>(opts.iterations, static_cast<std::size_t>(1));
  const std::uint64_t max_key_value = static_cast<std::uint64_t>(std::numeric_limits<key_type>::max());
  const std::uint64_t required_keys = static_cast<std::uint64_t>(block_count) + 8;
  sn::util::log::ensure(
      required_keys < max_key_value, "omap::validate: key space insufficient for validation workload"
  );

  seed_generator seed_gen;

  struct expectation {
    block_data initial_dataset{};
    block_data expected_dataset{};
    block_data request_payload{};
    bool is_real = false;
    bool is_write = false;
    bool accessed = false;
    bool retrieved = false;
  };

  const auto verbosity = log.verbosity();
  const bool debug_logs = static_cast<int>(verbosity) >= static_cast<int>(sn::util::log::level::debug);
  const bool pedantic_logs = static_cast<int>(verbosity) >= static_cast<int>(sn::util::log::level::pedantic);

  for (std::size_t iter = 0; iter < iterations; ++iter) {
    auto prng = make_prng(seed_gen);

    if (iterations > 1) {
      log.inff("omap::validate: iteration %zu/%zu", iter + 1, iterations);
    }

    std::vector<block_data> dataset(block_count);
    for (auto& block : dataset) {
      fill_random_buffer(block, prng);
    }
    log.dbgf("omap::validate[%zu]: initialized dataset with random payloads", iter);

    std::vector<maybe_request> build_set(block_count);
    std::vector<expectation> expectations(block_count);
    std::unordered_map<key_type, std::size_t> key_to_index;
    key_to_index.reserve(block_count);

    using unsigned_key = std::make_unsigned_t<key_type>;
    unsigned_key next_key = static_cast<unsigned_key>(1);

    for (std::size_t idx = 0; idx < block_count; ++idx) {
      auto& request = build_set[idx];
      auto& exp = expectations[idx];
      exp.initial_dataset = dataset[idx];
      exp.expected_dataset = dataset[idx];
      exp.request_payload = dataset[idx];

      const bool force_real = idx == 0;
      const bool real_bit = force_real || (prng.random_u64(0, 3) != 0);
      exp.is_real = real_bit;
      request.is_dummy = !real_bit;

      if (!real_bit) {
        continue;
      }

      const key_type key = static_cast<key_type>(next_key++);
      request.value.key = key;
      request.value.is_write = (prng.random_u64(0, 1) == 1);
      exp.is_write = request.value.is_write;

      if (exp.is_write) {
        fill_random_buffer(exp.request_payload, prng);
        request.value.data = exp.request_payload;
        exp.expected_dataset = exp.request_payload;
      } else {
        request.value.data = exp.request_payload;
      }

      const auto [pos, inserted] = key_to_index.emplace(key, idx);
      sn::util::log::ensure(inserted, "omap::validate: duplicate key in build set");
      if (pedantic_logs) {
        log.pedf(
            "  build[%03zu @%zu]: key=%llu op=%s", idx, iter, static_cast<unsigned long long>(key),
            request.value.is_write ? "write" : "read"
        );
      }
    }

    log.dbgf(
        "omap::validate[%zu]: build set prepared real=%zu dummy=%zu", iter, key_to_index.size(),
        block_count - key_to_index.size()
    );

    table.build(sn::util::span<maybe_request>(build_set.data(), build_set.size()));
    log.dbgf("omap::validate[%zu]: table.build completed", iter);

    const std::size_t real_count = key_to_index.size();
    sn::util::log::ensure(real_count > 0, "omap::validate: build set must include real entries");
    log.inff("omap::validate[%zu]: verifying %zu real entries via access_one", iter, real_count);

    for (std::size_t idx = 0; idx < block_count; ++idx) {
      auto& exp = expectations[idx];
      if (!exp.is_real) {
        continue;
      }

      auto& slot = dataset[idx];
      const key_type key = build_set[idx].value.key;
      const bool found = table.access_one(key, slot.data());
      sn::util::log::ensure(found, "omap::validate: expected key absent during access_one");

      const bool data_ok = std::equal(slot.begin(), slot.end(), exp.expected_dataset.begin());
      sn::util::log::ensure(data_ok, "omap::validate: dataset entry diverged after access_one");

      exp.accessed = true;
      if (debug_logs) {
        log.dbgf(
            "  access_one[%03zu @%zu]: key=%llu op=%s", idx, iter, static_cast<unsigned long long>(key),
            exp.is_write ? "write" : "read"
        );
      }
    }

    const unsigned_key dummy_key_base = next_key;
    const std::size_t remaining_budget = block_count - real_count;
    std::size_t dummy_attempts = std::min<std::size_t>(remaining_budget, static_cast<std::size_t>(8));
    log.dbgf("omap::validate[%zu]: issuing %zu dummy probes", iter, dummy_attempts);
    for (std::size_t trial = 0; trial < dummy_attempts; ++trial) {
      block_data scratch{};
      const key_type probe = static_cast<key_type>(dummy_key_base + static_cast<unsigned_key>(trial));
      const bool found = table.access_one(probe, scratch.data());
      sn::util::log::ensure(!found, "omap::validate: unexpected hit for dummy key");
    }

    for (std::size_t idx = 0; idx < block_count; ++idx) {
      const auto& exp = expectations[idx];
      if (!exp.is_real) {
        continue;
      }
      sn::util::log::ensure(exp.accessed, "omap::validate: real entry was never accessed");
    }

    const std::size_t retrieval_count = block_count * 2;
    std::vector<maybe_request> retrieved(retrieval_count);
    std::vector<std::uint8_t> marks(retrieval_count);
    std::vector<std::size_t> prefix(retrieval_count + 1);

    table.retrieve(
        sn::util::span<maybe_request>(retrieved.data(), retrieved.size()),
        sn::util::span<std::uint8_t>(marks.data(), marks.size()),
        sn::util::span<std::size_t>(prefix.data(), prefix.size())
    );
    log.dbgf("omap::validate[%zu]: retrieve completed (buffer=%zu)", iter, retrieved.size());

    std::unordered_set<key_type> seen_keys;
    seen_keys.reserve(real_count);

    for (std::size_t idx = 0; idx < retrieved.size(); ++idx) {
      const auto& entry = retrieved[idx];
      if (entry.is_dummy) {
        if (pedantic_logs) {
          log.pedf("  retrieve[%03zu @%zu]: dummy", idx, iter);
        }
        continue;
      }

      const key_type key = entry.value.key;
      auto it = key_to_index.find(key);
      sn::util::log::ensure(it != key_to_index.end(), "omap::validate: retrieve returned unknown key");

      const bool inserted = seen_keys.insert(key).second;
      sn::util::log::ensure(inserted, "omap::validate: retrieve produced duplicate key");

      auto& exp = expectations[it->second];
      sn::util::log::ensure(exp.is_write == entry.value.is_write, "omap::validate: retrieve flagged wrong op type");

      const auto& expected_block = exp.expected_dataset;
      const bool data_match = std::equal(entry.value.data.begin(), entry.value.data.end(), expected_block.begin());
      sn::util::log::ensure(data_match, "omap::validate: retrieve produced incorrect payload");
      if (pedantic_logs) {
        log.pedf(
            "  retrieve[%03zu @%zu]: key=%llu op=%s", idx, iter, static_cast<unsigned long long>(key),
            entry.value.is_write ? "write" : "read"
        );
      }

      exp.retrieved = true;
    }

    for (std::size_t idx = 0; idx < block_count; ++idx) {
      const auto& exp = expectations[idx];
      if (!exp.is_real) {
        continue;
      }
      sn::util::log::ensure(exp.retrieved, "omap::validate: real entry missing from retrieve output");
    }

    validate_result iter_result{};
    iter_result.access_one_accesses = real_count;
    iter_result.dummy_probe_accesses = dummy_attempts;
    iter_result.batch_accesses = 0;

    if (block_count > 0) {
      log.inff("omap::validate[%zu]: verifying %zu real entries via access_batch", iter, real_count);

      for (std::size_t idx = 0; idx < block_count; ++idx) {
        dataset[idx] = expectations[idx].initial_dataset;
        expectations[idx].accessed = false;
        expectations[idx].retrieved = false;
      }

      table.build(sn::util::span<maybe_request>(build_set.data(), build_set.size()));
      log.dbgf("omap::validate[%zu]: table rebuilt for access_batch verification", iter);

      std::vector<data_query> batch_queries(block_count);
      std::vector<bucket_index> batch_pos_l1(block_count);
      std::vector<bucket_index> batch_pos_l2(block_count);

      const std::uint64_t batch_dummy_base =
          static_cast<std::uint64_t>(max_key_value) - static_cast<std::uint64_t>(block_count) - 1;
      sn::util::log::ensure(
          batch_dummy_base > static_cast<std::uint64_t>(real_count),
          "omap::validate: insufficient key space for batch dummy queries"
      );

      std::uint64_t dummy_cursor = batch_dummy_base;
      for (std::size_t idx = 0; idx < block_count; ++idx) {
        auto& query = batch_queries[idx];
        const auto& exp = expectations[idx];
        const auto payload = sn::util::span<const std::uint8_t>(dataset[idx].data(), dataset[idx].size());
        if (exp.is_real) {
          const key_type key = build_set[idx].value.key;
          query.assign(key, payload, exp.is_write);
        } else {
          const key_type dummy_key = static_cast<key_type>(dummy_cursor--);
          query.assign(dummy_key, payload, false);
        }
      }

      table.access_batch(
          sn::util::span<data_query>(batch_queries.data(), batch_queries.size()),
          sn::util::span<bucket_index>(batch_pos_l1.data(), batch_pos_l1.size()),
          sn::util::span<bucket_index>(batch_pos_l2.data(), batch_pos_l2.size())
      );

      for (std::size_t idx = 0; idx < block_count; ++idx) {
        auto& query = batch_queries[idx];
        auto& exp = expectations[idx];
        if (!exp.is_real) {
          sn::util::log::ensure(
              !query.result_ok(), "omap::validate: dummy dataset entry unexpectedly matched during access_batch"
          );

          const bool unchanged =
              std::equal(query.data.begin(), query.data.end(), exp.initial_dataset.begin(), exp.initial_dataset.end());
          sn::util::log::ensure(unchanged, "omap::validate: dummy dataset entry modified during access_batch");
          continue;
        }

        sn::util::log::ensure(query.result_ok(), "omap::validate: batch query missed expected key");
        const bool data_ok =
            std::equal(query.data.begin(), query.data.end(), exp.expected_dataset.begin(), exp.expected_dataset.end());
        sn::util::log::ensure(data_ok, "omap::validate: batch query produced incorrect payload");
        sn::obliv::copy(query.data.begin(), query.data.end(), dataset[idx].begin());
        exp.accessed = true;
      }

      for (std::size_t idx = 0; idx < block_count; ++idx) {
        const auto& exp = expectations[idx];
        if (!exp.is_real) {
          continue;
        }
        sn::util::log::ensure(exp.accessed, "omap::validate: real entry missing from access_batch results");
      }

      sn::obliv::fill(marks.begin(), marks.end(), 0);
      sn::obliv::fill(prefix.begin(), prefix.end(), 0);
      table.retrieve(
          sn::util::span<maybe_request>(retrieved.data(), retrieved.size()),
          sn::util::span<std::uint8_t>(marks.data(), marks.size()),
          sn::util::span<std::size_t>(prefix.data(), prefix.size())
      );
      log.dbgf("omap::validate[%zu]: retrieve completed after batch validation", iter);

      seen_keys.clear();
      for (std::size_t idx = 0; idx < retrieved.size(); ++idx) {
        const auto& entry = retrieved[idx];
        if (entry.is_dummy) {
          continue;
        }

        const key_type key = entry.value.key;
        auto it = key_to_index.find(key);
        sn::util::log::ensure(it != key_to_index.end(), "omap::validate: retrieve (batch) returned unknown key");

        const bool inserted = seen_keys.insert(key).second;
        sn::util::log::ensure(inserted, "omap::validate: retrieve (batch) produced duplicate key");

        auto& exp = expectations[it->second];
        sn::util::log::ensure(
            exp.is_write == entry.value.is_write, "omap::validate: retrieve (batch) op type mismatch"
        );

        const bool data_match =
            std::equal(entry.value.data.begin(), entry.value.data.end(), exp.expected_dataset.begin());
        sn::util::log::ensure(data_match, "omap::validate: retrieve (batch) produced incorrect payload");

        exp.retrieved = true;
      }

      for (std::size_t idx = 0; idx < block_count; ++idx) {
        const auto& exp = expectations[idx];
        if (!exp.is_real) {
          continue;
        }
        sn::util::log::ensure(exp.retrieved, "omap::validate: real entry missing from batch retrieve output");
      }

      iter_result.batch_accesses = block_count;
    }

    overall_result.access_one_accesses += iter_result.access_one_accesses;
    overall_result.dummy_probe_accesses += iter_result.dummy_probe_accesses;
    overall_result.batch_accesses += iter_result.batch_accesses;
  }

  log.inff(
      "omap::validate: completed %zu iteration(s) (real=%zu dummy=%zu batch=%zu)", iterations,
      overall_result.access_one_accesses, overall_result.dummy_probe_accesses, overall_result.batch_accesses
  );
  return overall_result;
}

} // namespace sn::omap::harness::o2th
