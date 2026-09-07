// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Whether a daemon's refusal describes a condition the whole BUILD will hit.
///
/// **The launcher is a per-translation-unit process with no sink**, which is the
/// whole difficulty. A refusal that will recur for every unit of a build is not one
/// an operator should have to run `--show-stats` to discover -- but a line per unit
/// is thousands of lines and gets filtered, which is worse than silence because it
/// buries everything else too.
///
/// So a persistent refusal is announced at most once per interval, and a transient
/// one is not announced at all. Which is which is a COLUMN, because the distinction
/// is a property of the code rather than of the call site: `UnsupportedVersion` is
/// true of the daemon and will be true of the next unit and the one after, while a
/// `MalformedValue` is about one stored object and says nothing about the next.
///
/// The motivating case is #815, recorded in the rulebook: an 0.1.0 daemon refused a
/// wire-3 launcher, everything degraded exactly as designed, and the only trace was
/// a `STATUS` line and a `--show-stats` tally nobody reads. A whole team built
/// through a cache that was answering "no" to every request, for weeks.
struct PersistentRefusal
{
    CompileCacheWire::ErrorCode code; ///< The refusal this row governs.
    /// What an operator should do about it, in one clause.
    ///
    /// Carried rather than derived, because the wire's own `defaultMessage` says
    /// what happened and this says what it MEANS for the build -- and the second is
    /// the half a developer watching a slow build actually needs.
    std::string_view remedy;
};

/// Refusals that describe the daemon rather than the request.
///
/// Deliberately short. A code earns a row only when it will still be true for the
/// next translation unit: adding a transient one turns this into a per-unit warning
/// with a throttle, which is the thing it exists to avoid.
inline constexpr std::array PersistentRefusalTable {
    PersistentRefusal { .code = CompileCacheWire::ErrorCode::UnsupportedVersion,
                        .remedy = "this launcher and that daemon cannot speak to each other; rebuild and redeploy "
                                  "both, they are a version pair" },
    PersistentRefusal { .code = CompileCacheWire::ErrorCode::Unauthenticated,
                        .remedy = "the daemon requires a credential this launcher did not present; set "
                                  "FASTCACHE_TOKEN or check the token file" },
};

/// Whether this refusal is one the whole build will meet.
/// @param code The daemon's refusal code.
/// @return The row, or nullptr when the refusal is about this request alone.
[[nodiscard]] PersistentRefusal const* PersistentRefusalFor(CompileCacheWire::ErrorCode code) noexcept;

/// How often one persistent refusal may be announced.
///
/// Five minutes, chosen against a build rather than as a round number: long enough
/// that a thousand-unit build produces one line and not a thousand, short enough
/// that somebody who fixes the daemon and rebuilds sees the message stop. It is a
/// ceiling on noise, not a guarantee of exactly one -- see `ShouldAnnounceRefusal`.
inline constexpr std::chrono::seconds RefusalNoticeInterval { 300 };

/// Whether to print this refusal now, throttled through the state directory.
///
/// **Racy by construction and deliberately not locked.** Every unit of a parallel
/// build is a separate process and they will occasionally both decide to announce;
/// the cost is a duplicate line, and the cost of a lock on a build's hot failure
/// path is worse. What matters is that a thousand units produce a handful of lines
/// rather than a thousand.
///
/// An unresolvable state directory answers TRUE, once, rather than false: a machine
/// that cannot persist the stamp is one where suppressing the message would make it
/// permanently silent, and this exists precisely to break a silence.
///
/// @param stateDir Where to keep the stamp; empty when none could be resolved.
/// @param endpoint The daemon that refused, so two daemons throttle separately.
/// @param code The refusal.
/// @param now The clock reading to compare and record.
/// @return True when the caller should print.
[[nodiscard]] bool ShouldAnnounceRefusal(std::filesystem::path const& stateDir,
                                         std::string_view endpoint,
                                         CompileCacheWire::ErrorCode code,
                                         std::chrono::system_clock::time_point now,
                                         std::chrono::seconds interval = RefusalNoticeInterval);

/// The line to print for a persistent refusal.
/// @param endpoint The daemon that refused.
/// @param row Its table row.
/// @param detail The daemon's own words, which may be empty.
/// @return One line, without a trailing newline.
[[nodiscard]] std::string RefusalNoticeLine(std::string_view endpoint,
                                            PersistentRefusal const& row,
                                            std::string_view detail);

} // namespace FastCache::Cc
