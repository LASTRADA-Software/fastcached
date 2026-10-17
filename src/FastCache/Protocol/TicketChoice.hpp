// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// @file TicketChoice.hpp
/// Which credential one exchange presents, where this machine's machine tickets come from, and why
/// none was minted.
///
/// Header-only, over headers that are themselves std-only: the launcher includes it without linking
/// `FastCache`, and `fastcache-cli` and the node's operator verbs ask the same three questions, so
/// one answer serves all of them.

/// Which credential one exchange presents.
///
/// **PRIVATE: persisted and transmitted nowhere**; what travels is the credential itself.
enum class CredentialChoice : std::uint8_t
{
    None,     ///< Nothing: this machine is admitted as itself, or the audience names nobody.
    Password, ///< The one password configured, to the one endpoint it was configured for.
    Ticket,   ///< A machine ticket minted for this audience.
    Last,     ///< Enumerator count.
};

/// Which credential an exchange going to @p audience presents.
///
/// **The password goes to the ONE endpoint it was configured for** -- the cache at
/// `FASTCACHE_ADDR`, a `fastcached --requirepass` -- and nowhere else: sent to every node it would
/// stand in for a ticket, and a node admits no machine by a password. An audience that names no one
/// machine presents nothing, asked through the MINTER's own rule (`NamesNoOneMachine`): loopback
/// and `localhost` admit this machine as itself, a wildcard is nobody, and asking the node for a
/// ticket it refuses would move its refusal counter once per exchange. An audience that is not an
/// endpoint presents nothing either, because there is nobody to mint for. Anything else gets a
/// ticket of its own.
/// @param audience The endpoint this exchange dials, as `host:port`.
/// @param passwordFor The endpoint the password belongs to; empty when no password is configured.
/// @return The choice.
[[nodiscard]] inline CredentialChoice ChooseCredential(std::string_view audience, std::string_view passwordFor)
{
    if (!passwordFor.empty() && audience == passwordFor)
        return CredentialChoice::Password;
    auto const endpoint = ParseDialEndpoint(audience);
    if (!endpoint.has_value() || NamesNoOneMachine(endpoint->first))
        return CredentialChoice::None;
    return CredentialChoice::Ticket;
}

/// Where this machine's own node answers `MINT-TICKET`: a loopback LITERAL, always, at the port the
/// cache address names -- never a name, which a resolver could send anywhere.
///
/// **Loopback whatever else is configured.** The node mints only for a caller on its own machine
/// (`Op::MintTicket` is judged per connection), so the one address that can answer is this
/// machine's -- a remote `FASTCACHE_ADDR` or a scheduler elsewhere names somebody who would refuse.
/// So the source is `127.0.0.1` at the cache address's PORT, because the node on this machine is
/// configured like the one the cache address names far more often than it listens on a default.
/// The one exception is a cache address that is ITSELF a loopback literal -- `[::1]:6674`,
/// `127.0.0.2:6674` -- which is kept, rebuilt from its parsed parts: it is already a literal on this
/// machine, and the node answering it may listen on that family or address alone. A NAME never
/// qualifies, however it begins (`IsLoopbackHost` parses literals only). There is no default: an
/// address that is not an endpoint names no port, so there is no source, and the caller says so
/// rather than guessing one.
/// @param cacheAddr `FASTCACHE_ADDR`, as the launcher resolved it.
/// @return `host:port` to mint from, or nullopt when @p cacheAddr names no endpoint.
[[nodiscard]] inline std::optional<std::string> TicketSourceFor(std::string_view cacheAddr)
{
    constexpr std::string_view LoopbackLiteral = "127.0.0.1";
    auto const endpoint = ParseDialEndpoint(cacheAddr);
    if (!endpoint.has_value())
        return std::nullopt;
    auto const& [host, port] = *endpoint;
    return FormatHostPort(IsLoopbackHost(host) ? std::string_view { host } : LoopbackLiteral, port);
}

/// Ceiling on CONNECTING to this machine's node for a mint (#1664), whoever mints.
///
/// A mint source is a loopback literal by construction (`TicketSourceFor`), so it is bounded by
/// what loopback costs and not by a LAN exchange's connect budget. That matters on Windows, where a
/// connect to a CLOSED loopback port is not refused at once: the stack retries the SYN and answers
/// `WSAECONNREFUSED` after ~2 s, so a machine with no node used to wait out the whole exchange
/// budget (a second) on every mint.
///
/// **Every mint path dials under it, which is why it lives here and not beside the launcher's**
/// (#1669): the launcher (`MintBudget`), the node's operator verbs (`OperatorCredentials`, whose
/// exchange dials with the budget's connect) and `fastcache-cli` (`MintTicket`, over its own
/// `--connect-timeout`), each capped through `ConnectAtMost`. The operator verbs used to dial under
/// a fixed 2 s of their own, which on Windows is the whole refusal: a verb on a machine with no
/// node waited it out before going anywhere.
///
/// **The figures it was chosen from, and their conditions** -- 32-thread hosts, each sampled idle and
/// under 64 CPU-bound processes, 3,000 connects per row (closed port on Windows: 5), timed in Python
/// around the connect alone, the worse of idle and loaded shown:
///
/// | host | instrument | live listener, max (p99.9) | closed port |
/// |---|---|---|---|
/// | Windows 11 26200 | `asyncio` Proactor (IOCP `ConnectEx`) | 0.76 ms (0.60 ms) | 2,016--2,042 ms, refused |
/// | Windows 11 26200 | blocking `socket.connect` | 0.58 ms (0.58 ms) | 2,031--2,056 ms, refused |
/// | WSL2 Ubuntu 24.04, kernel 6.6 | `asyncio` selector (`epoll`) | 13.5 ms (7.2 ms) | 0.15--0.47 ms, refused |
/// | WSL2 Ubuntu 24.04, kernel 6.6 | blocking `socket.connect` | 1.45 ms (0.44 ms) | 0.14 ms max, refused |
///
/// The `asyncio` Linux tail is the interpreter's event loop as much as the kernel's -- the blocking
/// connect on the same host and load tops out at 1.45 ms -- and Linux refuses a closed port at once,
/// so there the bound is never paid. Fifty milliseconds is over 60 times the Windows maximum and over
/// 30 times the blocking Linux one, and it is about what a machine with no node pays, ONCE per
/// invocation (`MintRetry::NotAgain`). End to end on Windows -- a Release build of the launcher, the
/// MISS and HIT rows against one remote cache, a machine with no node against one whose node mints
/// -- the difference measured 59--73 ms.
///
/// **What it measures is the KERNEL's handshake, not the node.** A loopback connect completes at
/// the SYN-ACK, which the kernel answers from the listening socket's backlog whatever the node's
/// own threads are doing; the node's `accept` comes later and is not waited for. So a RUNNING node
/// misses the bound only when that backlog is full or the host cannot run the kernel's side of the
/// handshake in time -- and that is **unmeasured**: a busy local node under a `-j 60` build is
/// #1668's condition, and none of the figures above was taken under it.
///
/// **It fails toward fewer tickets, never a failed build.** A running but saturated node that misses
/// the bound reads as one that did not answer and is REMEMBERED (`MintRetry::NotAgain`): the rest of
/// that invocation presents no ticket -- its lease and its compile at a remote worker included -- and
/// each exchange a node-backed cache or scheduler refuses for it ends in a local compile.
inline constexpr std::chrono::milliseconds LoopbackConnectBound { 50 };

