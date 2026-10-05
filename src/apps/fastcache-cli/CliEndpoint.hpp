// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/SecureBytes.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace FastCache::Cli
{

/// @file CliEndpoint.hpp
/// Where to dial, how long to wait, and what to present.
///
/// Separate from `SocketExchange.hpp` so the command-line parser can name these
/// without reaching `Net/`. That keeps the parse a pure function over argv -- no
/// sockets, no clock, no environment -- which is what makes the whole CLI surface
/// testable in a binary that never opens a connection.

/// Where to dial.
struct Endpoint
{
    std::string host {};      ///< Host or address. IPv6 literals unbracketed, as `Net/TcpClient` takes them.
    std::uint16_t port { 0 }; ///< Port. Zero means unset.

    /// Whether this endpoint names somewhere to dial.
    /// @return True when both halves are set.
    [[nodiscard]] bool Configured() const noexcept
    {
        return !host.empty() && port != 0;
    }
};

/// @p endpoint as `host:port`, an IPv6 literal bracketed so its port stays readable.
/// @param endpoint Where.
/// @return The text.
[[nodiscard]] inline std::string EndpointText(Endpoint const& endpoint)
{
    return endpoint.host.contains(':') ? std::format("[{}]:{}", endpoint.host, endpoint.port)
                                       : std::format("{}:{}", endpoint.host, endpoint.port);
}

/// How long to wait.
///
/// Generous, because an operator typed the command and is watching -- the same
/// reasoning `ClusterAdminCli` records for its ten seconds. A CLI that gives up in
/// 500 ms on a loaded server is a CLI people stop believing.
struct DialTimeouts
{
    std::chrono::milliseconds connect { 5'000 }; ///< Cap on establishing the connection.
    std::chrono::milliseconds io { 10'000 };     ///< Cap on each read or write.
};

/// The credential to present, if any.
struct Credential
{
    std::string username {}; ///< Empty for the `requirepass` form, which is the usual one.
    /// Empty means present nothing. Held where it is wiped on release (#1578): the plain copies
    /// left are the ones a wire needs spelled out, stated where they are made.
    SecureString secret {};

    /// Whether there is anything to present.
    /// @return True when a secret is set.
    [[nodiscard]] bool Configured() const noexcept
    {
        return !secret.empty();
    }
};

/// What a `0xFC` connection presents, decided per endpoint it dials (`Cc::ChooseCredential`).
///
/// **The token goes to the endpoint it was given for, and nowhere else**: `--addr`, never a leader
/// a `NotLeader` redirect named. Every other machine is shown a ticket minted by this machine's own
/// node, which is how a node admits a machine; loopback is admitted as itself and shown nothing.
struct NodeCredentials
{
    Credential const* password { nullptr }; ///< The configured token; borrowed, and may be null.
    std::string passwordFor {};             ///< `host:port` the token belongs to, as `EndpointText` spells it.
    /// `--mint-from`: where this machine's node answers `MINT-TICKET`. Unset means the port of the
    /// endpoint dialled; the host is loopback either way (`Cc::TicketSourceFor`).
    Endpoint mintFrom {};
};

} // namespace FastCache::Cli
