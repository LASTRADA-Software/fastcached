// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace FastCache
{

/// Emits the daemon's readiness line once every acceptor is actually armed.
///
/// ## Why this is a type rather than one `Logf` call
///
/// `fastcached` logged `ready, accepting connections` **before** starting a single
/// accept loop ([#646](https://github.com/LASTRADA-Software/fastcached/issues/646)).
/// Nothing observably broke -- the listen backlog covers the window -- and that is
/// exactly what made it worth fixing: the line reads like a readiness marker, so a
/// fixture author looking for one finds it, waits on it, and has been given a fact
/// **weaker than the bind they already had**. The failure is asymmetric in the
/// dangerous direction. Over-waiting costs seconds; under-waiting produces a test
/// that has never once exercised its property and is green throughout.
///
/// This repository has already paid for the same shape on the other binary: #634's
/// `compile node ready` changed from meaning *surveyed* to meaning *bound*, same
/// wording, different fact, and three include-tree walks silently stopped being
/// serialised.
///
/// So the line is emitted by the thing that knows every acceptor armed, and there is
/// no `Logf` at a call site for somebody to move back above the acceptors.
///
/// ## Why it counts rather than being called once at the end
///
/// Only one of the three server loops has a place where "the acceptors have started"
/// is a statement one thread can make. On POSIX with `--threads N` each reactor arms
/// its own accept loops on its own thread and then blocks in `Run()`, and on Windows
/// each bind gets an acceptor thread of its own. Whoever arms **last** is the only
/// caller that can announce, and which thread that is changes from run to run -- so
/// the count decides, and the announcement happens on whichever thread brings it home.
///
/// ## The wording is a contract with things outside this repository
///
/// `bench/runner.py` (`READY_MARKER`) and the packaging job in
/// `.github/workflows/build.yml` both wait on the literal substring
/// `ready, accepting connections`. This change makes that substring name a *stronger*
/// fact and deliberately does not reword it, so neither waiter needs to change and
/// neither silently starts waiting on something else.
class ReadinessAnnouncer
{
  public:
    /// Construct over the logger the line is emitted to.
    /// @param logger Sink for the per-acceptor Debug lines and the readiness line.
    /// @param acceptorCount How many acceptors must arm before the daemon is ready.
    ///        Zero means nothing will ever arm, so nothing is ever announced -- the
    ///        truthful answer for a server with no listener, which `RunReactorServer`
    ///        refuses one step earlier.
    /// @param endpointSummary What the readiness line says it is ready on, e.g.
    ///        `2 bind(s) x 4 reactors`. Copied; the announcer outlives no caller's
    ///        temporary.
    ReadinessAnnouncer(ILogger& logger, std::size_t acceptorCount, std::string endpointSummary) noexcept;

    /// Record one acceptor as armed, announcing readiness when it is the last.
    ///
    /// Safe from any thread: the count is atomic and exactly one caller observes the
    /// transition to `acceptorCount`, so the readiness line is emitted once however
    /// many threads arm concurrently.
    /// @param what Names the acceptor for the Debug line, e.g. `127.0.0.1:6674`.
    void AcceptorArmed(std::string_view what);

    /// @return True once the readiness line has been emitted.
    [[nodiscard]] bool Announced() const noexcept;

    /// @return How many acceptors have reported themselves armed.
    [[nodiscard]] std::size_t ArmedCount() const noexcept;

    /// @return How many must arm before readiness is announced.
    [[nodiscard]] std::size_t AcceptorCount() const noexcept
    {
        return _acceptorCount;
    }

  private:
    ILogger& _logger;
    std::size_t _acceptorCount;
    std::string _endpointSummary;
    std::atomic<std::size_t> _armed { 0 };
};

} // namespace FastCache
