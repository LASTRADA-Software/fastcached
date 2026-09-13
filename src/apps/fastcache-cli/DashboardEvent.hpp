// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "DashboardFrame.hpp"
#include "SocketExchange.hpp"
#include "StatsSource.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace FastCache::Cli
{

/// @file DashboardEvent.hpp
/// Everything that reaches the `live-stats` dashboard, as one ordered vocabulary.
///
/// **One stream rather than four seams, because the INTERLEAVING is the thing worth
/// testing.** With a clock seam, a terminal seam, a stats seam and a resize seam, a
/// case cannot say *a resize arrived between the fetch and the frame* without racing
/// something; with one ordered list it is an input, and every ordering that has ever
/// broken a dashboard becomes a case somebody can write down. That is `FleetHarness`'s
/// rule in this tree already -- a harness PLACES the interleaving rather than waiting
/// for one.
///
/// The property this buys, which is what makes *100% mockable* checkable rather than
/// aspirational: **the same event list renders byte-identical frames, every run.** One
/// assertion that fails the moment the loop reads a clock, an environment variable, a
/// terminal or a socket that is not in the list.

/// What happened.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted.
///
/// **The ugly ones are first-class deliberately.** A fake that only ever produces
/// well-formed samples on a tidy cadence tests a world that does not exist, and nothing
/// about it looks wrong -- which is the *fake more permissive than the thing it stands
/// for* rule. So a failed read, a departed source and a keystroke that lands mid-fetch
/// are events with names rather than states a test has to contrive.
enum class DashboardEventKind : std::uint8_t
{
    Tick,          ///< A frame is owed. The source decides when; the loop keeps no cadence.
    Sample,        ///< A reading arrived, raw; the session's reader decides what it says.
    SampleFailed,  ///< The source could not be read at all. NOT a sample of zeroes.
    Key,           ///< A keystroke arrived.
    StopRequested, ///< The operator asked to stop by a route that is not a keystroke.
    Resize,        ///< The terminal geometry changed.
    Detached,      ///< The source went away mid-session.
    Last,
};

/// One thing that happened, in the order it happened.
///
/// A flat aggregate rather than a variant, for one reason: a fixture is a LIST, and a
/// list of designated initializers is what a person can read and write. `TerminalRead`
/// next door is shaped the same way, and for the same reason.
///
/// The cost is honest and small -- an event carries fields its kind does not use -- and
/// it buys that adding a kind cannot silently change how existing fixtures parse.
struct DashboardEvent
{
    DashboardEventKind kind { DashboardEventKind::Tick };

    /// `Sample` and `SampleFailed`: when the reading was TAKEN, on the injected steady clock.
    ///
    /// A rate is a change divided by the time it took, and that time is measured rather than
    /// assumed. The nominal `--interval` would count the wait that was ASKED for instead of the
    /// one that happened -- `DrainWithin`'s defect -- so a sampler that fell behind would report
    /// every rate too high by exactly how far behind it was, and nothing would say so.
    ///
    /// A steady `TimePoint`, and the `static_assert` below the struct keeps it one. A wall clock
    /// is not a duration -- this tree has measured WSL2 stepping `CLOCK_REALTIME` backwards -- so
    /// #134 §9.4's *wall time steps back and no rate moves* holds because no wall time can be
    /// stored here at all.
    TimePoint at {};

    /// `Sample`, `cache` and `node` sessions: what each stats source said.
    ///
    /// The RAW attempts rather than an already-chosen record, so the decision -- every source
    /// silent, the rich one down and the thin one up, one never asked -- is made by the reader
    /// the fold was given, inside the deterministic half where a fixture can reach it, instead
    /// of behind a socket.
    std::vector<StatsAttempt> attempts {};

    /// `Sample`, `fleet` session: what the leader's admin document fetch produced.
    ///
    /// Raw for the reason `attempts` is: success or failure, it is the reader's to interpret. A
    /// `/fleet.txt` answer is a table rather than a stats attempt, so without its own member a
    /// fleet session could not stream at all. Disengaged on a `cache` or `node` sample.
    std::optional<std::expected<std::string, AdminError>> document {};

    /// `Sample` and `SampleFailed`, `node` session: what the node said about itself when this
    /// sample was taken, or nullopt when the sample carried no status.
    ///
    /// **Per sample, never a copy of the session's first answer.** Toolchains served, registrars,
    /// the scheduler role and the slots are what a `node` panel exists to show moving, so a status
    /// asked once and repeated would be a live view of the past. The version and uptime ride here
    /// too, which is why the node panel reads them from this rather than from a stats record.
    std::optional<CompileCacheWire::NodeStatusFields> nodeStatus {};

    /// `SampleFailed`: what the failure means as an outcome.
    ///
    /// A run that never read anything ends with this, and `Unreachable` and `Refused` are
    /// different exit codes -- 3 and 4 -- with different remedies. A failure that did not say
    /// which would force the loop to pick one for it.
    Outcome outcome { Outcome::Unreachable };

    /// `SampleFailed` and `Detached`: why, in words for a person.
    std::string note {};

    /// `Key`: the decoded keystroke's bytes, as they arrived.
    ///
    /// Never bytes nobody typed. A stop that reached this process some other way -- a signal
    /// on a run with no raw-mode terminal to decode a Ctrl-C byte -- is `StopRequested`, not a
    /// `Key` spelling `"\x03"` on its behalf. Both routes exist: a raw-mode terminal still
    /// delivers a real Ctrl-C here, and `IsQuitKey` still reads it.
    std::string keys {};

    /// `Resize`: the new geometry, as endo reports it.
    ///
    /// Two ints rather than a geometry struct, because `tui::Terminal` answers
    /// `columns()` and `rows()` and has no such struct. Wrapping them here would be
    /// a third spelling of something that already has two, which is the duplicate
    /// type this deliberately avoided the first time.
    ///
    /// Zero means *not reported yet* rather than a terminal of no width: nothing
    /// draws from these until a `Resize` has carried real ones.
    int columns { 0 };
    int rows { 0 };

    /// `Resize`: how many pixels a cell measures now, or nullopt when the terminal has not said.
    ///
    /// Rides the resize because a font change resizes the grid and the cells together, and an image
    /// drawn from the old cell size would be the wrong size in the new grid.
    std::optional<CellPixelSize> cellPixels {};
};

/// A wall `time_point` cannot be stored as when a reading was taken.
static_assert(std::is_same_v<decltype(DashboardEvent::at)::clock, std::chrono::steady_clock>,
              "DashboardEvent::at must be a steady-clock time: a wall clock is not a duration");

/// Where the dashboard's input comes from.
///
/// **One door, and it SUSPENDS.** A source that resolved synchronously would satisfy
/// this signature and make every property defined by waiting vacuous -- a tick that
/// arrives while a fetch is outstanding, a quit during a slow sample -- while appearing
/// to pass. That is the same rule `ITerminalChannel` states about its own `Read`, and it
/// is why the scripted implementation parks through the injected reactor rather than
/// returning a ready task: a fake that resolves what production suspends on cannot
/// exercise a suspension protocol.
///
/// Production multiplexes a terminal reader and a sampler into one queue and owes a frame
/// after each sample and after each resize -- it runs no tick timer. A fixture is a list.
/// The loop cannot tell them apart, which is the point.
class IDashboardEventSource
{
  public:
    IDashboardEventSource() = default;
    IDashboardEventSource(IDashboardEventSource const&) = delete;
    IDashboardEventSource(IDashboardEventSource&&) = delete;
    IDashboardEventSource& operator=(IDashboardEventSource const&) = delete;
    IDashboardEventSource& operator=(IDashboardEventSource&&) = delete;
    virtual ~IDashboardEventSource() = default;

    /// The next event, in order.
    ///
    /// Suspends until there is one. Resumes with `Detached` once the source is closed
    /// and drained, so the loop has one termination condition rather than a sentinel
    /// beside a flag.
    /// @return The event.
    [[nodiscard]] virtual Task<DashboardEvent> Next() = 0;

    /// Stop producing.
    ///
    /// An outstanding `Next()` resumes with `Detached`. Idempotent, because the loop
    /// closes on the way out of every exit path and a second close must not be a
    /// different event.
    virtual void Close() noexcept = 0;
};

} // namespace FastCache::Cli
