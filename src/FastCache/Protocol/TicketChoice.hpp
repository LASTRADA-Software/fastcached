// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

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

/// Why no ticket was presented.
///
/// **PRIVATE: persisted and transmitted nowhere**; its row's reason is what the invocation records.
enum class MintFailure : std::uint8_t
{
    NoSource,    ///< Nothing configured names the port this machine's node answers on.
    Unreachable, ///< This machine's node did not answer: not running, or not on that port.
    Refused,     ///< This machine's node refused to mint.
    NoTicket,    ///< This machine's node answered with no ticket in its reply.
    Last,        ///< Enumerator count.
};

/// One row of `MintFailureTable`.
struct MintFailureRow
{
    MintFailure failure;     ///< Which failure.
    std::string_view reason; ///< What the invocation records: FIXED, so `--show-stats` tallies a cause, not a compile.
};

/// Every way a mint fails, and the reason each is recorded under.
inline constexpr EnumTable<MintFailure, MintFailureRow> MintFailureTable { {
    { .failure = MintFailure::NoSource,
      .reason = "no machine ticket: nothing configured names the port this machine's node answers on, so it could "
                "not be asked" },
    { .failure = MintFailure::Unreachable, .reason = "no machine ticket: this machine's node did not answer MINT-TICKET" },
    { .failure = MintFailure::Refused, .reason = "no machine ticket: this machine's node refused MINT-TICKET" },
    { .failure = MintFailure::NoTicket, .reason = "no machine ticket: this machine's node answered MINT-TICKET with none" },
} };

static_assert(RowsInEnumeratorOrder(MintFailureTable, &MintFailureRow::failure),
              "MintFailureTable must hold one row per MintFailure, in enumerator order");

/// @param failure Why no ticket was presented.
/// @return The fixed reason the invocation records it under.
[[nodiscard]] constexpr std::string_view ReasonFor(MintFailure failure) noexcept
{
    return MintFailureTable[static_cast<std::size_t>(failure)].reason;
}

} // namespace FastCache::Cc
