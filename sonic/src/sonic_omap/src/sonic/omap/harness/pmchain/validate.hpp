#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

#include "sonic/obliv/ops/platform_ops.hpp"

#include "sonic/crypto/prng.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::omap::harness::pmchain {

namespace detail {

template <typename Chain> struct batch_buffers {
  using operation_type = typename Chain::operation;

  explicit batch_buffers(std::size_t batch_size) : ops(batch_size) {}

  std::vector<operation_type> ops;
};

template <typename Chain> struct request_record {
  using key_type = typename Chain::key_type;
  bool is_dummy = true;
  bool is_write = false;
  key_type key{};
  std::vector<std::uint8_t> payload;
};

template <typename Chain>
void prepare_batch(
    Chain& chain, batch_buffers<Chain>& buffers, sn::crypto::prng& prng, std::size_t batch_index,
    std::size_t writes_per_batch, double dummy_ratio = 0.0
) {
  const std::size_t batch_size = buffers.ops.size();
  const std::size_t block_count = chain.block_count();
  const std::size_t key_base = (batch_index * batch_size) % block_count;

  const bool always_dummy = dummy_ratio >= 1.0;
  const bool never_dummy = dummy_ratio <= 0.0;
  std::uint64_t threshold = 0;
  if (!always_dummy && !never_dummy) {
    threshold =
        static_cast<std::uint64_t>(dummy_ratio * static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
  }

  std::size_t real_count = 0;
  if (always_dummy) {
    real_count = 0;
  } else if (never_dummy) {
    real_count = batch_size;
  } else {
    for (std::size_t slot = 0; slot < batch_size; ++slot) {
      const bool sampled_dummy = prng.random_u64() < threshold;
      real_count += sampled_dummy ? 0 : 1;
    }
  }

  const std::size_t writes_target = std::min(writes_per_batch, real_count);
  std::size_t writes_assigned = 0;

  for (std::size_t slot = 0; slot < batch_size; ++slot) {
    auto& op = buffers.ops[slot];
    const std::size_t key_index = (key_base + slot) % block_count;
    // pmchain input: real must precede dummy slots
    const bool is_dummy = slot >= real_count;

    op.key = static_cast<typename Chain::key_type>(key_index);
    op.is_dummy = is_dummy;
    op.extra_data = static_cast<std::uint32_t>(slot);

    if (!op.is_dummy && writes_assigned < writes_target) {
      op.is_write = true;
      ++writes_assigned;
    } else {
      op.is_write = false;
    }

    auto data_span = chain.request_buffer(slot);
    if (op.is_write) {
      prng.random_bytes(data_span.data(), data_span.size());
    } else {
      sn::obliv::fill(data_span.begin(), data_span.end(), std::uint8_t{0});
    }
  }
}

template <typename Chain>
void verify_batch(
    Chain& chain, sn::util::span<const request_record<Chain>> records,
    std::vector<std::vector<std::uint8_t>>& shadow_blocks
) {
  sn::util::log::ensure(shadow_blocks.size() == chain.block_count(), "pmchain::validate: shadow block count mismatch");

  const auto retrieved = chain.retrieved_requests();
  sn::util::log::ensure(retrieved.size() == records.size(), "pmchain::validate: retrieved count mismatch");

  const std::size_t block_count = shadow_blocks.size();
  std::vector<std::vector<std::size_t>> pending(block_count);
  pending.reserve(block_count);
  std::size_t real_request_count = 0;
  for (std::size_t idx = 0; idx < records.size(); ++idx) {
    const auto& record = records[idx];
    if (record.is_dummy) {
      continue;
    }
    const std::size_t key = static_cast<std::size_t>(record.key);
    sn::util::log::ensure(key < block_count, "pmchain::validate: request key out of range");
    pending[key].push_back(idx);
    ++real_request_count;
  }

  bool saw_dummy_output = false;
  std::size_t matched_real = 0;
  for (std::size_t slot = 0; slot < retrieved.size(); ++slot) {
    const auto& entry = retrieved[slot];
    if (entry.is_dummy) {
      saw_dummy_output = true;
      continue;
    }
    sn::util::log::ensure(!saw_dummy_output, "pmchain::validate: real response after dummy output");

    const std::size_t key = static_cast<std::size_t>(entry.value.key);
    sn::util::log::ensure(key < block_count, "pmchain::validate: response key out of range");
    auto& key_queue = pending[key];
    sn::util::log::ensure(!key_queue.empty(), "pmchain::validate: unexpected response key");

    const std::size_t request_index = key_queue.back();
    key_queue.pop_back();
    const auto& request = records[request_index];
    sn::util::log::ensure(!request.is_dummy, "pmchain::validate: matched dummy request");
    sn::util::log::ensure(request.is_write == entry.value.is_write, "pmchain::validate: response write flag mismatch");

    auto data_span = chain.request_buffer(slot);
    sn::util::log::ensure(data_span.size() == request.payload.size(), "pmchain::validate: buffer size mismatch");

    if (request.is_write) {
      shadow_blocks[key] = request.payload;
    } else {
      const auto& expected = shadow_blocks[key];
      const bool read_match = std::equal(data_span.begin(), data_span.end(), expected.begin(), expected.end());
      if (!read_match) {
        sn::util::log::failf(
            "pmchain::validate: read payload mismatch (slot=%zu key=%zu request_index=%zu)", slot, key, request_index
        );
      }
    }
    ++matched_real;
  }

  sn::util::log::ensure(matched_real == real_request_count, "pmchain::validate: missing responses");
  for (const auto& queue : pending) {
    sn::util::log::ensure(queue.empty(), "pmchain::validate: unfulfilled request");
  }
}

template <typename Chain>
void capture_request_records(
    Chain& chain, const batch_buffers<Chain>& buffers, sn::util::span<request_record<Chain>> records
) {
  sn::util::log::ensure(records.size() == buffers.ops.size(), "pmchain::validate: record span mismatch");
  for (std::size_t slot = 0; slot < buffers.ops.size(); ++slot) {
    const auto& op = buffers.ops[slot];
    auto& record = records[slot];
    record.is_dummy = op.is_dummy;
    record.is_write = op.is_write;
    record.key = op.key;
    auto data_span = chain.request_buffer(slot);
    sn::util::log::ensure(record.payload.size() == data_span.size(), "pmchain::validate: record payload size mismatch");
    std::copy(data_span.begin(), data_span.end(), record.payload.begin());
  }
}

} // namespace detail

struct validate_options {
  std::size_t batches = 1;
  double write_ratio = 0.5;
  double dummy_ratio = 0.0;
  sn::util::log::logger logger = sn::util::log::create("pmchain:validate");
};

template <typename Chain> void validate(Chain& chain, const validate_options& opts = {}) {
  auto log = opts.logger;
  const std::size_t batch_size = chain.batch_size();
  const std::size_t block_bytes = chain.block_size();
  const std::size_t block_count = chain.block_count();

  sn::util::log::ensure(batch_size > 0, "pmchain::validate: batch size must be positive");
  sn::util::log::ensure(block_bytes > 0, "pmchain::validate: block size must be positive");
  sn::util::log::ensure(block_count > 0, "pmchain::validate: block count must be positive");

  sn::crypto::prng prng;

  std::vector<std::vector<std::uint8_t>> expected_payloads(
      block_count, std::vector<std::uint8_t>(block_bytes, std::uint8_t{0})
  );

  const double clamped_ratio = std::clamp(opts.write_ratio, 0.0, 1.0);
  const std::size_t writes_per_batch =
      static_cast<std::size_t>(std::llround(clamped_ratio * static_cast<double>(batch_size)));

  detail::batch_buffers<Chain> buffers(batch_size);
  std::vector<detail::request_record<Chain>> records(batch_size);
  for (auto& record : records) {
    record.payload.resize(block_bytes);
  }

  log.inff(
      "pmchain::validate: batches=%zu batch_size=%zu block_bytes=%zu writes_per_batch=%zu dummy_ratio=%.3f",
      opts.batches, batch_size, block_bytes, writes_per_batch, opts.dummy_ratio
  );

  for (std::size_t batch = 0; batch < opts.batches; ++batch) {
    detail::prepare_batch(chain, buffers, prng, batch, writes_per_batch, opts.dummy_ratio);
    detail::capture_request_records(
        chain, buffers, sn::util::span<detail::request_record<Chain>>(records.data(), records.size())
    );

    chain.populate_requests(sn::util::span<const typename Chain::operation>(buffers.ops.data(), buffers.ops.size()));
    chain.execute_o2th_chains();
    chain.sort_o2th_chains();
    chain.execute_oram_queries();
    chain.flush_pending();

    detail::verify_batch(
        chain, sn::util::span<const detail::request_record<Chain>>(records.data(), records.size()), expected_payloads
    );
  }

  log.inff("pmchain::validate: completed %zu batch(es)", opts.batches);
}

} // namespace sn::omap::harness::pmchain
