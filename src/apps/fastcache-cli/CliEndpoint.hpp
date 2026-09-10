// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
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
    std::string secret {};   ///< Empty means present nothing.

    /// Whether there is anything to present.
    /// @return True when a secret is set.
    [[nodiscard]] bool Configured() const noexcept
    {
        return !secret.empty();
    }
};

} // namespace FastCache::Cli
