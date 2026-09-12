// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "StatsSource.hpp"

#include <FastCache/Async/Task.hpp>

#include <cstdint>
#include <string>

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
    Tick,         ///< The render cadence fired.
    Sample,       ///< A stats reading arrived; `attempts` carries it, absence included.
    SampleFailed, ///< The source could not be read at all. NOT a sample of zeroes.
    Key,          ///< A keystroke arrived.
    Resize,       ///< The terminal geometry changed.
    Detached,     ///< The source went away mid-session.
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

    /// `Sample` only: what each source said, in the gatherer's own vocabulary.
    ///
    /// The RAW attempts rather than an already-chosen record, so the fold runs
    /// `ChooseStats` itself. That keeps the decision -- every source silent, the rich
    /// one down and the thin one up, one never asked -- inside the deterministic half
    /// where a fixture can reach it, instead of behind a socket.
    std::vector<StatsAttempt> attempts {};

    /// `SampleFailed` and `Detached`: why, in words for a person.
    std::string note {};

    /// `Key`: the decoded keystroke's bytes, as they arrived.
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
};

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
/// Production multiplexes a tick timer, a terminal reader and a sampler into one queue;
/// a fixture is a list. The loop cannot tell them apart, which is the point.
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
