// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Distributed
{

/// What a dial hint is decided from: one registry entry's three address facts.
struct DialHintInputs
{
    std::string_view advertised;                     ///< What the worker registered; what the token signs.
    std::string_view observedHost;                   ///< The kernel's peer host on its last heartbeat.
    std::span<std::string const> interfaceAddresses; ///< What the worker says it answers on.
};

/// Why a lease carries no hint. PRIVATE: never transmitted, never persisted.
enum class HintVeto : std::uint8_t
{
    NoObservedHost,
    LoopbackObserved,
    LinkLocalObserved,
    AdvertiseUnparsable,
    AdvertiseIsLiteral,
    NotAReportedInterface,
    Last,
};

/// One veto: its name for a person, and the predicate that raises it.
struct HintVetoRow
{
    HintVeto veto;
    std::string_view name;
    /// @param in The entry's facts. @param advertisedHost The advertise's host, empty when it did not parse.
    bool (*applies)(DialHintInputs const& in, std::string_view advertisedHost);
};

/// The vetoes, asked in enumerator order; the first that applies decides.
///
/// Every host comparison here goes through `Core/HostPort.hpp`'s own folds rather than a
/// second one. For the ADVERTISE, `ParseDialEndpoint` strips an IPv6 literal's brackets before
/// anything here sees it, so a bracketed and an unbracketed spelling already compare equal by
/// the time `IsIpLiteralHost` runs on it. `observedHost` and `interfaceAddresses`
/// need no such unbracketing: `observedHost` is `CallerContext::peerId`, which the transport
/// fills from `core::net::ISocket::PeerAddress()`, and `interfaceAddresses` is produced by
/// `Platform/LocalAddresses.cpp` deliberately the same way -- `inet_ntop` on the address bytes,
/// no `%scope` suffix -- so the two are already bare, unbracketed text in the same spelling
/// before anything here compares them. What they still disagree on is the `::ffff:`-mapped IPv4
/// form, which `UnmappedHost`/`SameHost` fold both ways round. Nothing here folds case, because
/// nothing here ever compares two names: every predicate reading `observedHost` compares it
/// against another address, never against text a person or a DNS record spelled.
inline constexpr EnumTable<HintVeto, HintVetoRow> HintVetoes { {
    { .veto = HintVeto::NoObservedHost,
      .name = "no observed host",
      .applies = [](DialHintInputs const& in, std::string_view) { return UnmappedHost(in.observedHost).empty(); } },
    { .veto = HintVeto::LoopbackObserved,
      .name = "observed on loopback",
      .applies = [](DialHintInputs const& in, std::string_view) { return IsLoopbackHost(in.observedHost); } },
    { .veto = HintVeto::LinkLocalObserved,
      .name = "observed on a link-local address",
      .applies = [](DialHintInputs const& in, std::string_view) { return IsLinkLocalHost(in.observedHost); } },
    { .veto = HintVeto::AdvertiseUnparsable,
      .name = "advertise names no host and port",
      .applies = [](DialHintInputs const&, std::string_view host) { return host.empty(); } },
    { .veto = HintVeto::AdvertiseIsLiteral,
      .name = "advertise is an IP literal",
      .applies = [](DialHintInputs const&, std::string_view host) { return IsIpLiteralHost(host); } },
    { .veto = HintVeto::NotAReportedInterface,
      .name = "observed host is not an address the worker reports",
      .applies =
          [](DialHintInputs const& in, std::string_view) {
              return std::ranges::none_of(in.interfaceAddresses,
                                          [&in](std::string const& address) { return SameHost(in.observedHost, address); });
          } },
} };

static_assert(RowsInEnumeratorOrder(HintVetoes, &HintVetoRow::veto), "HintVetoes must hold one row per HintVeto, in order");

/// A hint, or which veto refused one.
struct DialHintDecision
{
    std::string endpoint;         ///< `host:port` to dial first; empty when vetoed.
    std::optional<HintVeto> veto; ///< Why there is none, or nothing when there is one.
};

/// Decide one entry's dial hint. PURE: no clock, no resolver, no socket.
/// @param in The entry's facts.
/// @return The hint, unmapped and bracketed as `FormatHostPort` spells it, or the veto.
[[nodiscard]] inline DialHintDecision DecideDialHint(DialHintInputs const& in)
{
    auto const parsed = ParseDialEndpoint(in.advertised);
    auto const host = parsed.has_value() ? parsed->first : std::string {};
    auto const port = parsed.has_value() ? parsed->second : std::uint16_t { 0 };
    for (auto const& row: HintVetoes)
        if (row.applies(in, host))
            return DialHintDecision { .endpoint = {}, .veto = row.veto };
    return DialHintDecision { .endpoint = FormatHostPort(UnmappedHost(in.observedHost), port), .veto = std::nullopt };
}

/// @param in The entry's facts. @return The hint, or empty.
[[nodiscard]] inline std::string DialHintFor(DialHintInputs const& in)
{
    return DecideDialHint(in).endpoint;
}

} // namespace FastCache::Distributed
