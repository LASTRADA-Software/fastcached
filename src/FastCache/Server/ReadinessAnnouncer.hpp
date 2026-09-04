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
/// ## The expected count is REGISTERED, never computed
///
/// `ExpectAcceptor()` is called by the code that creates each participant, in the
/// same loop that creates it, and `AcceptorsAllSpawned()` closes the set. There is
/// deliberately no count parameter, because the two failure directions are not
/// symmetric and the arithmetic can only get the dangerous one wrong:
///
///   - A total too **low** announces early. That is #646 again, mild, and visible.
///   - A total too **high** means the announcement **never fires at all**. The line
///     never appears, and two out-of-tree waiters plus a CI step block on it -- a
///     hang, behind a line a green suite says nothing about.
///
/// A number derived by hand from two other numbers (`binds.size() + reactorCount`,
/// say) can disagree with what actually started; a count incremented where the
/// participant is created cannot. So the type offers no way to state a total.
///
/// ## And it cannot end silently
///
/// If this object is destroyed having never announced, it says so, naming how many
/// of how many armed. A readiness contract that can quietly never fire is worse than
/// one that fires early, because early is at least observable -- so the one state
/// with no log line of its own is the one state this must not be able to reach.
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
    ///        Must outlive this object, which is why every call site declares it
    ///        after the logger and before the threads that arm it.
    /// @param endpointSummary What the readiness line says it is ready on, e.g.
    ///        `2 bind(s) x 4 reactors`. Copied.
    ReadinessAnnouncer(ILogger& logger, std::string endpointSummary) noexcept;

    ReadinessAnnouncer(ReadinessAnnouncer const&) = delete;
    ReadinessAnnouncer(ReadinessAnnouncer&&) = delete;
    ReadinessAnnouncer& operator=(ReadinessAnnouncer const&) = delete;
    ReadinessAnnouncer& operator=(ReadinessAnnouncer&&) = delete;

    /// Report readiness as never reached, when that is how this object ends.
    ///
    /// The whole point of the type is that a waiter may rely on the line, so the
    /// case where it never came has to be explained rather than left as an absence
    /// somebody has to infer. Silent on the ordinary path, because announcing and
    /// then shutting down cleanly is not an event.
    ~ReadinessAnnouncer();

    /// Register one participant that will arm, called where it is created.
    ///
    /// One call per `ExpectAcceptor()` per `AcceptorArmed()`, and both live next to
    /// the construct they describe -- that correspondence is the guard, and it is
    /// why no caller is handed a total to compute. Has no effect once
    /// `AcceptorsAllSpawned()` has been called, which is reported rather than
    /// ignored.
    void ExpectAcceptor();

    /// Close the set of participants, announcing if they have all already armed.
    ///
    /// Called once, by the loop that did the spawning, after its last
    /// `ExpectAcceptor()`. Until it is called nothing can announce, so a participant
    /// that arms while others are still being created cannot announce on their
    /// behalf.
    void AcceptorsAllSpawned();

    /// Record one participant as armed, announcing when it is the last.
    ///
    /// Safe from any thread: exactly one caller observes the transition, so the
    /// readiness line is emitted once however many threads arm concurrently.
    /// @param what Names the participant for the Debug line, e.g. `reactor 0 bind 1`.
    void AcceptorArmed(std::string_view what);

    /// @return True once the readiness line has been emitted.
    [[nodiscard]] bool Announced() const noexcept;

    /// @return How many participants have reported themselves armed.
    [[nodiscard]] std::size_t ArmedCount() const noexcept;

    /// @return How many participants were registered by `ExpectAcceptor()`.
    [[nodiscard]] std::size_t ExpectedCount() const noexcept;

  private:
    /// Emit the readiness line if the set is closed and every participant armed.
    /// Idempotent; exactly one caller ever wins.
    void MaybeAnnounce();

    ILogger& _logger;
    std::string _endpointSummary;
    std::atomic<std::size_t> _expected { 0 };
    std::atomic<std::size_t> _armed { 0 };
    std::atomic<bool> _sealed { false };
    std::atomic_flag _announced = ATOMIC_FLAG_INIT;
};

} // namespace FastCache
