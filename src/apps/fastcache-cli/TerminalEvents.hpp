// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "TerminalCapabilities.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>

#include <expected>
#include <memory>
#include <string>

namespace FastCache::Cli
{

/// @file TerminalEvents.hpp
/// The production producer of the dashboard's TERMINAL events: keystrokes, geometry and the
/// terminal going away. And, beside them, what the terminal can draw.
///
/// **This header names nothing from `vendor/`**, and that is the design rather than a tidiness
/// preference. The endo types this is built on live in `TerminalEvents.cpp` and
/// `TerminalEventStream.*`, which makes those the only place the two projects meet.
///
/// **Two types, so "start before reading" is the only thing that compiles.** A terminal event
/// source has to acquire the terminal (raw mode, then the queries the rung is decided from)
/// before it can produce anything, and it must not acquire it earlier than asked: a run that
/// never draws may compose one and never start it. With one interface and a `Start()` beside
/// `Next()`, reading first and starting twice both compile and only a comment says not to. Here
/// the unstarted value has nothing to call, `StartTerminal` consumes it, and the event source
/// exists only in what a successful start returns.
///
/// **The events suspend on READINESS, never on a timer.** Each wait is endo's
/// `TerminalEventSource::wait`: `::poll(2)` on POSIX, `WaitForMultipleObjects` on Windows. That
/// wait BLOCKS, so it runs on the pool and the result is handed back: the same two-hop
/// `TakeSample` uses, for the same reason. On the reactor it would stall ticks, samples and quit
/// together.

/// A terminal that has not been acquired yet. It has nothing to call; `StartTerminal` consumes it.
///
/// Holding one touches no terminal: no raw mode, no protocol change, no query. Destroying one that
/// was never started leaves the terminal exactly as it was.
class UnstartedTerminal final
{
  public:
    /// What the production and the fallback builds each put inside. Defined where it is built.
    struct Parts;

    /// @param parts What `StartTerminal` will acquire from.
    explicit UnstartedTerminal(std::unique_ptr<Parts> parts) noexcept;
    ~UnstartedTerminal();

    UnstartedTerminal(UnstartedTerminal const&) = delete;
    UnstartedTerminal(UnstartedTerminal&&) = delete;
    UnstartedTerminal& operator=(UnstartedTerminal const&) = delete;
    UnstartedTerminal& operator=(UnstartedTerminal&&) = delete;

  private:
    friend struct UnstartedTerminalAccess;
    std::unique_ptr<Parts> _parts;
};

/// A terminal that was acquired: what it can draw, and the events it produces.
///
/// The record arrives beside the source so the rung can be decided (`ChooseRenderRung`) before
/// the first event is read. `events` owns the terminal: it is restored when `events` is destroyed,
/// which must follow `Close()` and the outstanding `Next()` resuming. The first event is always a
/// `Resize` carrying the geometry at start.
struct StartedTerminal
{
    TerminalCapabilities capabilities;
    std::unique_ptr<IDashboardEventSource> events;
};

/// A terminal event source over this process's standard streams, not yet started.
///
/// Touches no terminal, so a composition may call it and never start the result.
///
/// Pointers rather than references, matching `TakeSample`: siblings composed the same way, and
/// every coroutine parameter in this tree is a pointer. None may be null, and both must outlive the
/// result and everything started from it.
///
/// Give `pool` a thread of its own. The readiness wait parks there for as long as the operator
/// types nothing, so a sampler sharing a one-thread pool would never run.
///
/// Fails only for a reason unrelated to whether there is a terminal: the OS refusing a wakeup
/// handle, or a build without the vendored TUI. Whether there IS a terminal is `StartTerminal`'s
/// question.
/// @param pool Where acquiring the terminal and each blocking wait run.
/// @param resumeOn Where `StartTerminal` and `Next()` resume before they return.
/// @return The unstarted terminal, or why none could be made.
[[nodiscard]] std::expected<std::unique_ptr<UnstartedTerminal>, std::string> MakeTerminalEvents(IExecutor* pool,
                                                                                                IExecutor* resumeOn);

/// Acquire @p terminal and learn what it can draw.
///
/// Hops to the pool; refuses unless stdin and stdout are both a terminal; enters raw mode; asks
/// for the device attributes (DA1, for Sixel); reads the text encoding behind `Platform/Terminal`;
/// then hops back to `resumeOn` before returning. Input typed while the queries were in flight is
/// kept and arrives from `events`.
///
/// **A terminal that never answers DA1 is a successful start** whose record says
/// `SixelAnswer::NoReply`, so the rung falls back rather than the dashboard failing. Only failing
/// to acquire the terminal at all is an error, and then the terminal is restored before this
/// returns, however far the acquisition got.
/// @param terminal The terminal to start; consumed.
/// @return The started terminal, or why it could not be acquired.
[[nodiscard]] Task<std::expected<StartedTerminal, std::string>> StartTerminal(std::unique_ptr<UnstartedTerminal> terminal);

} // namespace FastCache::Cli
