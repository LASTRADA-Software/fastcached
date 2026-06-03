// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>

#include <cstdint>
#include <vector>

namespace FastCache
{

/// On-the-wire CAS token. Monotonically increasing for the storage instance.
/// 0 is reserved to mean "no CAS value known" — callers can use it when the
/// protocol does not transmit a CAS.
using CasToken = std::uint64_t;

/// A cached value with its associated metadata.
struct CacheEntry
{
    /// Opaque payload bytes.
    std::vector<std::byte> value;

    /// memcached "flags" word — opaque to the server, returned verbatim on get.
    std::uint32_t flags { 0 };

    /// Compare-and-swap token; updated on every mutation that produces a
    /// new value. Comparison is against the caller-provided expected value;
    /// the storage layer is the single source of truth for these tokens.
    CasToken cas { 0 };

    /// Absolute expiry time; TimePoint::max() means "never".
    TimePoint expiry { TimePoint::max() };

    /// Generation counter (for flush_all). Entries with a generation lower
    /// than the storage's current "live" generation are treated as missing.
    std::uint64_t generation { 0 };

    /// Last successful read time. Used by the meta protocol's `l` flag
    /// ("seconds since last access"). Updated by storage backends on every
    /// hit; defaults to TimePoint::min() for entries that have never been
    /// read since insertion.
    TimePoint lastAccess { TimePoint::min() };

    /// Stale marker. Set by the meta protocol's `md I` and `ms I` flags to
    /// indicate the entry is technically expired but still readable for
    /// recache-coordination purposes. A reader sees `X` in the response
    /// flag set. Flipped on by `IStorage::MarkStale`.
    bool stale { false };

    /// True once the entry has been returned to a client by a successful
    /// `Get`. Drives the `evicted_unfetched` / `expired_unfetched` stats
    /// (entries discarded before any client ever read them). Reset to
    /// `false` on insertion and on every value-rewriting mutation.
    bool fetched { false };
};

/// Result of a get-style operation that needs to distinguish "miss" from
/// "found and here's the value".
struct GetResult
{
    bool found { false };
    CacheEntry entry;
};

} // namespace FastCache
