// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Dispatch.hpp"

#include <FastCache/Cli/UsageDoc.hpp>

#include <cstdint>
#include <filesystem>
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

/// What the launcher did about DISTRIBUTION on one compile — an axis of its own,
/// beside `Outcome` rather than folded into it.
///
/// **Two independent facts.** A compile can be a cache miss *and* a dispatch
/// failure at the same time, so neither can stand in for the other: recording a
/// dispatch failure as `Outcome::Unavailable` would inflate a *cache* bucket for a
/// *fleet* event and send an operator to look at the daemon, which is the
/// conflation `RecordFallback`'s rule already refuses from the other side.
///
/// Before this axis existed there was no third place for it to go, so it went
/// nowhere: every non-answer from `Dispatch` ended at a `FASTCACHE_VERBOSE`-gated
/// stderr line that deliberately leaves the recorded outcome alone. A fleet in
/// which *every* dispatch failed therefore produced a perfectly ordinary miss rate
/// and total silence about distribution
/// ([#427](https://github.com/LASTRADA-Software/fastcached/issues/427)) — the shape
/// [#236](https://github.com/LASTRADA-Software/fastcached/issues/236) already cost
/// this project once, arriving one layer further in.
///
/// **Not attempted, refused here, declined and unreachable are four states, not a
/// `bool` and not a count.** A launcher with no scheduler configured never
/// distributes and must report *no fleet*; counted as a failure it would report a
/// 100 % dispatch failure rate forever, which is precisely what `NoUpstream`'s
/// honest `false` did to the node's upstream-store figure.
enum class DispatchOutcome : std::uint8_t
{
    /// This column did not exist when the line was written. **Not** "no fleet": a
    /// pre-upgrade record knows nothing about dispatch in either direction, so it
    /// is left out of the distribution report entirely rather than counted as
    /// anything — the treatment `Record::hasPhaseColumns` gives the phase
    /// histograms, for the same reason.
    ///
    /// Enumerator zero deliberately, so a `Record` nobody filled in reports
    /// *unknown* instead of making a claim about somebody's fleet.
    Unknown,
    /// No scheduler is configured: this launcher does not distribute at all. An
    /// absence, never a failure, and the whole reason it is not a `bool`.
    NotConfigured,
    /// A scheduler is configured and this compile never reached the dispatch
    /// decision — the cache served it, or it was uncacheable, or the key was never
    /// derived. Nothing about the fleet can be read from it either way.
    NotAttempted,
    /// The launcher itself refused to dispatch: a command line it cannot account
    /// for, a dispatch preprocess that failed, a toolchain whose fingerprint does
    /// not identify it. The fleet was never asked, so this says nothing about the
    /// fleet — it is fixed on THIS machine, which is why it is not `Declined`.
    Refused,
    /// The fleet declined — no worker on this toolchain, no capacity, the key
    /// already in flight, or the worker refusing the job. Ordinary; the operator's
    /// question is about the fleet's size or shape.
    Declined,
    /// The scheduler or the worker could not be reached, or an exchange broke. Also
    /// ordinary, and fixed somewhere else entirely: a machine that is down or a
    /// network that is not carrying, rather than a fleet that is merely busy. That
    /// difference is the whole point of splitting it from `Declined`.
    Unreachable,
    /// A worker answered about a compile other than the one it was asked for, and
    /// this client refused the object
    /// ([#280](https://github.com/LASTRADA-Software/fastcached/issues/280)). Kept
    /// apart for the reason `DispatchStatus::Mismatched` gives: every other state
    /// here is a fleet declining to help, and this one is a defect somebody has to
    /// look at. Folded in with `Unreachable` it would read as a network blip.
    Mismatched,
    /// A worker ran the compiler and this client did not keep the object — a
    /// non-zero remote exit code retried locally, or an artefact that could not be
    /// written. The exchange worked and the compile was still done twice.
    ///
    /// **A state rather than a reason on `Dispatched`.** The reports rate
    /// `Dispatched` against what was asked of the fleet, so a fleet whose every
    /// result was thrown away would headline *100% dispatched* with the
    /// contradiction relegated to a free-text tally underneath — a real failure
    /// wearing an ordinary-looking number, which is #427's own defect one layer
    /// further in. A state that is only true when read together with a string is
    /// not a state. The reason still says WHICH of the three it was.
    Discarded,
    /// A worker ran the compiler and this client used the object. Distribution
    /// worked, with nothing to qualify.
    Dispatched,
    /// The enumerator count; see `Core/EnumTable.hpp`. Never a state a `Record`
    /// carries.
    Last
};