/// Why no ticket was presented.
///
/// **PRIVATE: persisted and transmitted nowhere**; its row's reason is what the invocation records.
enum class MintFailure : std::uint8_t
{
    NoSource, ///< Nothing configured names the port this machine's node answers on.
    /// This machine's node did not answer: not running, not on that port, not completing the
    /// loopback connect within the bound (a saturated node, #1668), or accepting and then stalling.
    Unreachable,
    Refused,  ///< This machine's node refused to mint.
    NoTicket, ///< This machine's node answered with no ticket in its reply.
    Last,     ///< Enumerator count.
};

/// Whether a failed mint is asked again by the next exchange of the same process (#1664).
///
/// **PRIVATE: persisted and transmitted nowhere.**
enum class MintRetry : std::uint8_t
{
    /// Asked again: the node ANSWERED, so asking cost a loopback round trip, and its answer can
    /// differ per audience or per draw -- a malformed audience, a nonce it could not draw.
    PerExchange,
    /// Remembered for the process, since asking again could not learn anything the first attempt
    /// did not. For a node that did not answer, asking again costs up to the connect bound (nothing
    /// where a closed loopback port is refused at once, as Linux does), and the exchange's whole
    /// total for a node that accepted and then stalled. For no source, the configuration that named
    /// none cannot change within the process, and asking costs nothing, so remembering it only keeps
    /// its line from repeating. Stale for the life of the object that remembers it, which every owner
    /// keeps one-shot (a launcher's one compile, a node's one operator verb; see `TicketCredentials`),
    /// and it fails CLOSED -- no ticket, which a node-backed cache refuses and the launcher answers by
    /// compiling locally.
    NotAgain,

    Last, ///< Enumerator count.
};

/// One row of `MintFailureTable`.
struct MintFailureRow
{
    MintFailure failure;     ///< Which failure.
    std::string_view reason; ///< What the invocation records: FIXED, so `--show-stats` tallies a cause, not a compile.
    MintRetry retry;         ///< Whether a later exchange of the same process asks again.
};

/// Every way a mint fails, the reason each is recorded under, and whether it is asked again.
inline constexpr EnumTable<MintFailure, MintFailureRow> MintFailureTable { {
    { .failure = MintFailure::NoSource,
      .reason = "no machine ticket: nothing configured names the port this machine's node answers on, so it could "
                "not be asked",
      .retry = MintRetry::NotAgain },
    // EVERY transport failure, deliberately broader than `MarksUnreachable`, which remembers only a
    // connect that never completed: that memo steers a FLEET away from a machine for a TTL, so it
    // errs narrow, while this one lasts one compile and fails closed. A node that accepted and then
    // stalled (`Expired`) would otherwise charge the exchange's whole total to every exchange.
    { .failure = MintFailure::Unreachable,
      .reason = "no machine ticket: this machine's node did not answer MINT-TICKET",
      .retry = MintRetry::NotAgain },
    { .failure = MintFailure::Refused,
      .reason = "no machine ticket: this machine's node refused MINT-TICKET",
      .retry = MintRetry::PerExchange },
    { .failure = MintFailure::NoTicket,
      .reason = "no machine ticket: this machine's node answered MINT-TICKET with none",
      .retry = MintRetry::PerExchange },
} };

static_assert(RowsInEnumeratorOrder(MintFailureTable, &MintFailureRow::failure),
              "MintFailureTable must hold one row per MintFailure, in enumerator order");

/// @param failure Why no ticket was presented.
/// @return The fixed reason the invocation records it under.
[[nodiscard]] constexpr std::string_view ReasonFor(MintFailure failure) noexcept
{
    return MintFailureTable[static_cast<std::size_t>(failure)].reason;
}

/// @param failure Why no ticket was presented.
/// @return Whether a later exchange of the same process asks this machine's node again.
[[nodiscard]] constexpr MintRetry RetryFor(MintFailure failure) noexcept
{
    return MintFailureTable[static_cast<std::size_t>(failure)].retry;
}

} // namespace FastCache::Cc
