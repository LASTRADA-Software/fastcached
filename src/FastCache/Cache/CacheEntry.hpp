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
};

/// Result of a get-style operation that needs to distinguish "miss" from
/// "found and here's the value".
struct GetResult
{
    bool found { false };
    CacheEntry entry;
};

} // namespace FastCache
