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

/// Default ceiling on a value the launcher will offer to the daemon.
///
/// 256 MiB, matching the daemon's own `--storage-max-value` default, so that out
/// of the box the launcher does not spend a build's time pushing something the
/// other side is certain to refuse. The two are separate settings on separate
/// processes that never negotiate -- the wire has no handshake by design -- so
/// this agreement is a chosen default, not a derived one, and either side can be
/// retuned without the other noticing.
// `ULL`, not `UL`: `unsigned long` is 32 bits on Win64 (LLP64), so a future
// edit raising this past 4 GiB would silently wrap there and nowhere else.
inline constexpr std::size_t DefaultMaxStoreBytes = 256ULL * 1024ULL * 1024ULL;

/// Whether a value of `valueBytes` is worth offering to the daemon at all.
///
/// A single object file can be enormous -- a C++23 translation unit built with
/// `-g` reached 356 MB in the report that prompted this -- and one entry that
/// size would dominate a cache sized for thousands of ordinary objects. Past the
/// limit the launcher stores nothing: the compile has already succeeded, so the
/// build is unaffected and only this one result stays uncached.
///
/// This is a *client* policy, deliberately checked before connecting rather than
/// left for the daemon to refuse. Sending it anyway costs the transfer on every
/// rebuild of that translation unit, and pays it to be told no.
/// @param valueBytes Size of the encoded value.
/// @param limitBytes The ceiling; **0 means no limit**, not "store nothing".
/// @return True when the value may be sent.
[[nodiscard]] constexpr bool IsStorableSize(std::size_t valueBytes, std::size_t limitBytes) noexcept
{
    return limitBytes == 0 || valueBytes <= limitBytes;
}

} // namespace FastCache::Cc
