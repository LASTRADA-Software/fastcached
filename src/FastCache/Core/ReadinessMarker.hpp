// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Which binary's startup line this row describes.
enum class ReadinessMarker : std::uint8_t
{
    Daemon = 0,  ///< `fastcached`.
    CompileNode, ///< `fastcache-compile-node`.
    Last,        ///< Count; never a marker.
};

/// What a readiness line actually reports.
///
/// **Neither row is `Bound`, and there is deliberately no such enumerator.** Both
/// markers are logged strictly after their listener is already accepting, so a
/// fixture that waits on one and then reports a *bind* failure names a fact that was
/// true long before -- which is exactly what
/// [#652](https://github.com/LASTRADA-Software/fastcached/issues/652) found in
/// `node-scratch-isolation-e2e.ps1`, and what two rulebook files described the
/// node's marker as. Leaving the enumerator out means the mistake cannot be spelled
/// here.
///
/// `Unstated` is the zero value, so a row that omits the column has it and the
/// `static_assert` below refuses the build. That is `BindFailurePolicy`'s idiom and
/// it is the answer to the `0 of 0` shape
/// ([#736](https://github.com/LASTRADA-Software/fastcached/issues/736)): a fact
/// nobody stated must not read as a fact that happens to be false.
enum class ReadinessFact : std::uint8_t
{
    /// No row may hold this. Present so omission is a build failure.
    Unstated = 0,

    /// Bound, and every acceptor is parked in `accept()`. Nothing else is promised.
    Accepting,

    /// Accepting, AND the background work a client depends on is running. Later than
    /// `Accepting`, and EARLIER than any claim about that work having finished.
    Serving,
};

/// One startup line that something outside this repository waits on.
///
/// ## Why these literals are a table and not four format strings
///
/// `compile node ready` is a readiness contract for **four fixtures in two
/// languages** and was named by no constant and pinned by no test
/// ([#654](https://github.com/LASTRADA-Software/fastcached/issues/654)). Rewording a
/// human-facing log line is an ordinary thing to do; every unit test would pass, the
/// build would be green, and four fixtures would then wait on a string nothing
/// writes -- each failing by TIMEOUT, the slowest and least informative failure
/// there is, reported against whatever the fixture happened to be waiting for.
///
/// The worse case is already on record and involved no rename at all: #365 changed
/// what the node's line MEANT, from *surveyed* to its present sense, while every
/// fixture went on matching the string, and three include-tree walks silently
/// stopped being serialised (#634). **A rename breaks loudly but late; a re-meaning
/// does not break at all** -- which is why `fact` and `meaning` are columns rather
/// than comments, and why the test over this table asserts the text byte for byte.
///
/// The daemon's row is here for the same reason and to avoid a second source of
/// truth: `ReadinessAnnouncer` reads its text from this table rather than keeping
/// its own copy, or the fix for #654 would contain #654.
struct ReadinessMarkerRow
{
    /// The enumerator this row describes.
    ReadinessMarker marker {};

    /// The literal a waiter greps for.
    ///
    /// A wire constant in all but name: the waiters are shell, PowerShell and a YAML
    /// workflow step, none of which this build recompiles, so changing it changes a
    /// published interface. Keep the bytes, and change the *meaning* only in the
    /// direction that makes a waiter wait LONGER.
    std::string_view text;

    /// What the line reports. Never `Bound` -- see `ReadinessFact`.
    ReadinessFact fact {};

    /// One sentence a fixture author reads before waiting on this line.
    ///
    /// `static_assert`ed non-empty, because the column exists to be read at the
    /// moment somebody reaches for the marker as a signal for something it does not
    /// promise. A row stating only the verdict would have left the node's ordering
    /// unwritten, which is how two rulebook files came to call it *bound*.
    std::string_view meaning;
};

/// Every readiness line this repository publishes, in enumerator order.
inline constexpr EnumTable<ReadinessMarker, ReadinessMarkerRow> ReadinessMarkerTable { {
    ReadinessMarkerRow { .marker = ReadinessMarker::Daemon,
                         .text = "ready, accepting connections",
                         .fact = ReadinessFact::Accepting,
                         .meaning = "Every accept loop is armed. Emitted by ReadinessAnnouncer once the last acceptor has "
                                    "armed, which is strictly later than the bind (#646). Waited on by bench/runner.py's "
                                    "READY_MARKER and by build.yml's packaged-service step." },
    ReadinessMarkerRow { .marker = ReadinessMarker::CompileNode,
                         .text = "compile node ready",
                         .fact = ReadinessFact::Serving,
                         .meaning = "The 0xFC surface is accepting and the heartbeat thread is running. Later than the bind "
                                    "AND later than the accept start, which main.cpp separates deliberately; earlier than "
                                    "the toolchain survey, which #365 moved onto the heartbeat thread, so the toolchain "
                                    "count on the line is what the node is BRINGING UP. Waited on by e2e-common.sh's "
                                    "E2eNodeReadyMarker, node-scratch-isolation-e2e.ps1, build.yml and "
                                    "check-e2e-helpers.sh's stand-in node." },
} };
static_assert(RowsInEnumeratorOrder(ReadinessMarkerTable, &ReadinessMarkerRow::marker));

/// No row may leave its verdict or its explanation unstated.
static_assert(std::ranges::all_of(ReadinessMarkerTable, [](ReadinessMarkerRow const& row) {
    return row.fact != ReadinessFact::Unstated && !row.text.empty() && !row.meaning.empty();
}));

/// The literal a waiter greps for.
/// @param marker Which binary's line.
/// @return Its text, without the values the line goes on to format.
[[nodiscard]] constexpr std::string_view ReadinessMarkerText(ReadinessMarker marker) noexcept
{
    return ReadinessMarkerTable[static_cast<std::size_t>(marker)].text;
}

/// What that line reports.
/// @param marker Which binary's line.
/// @return The fact, which is never `Bound` because no such enumerator exists.
[[nodiscard]] constexpr ReadinessFact ReadinessFactOf(ReadinessMarker marker) noexcept
{
    return ReadinessMarkerTable[static_cast<std::size_t>(marker)].fact;
}

} // namespace FastCache
