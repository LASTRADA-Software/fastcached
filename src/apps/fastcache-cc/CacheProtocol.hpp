// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ITcpClient.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// How a cache round trip ended.
///
/// `Miss` and `Rejected` are deliberately separate. They were the same byte on
/// the pre-version wire, which meant a launcher talking to a daemon that could
/// not serve it saw an endlessly cold cache and reported nothing — the build
/// merely got slower, forever, with no clue why. Telling them apart is the whole
/// reason the wire carries a status space and an error code.
enum class CacheOutcomeKind : std::uint8_t
{
    Hit,       ///< The value was served. `value` holds it.
    Miss,      ///< The daemon has no such entry. Not an error.
    Rejected,  ///< The daemon refused the command. `code` and `message` say why.
    Transport, ///< The exchange never completed (socket error, short read).
};

/// The result of one FETCH or STORE.
struct CacheOutcome
{
    CacheOutcomeKind kind { CacheOutcomeKind::Transport };
    std::vector<std::byte> value;                                                     ///< Payload on a hit.
    CompileCacheWire::ErrorCode code { CompileCacheWire::ErrorCode::MalformedFrame }; ///< Valid when `kind == Rejected`.
    std::string message; ///< The daemon's own words, when it refused.

    /// @return True when the daemon served a value.
    [[nodiscard]] bool IsHit() const noexcept
    {
        return kind == CacheOutcomeKind::Hit;
    }
};

/// Describe an outcome for a diagnostic line — the daemon's own code and words
/// when it refused, so a launcher can say *why* a build is not caching rather
/// than only that it is not.
/// @param outcome The outcome to describe.
/// @return A short human-readable phrase; empty for a hit.
[[nodiscard]] std::string DescribeOutcome(CacheOutcome const& outcome);

/// FETCH one key over an already-connected client.
/// @param client Connected transport; not owned.
/// @param key The key to look up.
/// @return The outcome; `value` holds the stored bytes on a hit.
[[nodiscard]] CacheOutcome CacheFetch(ITcpClient& client, std::string_view key);

/// STORE one entry over an already-connected client.
/// @param client Connected transport; not owned.
/// @param request The fields to send.
/// @return The outcome; `kind == Hit` means the daemon acknowledged the write.
[[nodiscard]] CacheOutcome CacheStore(ITcpClient& client, CompileCacheWire::StoreRequest const& request);

} // namespace FastCache::Cc
