// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/UsageDoc.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// What the launcher did with one compile — the outcome recorded per invocation.
enum class Outcome : std::uint8_t
{
    Hit,         ///< Served from the cache; no compiler run.
    Miss,        ///< Compiled and stored.
    Uncacheable, ///< Deliberately not cached (time macros, unparsable line).
    Unavailable, ///< Cache error; fell back to a real compile.
};

/// @param outcome The outcome to name.
/// @return The stable token written to the log (also parsed back when reporting).
[[nodiscard]] std::string_view ToStringView(Outcome outcome) noexcept;

/// One recorded invocation.
struct Record
{
    Outcome outcome { Outcome::Unavailable };
    std::string prefetchGroup;   ///< FASTCACHE_PREFETCH_GROUP at the time of the compile.
    std::string source;          ///< Translation-unit path, for per-TU attribution.
    std::uint64_t valueBytes {}; ///< Cached payload size (0 when nothing moved).
    std::uint64_t elapsedMs {};  ///< Wall time this invocation took.
    std::string detail;          ///< Fall-back reason; empty on hit/miss.

    // Phase breakdown of elapsedMs. A hit is not "cache latency": it also pays a
    // full preprocess to derive the key, so a slow hit needs these to attribute.
    std::uint64_t preprocessMs {}; ///< Deriving the key (preprocess + compiler id).
    std::uint64_t cacheMs {};      ///< Talking to the daemon (connect + transfer).

    /// Whether `preprocessMs`/`cacheMs` were actually recorded, as opposed to
    /// defaulted to zero because this record came from a log line written
    /// before those columns existed. Without this, a pre-upgrade hit would
    /// plot as a real zero-millisecond preprocess/cache sample rather than
    /// being left out of that histogram entirely.
    bool hasPhaseColumns {};

    /// Direct mode: validating the header manifest instead of preprocessing. When
    /// `directHit` is set, `preprocessMs` is zero because no preprocess ran — that
    /// substitution is the whole point, so the report separates the two.
    std::uint64_t directMs {};
    bool directHit {};

    /// Wall-clock time the invocation was recorded, as seconds since the Unix
    /// epoch. Zero means "unknown" — either a pre-upgrade log line (the column
    /// did not exist yet) or a caller that never set it — and is excluded from
    /// any time-bucketed view rather than plotted as an epoch-zero data point.
    std::uint64_t timestampUnixSeconds {};
};

/// Append one record to the per-user log, creating it on first use.
///
/// Every compile is its own short-lived process, so aggregation cannot live in
/// memory: each invocation appends a single line and the reporting path folds
/// them. Failures are swallowed — statistics must never break a build.
/// @param record The invocation to record.
void AppendRecord(Record const& record);

/// Absolute path of the log file (%LOCALAPPDATA%/fastcache-cc/invocations.log on
/// Windows, $XDG_STATE_HOME or ~/.local/state equivalent elsewhere). Empty when
/// no suitable directory can be resolved.
[[nodiscard]] std::string LogPath();

/// Fold the log into a human-readable report: totals plus a per-group
/// breakdown, hit rate, latency distributions, the fall-back reasons, and the
/// translation units that never hit.
/// @param groupFilter When non-empty, report only this prefetch group.
/// @param color Whether to emit ANSI SGR escapes; see StdoutSupportsColor.
/// @return The formatted report, or an explanatory line when the log is absent.
[[nodiscard]] std::string FormatReport(std::string_view groupFilter, UsageColor color = UsageColor::Plain);

/// Read every record from the log, applying the same prefetch group filter and
/// tolerance for short (pre-upgrade) lines that `FormatReport` does.
///
/// Exposed separately from `FormatReport` so a caller that wants the raw
/// records — the HTML report's trend chart, or a test — does not have to
/// scrape them back out of formatted text.
/// @param groupFilter When non-empty, return only this prefetch group's records.
/// @return The parsed records, in file order. Empty when the log is absent
///         or empty.
[[nodiscard]] std::vector<Record> ParseLog(std::string_view groupFilter);

/// Render the same data as `FormatReport`, as a self-contained HTML dashboard
/// (inline CSS/JS, no network dependency): headline hit rate, per-outcome
/// tallies, a hit-rate-over-time trend, latency histograms, ranked fall-back
/// reasons, a per-group comparison table, and the translation units that
/// never hit.
/// @param groupFilter When non-empty, report only this prefetch group.
/// @return The complete HTML document, or an explanatory plain-text line when
///         the log is absent (mirroring `FormatReport`'s empty-log message).
[[nodiscard]] std::string FormatHtmlReport(std::string_view groupFilter);

/// Delete the log. @return True when the log is gone afterwards.
[[nodiscard]] bool ResetLog();

} // namespace FastCache::Cc
