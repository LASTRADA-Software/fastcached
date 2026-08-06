// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
    std::string cohort;          ///< FASTCACHE_COHORT at the time of the compile.
    std::string source;          ///< Translation-unit path, for per-TU attribution.
    std::uint64_t valueBytes {}; ///< Cached payload size (0 when nothing moved).
    std::uint64_t elapsedMs {};  ///< Wall time this invocation took.
    std::string detail;          ///< Fall-back reason; empty on hit/miss.

    // Phase breakdown of elapsedMs. A hit is not "cache latency": it also pays a
    // full preprocess to derive the key, so a slow hit needs these to attribute.
    std::uint64_t preprocessMs {}; ///< Deriving the key (preprocess + compiler id).
    std::uint64_t cacheMs {};      ///< Talking to the daemon (connect + transfer).

    /// Direct mode: validating the header manifest instead of preprocessing. When
    /// `directHit` is set, `preprocessMs` is zero because no preprocess ran — that
    /// substitution is the whole point, so the report separates the two.
    std::uint64_t directMs {};
    bool directHit {};
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

/// Fold the log into a human-readable report: totals plus a per-cohort
/// breakdown, hit rate, latency distributions, the fall-back reasons, and the
/// translation units that never hit.
/// @param cohortFilter When non-empty, report only this cohort.
/// @return The formatted report, or an explanatory line when the log is absent.
[[nodiscard]] std::string FormatReport(std::string_view cohortFilter);

/// Delete the log. @return True when the log is gone afterwards.
[[nodiscard]] bool ResetLog();

} // namespace FastCache::Cc
