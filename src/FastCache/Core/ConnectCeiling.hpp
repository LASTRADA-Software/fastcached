// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>

namespace FastCache
{

/// @file ConnectCeiling.hpp
/// The one rule for capping a connect timeout, header-only and std-only so the launcher, which links
/// nothing of `FastCache`, `fastcache-cli` and the node all spell it the same way.

/// @p connect, at most @p ceiling.
///
/// Capped rather than replaced: an operator who set a SHORTER connect timeout keeps it. A
/// non-positive connect is `core::net::DialOptions`' spelling of *leave the platform default in
/// place*, which may be minutes, so it takes the ceiling too -- or `FASTCACHE_CONNECT_TIMEOUT=0s`
/// would give the dial the cap exists for the longest wait of all. Every cap asks this, whatever it
/// caps -- an exchange budget (`Cc::WithConnectAtMost`) or a dial's own timeouts (`fastcache-cli`'s
/// mint) -- so the non-positive rule is written once.
/// @param connect The connect timeout as configured.
/// @param ceiling The longest the connect may take; positive.
/// @return @p connect, at most @p ceiling.
[[nodiscard]] constexpr std::chrono::milliseconds ConnectAtMost(std::chrono::milliseconds connect,
                                                                std::chrono::milliseconds ceiling) noexcept
{
    return connect > std::chrono::milliseconds::zero() ? std::min(connect, ceiling) : ceiling;
}

} // namespace FastCache
