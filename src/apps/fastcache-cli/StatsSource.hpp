// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliValue.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file StatsSource.hpp
/// Where `fastcache-cli stats` gets its numbers, and how it decides.
///
/// **There are three sources of wildly different richness and no single best one.**
/// Measured against this tree: the admin surface's `/metrics` carries roughly 102
/// catalogued counters plus 24 storage series and five more per present cache tier;
/// memcached's `stats` carries 24 fields; RESP `INFO` carries **seven**, in three
/// sections, and ignores any section argument it is given.
///
/// So a stats command that names one source is either thin or unavailable. The answer
/// is a ladder -- richest first -- with **one pure decision function** over what each
/// source said, which is what keeps this from becoming the "two commands discovering
/// the daemon two different ways" that #134 warns against. The chosen source is
/// reported as a field on stdout, not merely as a remark, because *which numbers am I
/// looking at* is something a machine consumer needs too.

/// One place stats can come from.
///
/// A private enum. The ladder's order IS this enum's order, richest first, which is
/// the one thing about it that is load-bearing.
enum class StatsOrigin : std::uint8_t
{
    /// The admin surface's Prometheus endpoint. Richest by a wide margin, and it needs
    /// no credential -- `/metrics` short-circuits above the `AdminCredential` gate. It
    /// does need the daemon to have been started with its metrics listener enabled,
    /// and it is on a different port from the data plane.
    Metrics,
    /// The node's own `NodeMetrics` verb over `0xFC`. Every counter its build carries,
    /// zeroes included, and it needs no second port and no credential a plain worker
    /// cannot check.
    ///
    /// **Below `/metrics` because it is narrower, not because it is worse**: it carries
    /// the counter catalogue and NOT the storage series or the per-tier ones, which the
    /// Prometheus renderer adds. Above `INFO` because it is an order of magnitude wider
    /// than seven fields -- and it is the only rung that answers at all against a
    /// `fastcache-compile-node`, which speaks no RESP.
    NodeMetrics,
    /// RESP `INFO` on the data port. Always present, and a small fixed set of
    /// fields -- deliberately not numbered here: that count belongs to the DAEMON's
    /// `INFO` handler, and a copy of it in the client is a claim nothing checks.
    /// The advisory states what was actually returned instead.
    Info,
    Last,
};

/// One origin's fixed properties.
struct StatsOriginSpec
{
    StatsOrigin origin;    ///< The enumerator this row describes.
    std::string_view name; ///< Stable lower-case name; reported as the `source` field.
    std::string_view what; ///< Where it is, for a remark an operator can act on.
    /// What choosing it costs, as the TAIL of a sentence the emitter opens by naming
    /// the source and the field count it observed. Empty when nothing.
    std::string_view caveat;
};

/// The origins, one row per enumerator, in enumerator order -- which is ladder order.
inline constexpr EnumTable<StatsOrigin, StatsOriginSpec> StatsOriginTable { {
    { .origin = StatsOrigin::Metrics, .name = "metrics", .what = "the admin surface's /metrics endpoint", .caveat = "" },
    { .origin = StatsOrigin::NodeMetrics,
      .name = "node-metrics",
      .what = "the node's own NodeMetrics verb over 0xFC",
      .caveat = "this is the counter catalogue only; /metrics adds the storage and "
                "per-tier series" },
    { .origin = StatsOrigin::Info,
      .name = "info",
      .what = "RESP INFO on the data port",
      .caveat = "start the daemon with its metrics listener enabled, or pass "
                "--admin-addr, for the full counter set" },
} };

static_assert(RowsInEnumeratorOrder(StatsOriginTable, &StatsOriginSpec::origin),
              "StatsOriginTable must hold one row per StatsOrigin, in enumerator order");

/// The row describing @p origin.
/// @param origin The origin.
/// @return Its row; never null for a value below `Last`.
[[nodiscard]] StatsOriginSpec const* DescriptorOf(StatsOrigin origin) noexcept;

/// What one source said when it was asked.
///
/// **Three states, not two, and the third is the one that gets collapsed.** A source
/// that was never asked -- because no admin port is known, because the operator
/// restricted the ladder -- is not a source that failed. Reporting *"/metrics did not
/// answer"* for an endpoint nothing dialled sends an operator to check a listener that
/// was never contacted. `asked` is what keeps those apart.
struct StatsAttempt
{
    StatsOrigin origin { StatsOrigin::Metrics }; ///< Which source.
    bool asked { false };                        ///< Whether it was contacted at all.
    std::optional<Value> record {};              ///< What it produced; nullopt when it produced nothing.
    std::string note {};                         ///< Why it produced nothing, or why it was not asked.
};

/// Choose which attempt to report and say what was covered.
///
/// Pure, so the interesting cases -- every source silent, the rich one down and the
/// thin one up, one never asked -- are tested without a daemon, an admin port or a
/// network. Which is the whole reason the gathering is somewhere else.
///
/// @param attempts What each source said, in any order.
/// @return The answer: the winning record with a `source` field prepended, or an
///         `Unreachable` conclusion naming what happened to each source.
[[nodiscard]] Answer ChooseStats(std::span<StatsAttempt const> attempts);

/// Parse a Prometheus exposition body into a record.
///
/// Comments and type declarations are skipped; a labelled series keeps its labels in
/// the field name (`fastcached_tier_items{tier="l1"}`), because dropping them would
/// merge two series into one field and silently report one tier's number for another.
/// @param body The response body.
/// @return A record in the order the series appeared.
[[nodiscard]] Value ParsePrometheus(std::string_view body);

/// Parse a RESP `INFO` body into a record.
///
/// `# Section` lines are skipped rather than turned into nesting: every key `INFO`
/// emits here is already unique, and a flat record is what the formats can all carry.
/// @param body The `INFO` payload.
/// @return A record in the order the fields appeared.
[[nodiscard]] Value ParseInfo(std::string_view body);

/// Ask every configured source.
///
/// A seam so the ladder's decision can be driven against scripted attempts. The
/// concrete implementation lives with `main`, which is the only place that knows the
/// endpoints.
class IStatsGatherer
{
  public:
    IStatsGatherer() = default;
    IStatsGatherer(IStatsGatherer const&) = delete;
    IStatsGatherer(IStatsGatherer&&) = delete;
    IStatsGatherer& operator=(IStatsGatherer const&) = delete;
    IStatsGatherer& operator=(IStatsGatherer&&) = delete;
    virtual ~IStatsGatherer() = default;

    /// Try every source and report what each said.
    /// @return One attempt per source, including the ones not asked.
    [[nodiscard]] virtual std::vector<StatsAttempt> Gather() = 0;
};

} // namespace FastCache::Cli
