#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "sonic/oram/core/access.hpp"
#include "sonic/oram/core/access_ops.hpp"
#include "sonic/oram/harness/detail/access_window.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::harness::detail {

template <typename Client, typename = void> struct has_flush_epoch : std::false_type {};

template <typename Client>
struct has_flush_epoch<Client, std::void_t<decltype(std::declval<Client&>().flush_epoch())>> : std::true_type {};

template <typename Client, typename = void> struct has_drop_epoch : std::false_type {};

template <typename Client>
struct has_drop_epoch<Client, std::void_t<decltype(std::declval<Client&>().drop_epoch())>> : std::true_type {};

template <typename Client, typename = void> struct has_access_concurrency : std::false_type {};

template <typename Client>
struct has_access_concurrency<Client, std::void_t<decltype(std::declval<const Client&>().options().access_concurrency)>>
    : std::true_type {};

template <typename Client> constexpr bool has_flush_epoch_v = has_flush_epoch<Client>::value;
template <typename Client> constexpr bool has_drop_epoch_v = has_drop_epoch<Client>::value;
template <typename Client> constexpr bool has_access_concurrency_v = has_access_concurrency<Client>::value;

template <typename Client> using access_scratch_type = typename std::remove_cvref_t<Client>::access_scratch;

template <typename Client> [[nodiscard]] inline std::size_t max_access_concurrency(const Client& client) noexcept {
  if constexpr (has_access_concurrency_v<Client>) {
    const auto configured = static_cast<std::size_t>(client.options().access_concurrency);
    return configured > 0 ? configured : 1;
  } else {
    return 1;
  }
}

template <typename Client> inline void flush_epoch_if_supported(Client& client) noexcept {
  if constexpr (has_flush_epoch_v<Client>) {
    client.flush_epoch();
  } else {
    (void) client;
  }
}

template <typename Client> inline void drop_epoch_if_supported(Client& client) noexcept {
  if constexpr (has_drop_epoch_v<Client>) {
    client.drop_epoch();
  } else {
    (void) client;
  }
}

template <typename Client> inline void apply_window_close(Client& client, window_close close) noexcept {
  switch (close) {
  case window_close::none:
    return;
  case window_close::flush:
    flush_epoch_if_supported(client);
    return;
  case window_close::drop:
    drop_epoch_if_supported(client);
    return;
  }
}

[[nodiscard]] inline sn::oram::access_request make_access_request(
    std::int64_t address, std::uint64_t cur_leaf, std::uint64_t new_leaf, bool is_write,
    sn::util::span<std::uint8_t> in_buf, sn::util::span<std::uint8_t> out_buf
) noexcept {
  sn::oram::access_request req{};
  req.address = address;
  req.cur_leaf = static_cast<std::int64_t>(cur_leaf);
  req.new_leaf = static_cast<std::int64_t>(new_leaf);
  req.is_write = is_write;
  req.in = in_buf;
  req.out = out_buf;
  return req;
}

template <typename Client> inline void configure_access_scratch(Client& client, access_scratch_type<Client>& scratch) {
  client.configure_access_scratch(scratch);
}

template <typename Client, typename InBuffer, typename OutBuffer, typename Mutator = sn::oram::read_write_mutator>
inline void issue_access(
    Client& client, access_scratch_type<Client>& scratch, std::int64_t address, std::uint64_t cur_leaf,
    std::uint64_t new_leaf, bool is_write, InBuffer& in_buf, OutBuffer& out_buf, Mutator&& mutator = Mutator{}
) {
  auto req = make_access_request(
      address, cur_leaf, new_leaf, is_write, sn::util::span<std::uint8_t>(in_buf.data(), in_buf.size()),
      sn::util::span<std::uint8_t>(out_buf.data(), out_buf.size())
  );
  client.access(req, scratch, std::forward<Mutator>(mutator));
}

} // namespace sn::oram::harness::detail
