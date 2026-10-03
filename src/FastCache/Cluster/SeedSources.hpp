// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/SrvResolver.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file SeedSources.hpp
/// Where a node not on the office LAN finds the fleet: an endpoint remembered from a past
/// join, then an operator's `--fleet-seed`, then a DNS SRV record -- in that order, because a
/// node that has already joined a fleet should walk back to it before it asks a flag or the
/// network to name one for it, and a flag an operator typed should be tried before a record
/// nobody here wrote.
namespace FastCache::Cluster
{

/// The port a bare host seed takes when it names no port of its own.
inline constexpr std::uint16_t DefaultSeedPort = 6674;

/// Where a seed came from.
///
/// **Private: the enumerator order IS the order seeds are tried** (spec section 3). Remembered
/// first, because a node that has already joined a fleet should walk back to it before it asks
/// anybody else; the operator's `--fleet-seed` next, because it was typed for this purpose; DNS
/// SRV last, because it is the one nobody here wrote.
enum class SeedSource : std::uint8_t
{
    Remembered,    ///< An endpoint kept from a past join; see `FleetEndpoints.hpp`.
    FleetSeedFlag, ///< `--fleet-seed`, as the operator typed it.
    DnsSrv,        ///< `_fastcache._tcp.<suffix>`, sorted by priority then weight.
    Last,
};

/// What a seed source is called, for diagnostics.
struct SeedSourceRow
{
    SeedSource source;     ///< The source this row describes.
    std::string_view name; ///< What a log line or a startup report calls it.
};

/// Every source, in the order `OrderSeeds` tries them.
inline constexpr EnumTable<SeedSource, SeedSourceRow> SeedSourceTable { {
    { .source = SeedSource::Remembered, .name = "remembered" },
    { .source = SeedSource::FleetSeedFlag, .name = "--fleet-seed" },
    { .source = SeedSource::DnsSrv, .name = "dns-srv" },
} };
static_assert(RowsInEnumeratorOrder(SeedSourceTable, &SeedSourceRow::source),
              "SeedSourceTable must hold one row per SeedSource, in the order seeds are tried");

/// One seed, and which source produced it.
struct SeedCandidate
{
    std::string endpoint; ///< `host:port`, normalized by `NormalizeSeed`.
    SeedSource source {}; ///< Where it came from.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(SeedCandidate const&, SeedCandidate const&) = default;
};

/// Normalize seed text into `host:port`.
///
/// Tries `ParseDialEndpoint` first -- a host and a usable port -- and, only when that finds no
/// port separator at all, accepts the text as a bare host taking `DefaultSeedPort`. Text that
/// looks like it names a port but does not resolve to one (an empty host, a port of zero) is
/// refused rather than silently reinterpreted as a hostname: `:6674` and `office-a:0` both split
/// cleanly and name nobody.
/// @param text `host`, `host:port`, or `[v6]:port`.
/// @return `host:port`, or nullopt when this names no machine.
[[nodiscard]] std::optional<std::string> NormalizeSeed(std::string_view text);

/// The SRV name a fleet answers under, for a given DNS suffix.
/// @param dnsSuffix The primary DNS suffix; empty when this machine has none.
/// @return `_fastcache._tcp.<dnsSuffix>`, or empty when @p dnsSuffix is empty -- no domain
///         means no SRV query is made at all, rather than one made against a bare label.
[[nodiscard]] std::string SrvQueryName(std::string_view dnsSuffix);

/// Order every seed a node should try dialling, remembered first, then flagged, then DNS SRV.
///
/// The SRV list is sorted by priority ascending and weight descending -- a fixed order rather
/// than RFC 2782's weighted random draw, so the walk this produces is reproducible and testable.
/// A seed already present under an earlier source keeps that source: normalizing two spellings of
/// the same endpoint (`office-a` and `office-a:6674`) to one candidate is what lets a remembered
/// endpoint outrank the same machine named again by a flag or a record.
/// @param remembered Endpoints kept from a past join, in `FleetEndpoints` record order.
/// @param flagSeeds `--fleet-seed` values, in the order they were given.
/// @param srv What a DNS SRV query for this fleet returned (`ISrvResolver::Lookup`).
/// @return The seeds to try, in the order to try them.
[[nodiscard]] std::vector<SeedCandidate> OrderSeeds(std::span<std::string const> remembered,
                                                    std::span<std::string const> flagSeeds,
                                                    std::span<SrvTarget const> srv);

} // namespace FastCache::Cluster