/// @param outcome The dispatch outcome to name.
/// @return The stable token written to the log (also parsed back when reporting).
[[nodiscard]] std::string_view ToStringView(DispatchOutcome outcome) noexcept;

/// How one `DispatchStatus` is recorded on the dispatch axis.
struct DispatchRecording
{
    /// The FIXED tally reason, under `RecordFallback`'s rule. Empty where the state
    /// alone says everything — a dispatch that worked, and a crossed reply, whose
    /// sentence the CACHE axis already carries under #280's rule and which would
    /// otherwise be printed twice in one report.
    std::string_view reason;
    DispatchOutcome outcome {}; ///< How `--show-stats` buckets it.
};

/// Decide how to record what `Dispatch` returned.
///
/// **Here rather than in the launcher's `main.cpp`, and that is the point.** This
/// bridges two enumerations, neither of them `main`'s, and a decision that lives in
/// `main.cpp` is a decision nothing can assert — that file is in no test target, as
/// `CMakeLists.txt` says in as many words about the wire framing it already had to
/// move out for the same reason. `RowsInEnumeratorOrder` proves such a table is
/// *total*, never that it is *right*: swap the declined and unreachable rows and
/// every assertion still passes while an operator is sent to look at the network
/// for a fleet that was merely busy.
///
/// @param status What `Dispatch` returned.
/// @return The state to record and the reason to tally it under.
[[nodiscard]] DispatchRecording RecordingFor(DispatchStatus status) noexcept;

/// One recorded invocation.
struct Record
{
    Outcome outcome { Outcome::Unavailable };
    /// What this compile did about distribution. Defaults to `Unknown` rather than
    /// to any real state, so a caller that never set it makes no claim.
    DispatchOutcome dispatch { DispatchOutcome::Unknown };
    std::string prefetchGroup;   ///< FASTCACHE_PREFETCH_GROUP at the time of the compile.
    std::string source;          ///< Translation-unit path, for per-TU attribution.
    std::uint64_t valueBytes {}; ///< Cached payload size (0 when nothing moved).
    std::uint64_t elapsedMs {};  ///< Wall time this invocation took.
    /// Fall-back reason, tallied per cause by `--show-stats`. Empty on a hit, and
    /// on a miss that had nothing to fall back FROM -- but a miss CAN carry one:
    /// a dispatched compile whose reply did not belong to the request is refused
    /// and compiled locally (#280), which leaves the outcome an honest miss with a
    /// reason worth ranking.
    std::string detail;

    /// Why distribution did not help, tallied per cause beside — never inside —
    /// the cache's `detail` above. One map each, because a compile that missed the
    /// cache and then failed to dispatch has two causes and an operator fixes them
    /// in two different places; ranked together, a fleet failure would show up in
    /// the list somebody reads to decide whether the *daemon* is healthy.
    ///
    /// A FIXED string under `RecordFallback`'s rule, which bites harder here than
    /// on the cache axis: `Dispatch` formats the worker's endpoint into its
    /// declined message, so forwarding that text verbatim would produce one tally
    /// row per worker in the fleet instead of one per cause. The endpoint rides the
    /// verbose line instead.
    ///
    /// Empty when there is nothing to explain — and non-empty is not the same as
    /// failure: a compile a worker ran and the client then discarded carries a
    /// reason while staying `DispatchOutcome::Dispatched`, exactly as an honest
    /// MISS can carry a fall-back reason above.
    std::string dispatchDetail;

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

/// The launcher's per-user state directory, created on demand.
///
/// `%LOCALAPPDATA%/fastcache-cc` on Windows; `$XDG_STATE_HOME/fastcache-cc` or
/// `~/.local/state/fastcache-cc` elsewhere. Empty when none can be resolved,
/// which every caller must treat as "do not persist" rather than as an error:
/// nothing kept here is required for a correct build.
///
/// Exposed rather than private to the statistics log because it is no longer only
/// the log's: the toolchain-fingerprint cache lives here too. Two copies of the
/// platform `#if` would be two places for the location to drift, and a cache
/// written to one path and read from another is a cache that silently never hits.
[[nodiscard]] std::filesystem::path StateDirectory();

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
