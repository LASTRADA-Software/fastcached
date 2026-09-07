// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// How many `NotLeader` redirects one heartbeat round will follow.
///
/// Two, for the same reasons `Dispatch.cpp`'s `MaxLeaseRedirects` is two: one hop
/// covers the ordinary case of asking a follower, and the spare covers the leader
/// having moved again between the two, which an election in progress makes
/// ordinary rather than exotic.
///
/// Bounded at all because the chain is not this node's to trust. Two schedulers
/// that disagree about who leads -- a partition healing, a stale `_knownLeader` --
/// can name each other indefinitely, and a worker without a ceiling would spend a
/// heartbeat interval discovering that instead of announcing itself.
///
/// A separate constant from the launcher's rather than a shared one, deliberately:
/// a lease chain is spent inside one compile and answers a client that can always
/// fall back to compiling locally, while this chain is spent once per heartbeat
/// interval and decides whether a machine is in the fleet at all. They are the
/// same number today because the argument for two happens to hold on both
/// surfaces, not because one is derived from the other.
constexpr int MaxAnnounceRedirects = 2;

/// Where this node believes the scheduler's leader is, across heartbeat rounds.
///
/// **Pure**: no socket, no clock, no logger. The heartbeat thread dials whatever
/// `Target()` names and reports back what happened, so every decision this makes
/// is testable without a fleet. That matters because the alternative home for it
/// is `main.cpp`, which is in no test target -- the same reason
/// `MakeWorkerLeaseValidator` moved out of `main` rather than staying a lambda
/// there.
///
/// ## Why a node has to follow a redirect at all
///
/// `SchedulerService::Gate()` refuses **every** verb, `Register` included, when the
/// node it reached is not the leader. Before this existed, the heartbeat thread
/// dialled the configured `--scheduler` unconditionally and logged the refusal, so
/// after an election every worker went on announcing itself to the demoted node,
/// expired out of the new leader's registry inside the heartbeat timeout, and the
/// leader answered every lease `NoWorker`. A launcher that correctly followed the
/// redirect -- the client half of
/// [#237](https://github.com/LASTRADA-Software/fastcached/issues/237) -- then
/// arrived at a leader with an empty fleet and compiled locally, behind a green
/// build and counters that all read zero. This is the other half.
///
/// ## What is remembered, and when
///
/// A leader is committed only once a round has actually been **accepted** there,
/// never merely because some scheduler named it. A redirect this node followed to
/// an endpoint that then refused for its own reasons -- not a member, a
/// fingerprint it will not take -- must not become the endpoint every future round
/// starts at. So `Redirect` moves this round, and `Accepted` is what makes it
/// stick.
///
/// A remembered leader that stops answering is forgotten and the configured
/// `--scheduler` is tried again **in the same round**, rather than a heartbeat
/// interval later: the configured endpoint is the one an operator can actually
/// fix, and skipping a round to reach it doubles the window in which this machine
/// is missing from the fleet.
class SchedulerLink
{
  public:
    /// @param configured The `--scheduler` endpoint, which is never forgotten and
    ///        is what this falls back to.
    explicit SchedulerLink(std::string configured);

    /// Start a heartbeat round, resetting the per-round redirect budget.
    ///
    /// The budget is per round rather than per process: a fleet that re-elects
    /// once an hour should spend one redirect an hour, not exhaust a lifetime
    /// ceiling and then never follow one again.
    void BeginRound();

    /// Where this round's next dial should go.
    [[nodiscard]] std::string const& Target() const noexcept;

    /// Whether `Target()` is a remembered leader rather than the configured
    /// endpoint, so a diagnostic can say which it failed to reach.
    [[nodiscard]] bool Following() const noexcept;

    /// `Target()` refused `NotLeader` and named `leader`.
    /// @param leader The endpoint it named; already validated by `RedirectTarget`.
    /// @return True when the caller should dial `Target()` again, now pointing at
    ///         `leader`; false when this round's chain is spent, which the caller
    ///         answers by giving up until the next round rather than by looping.
    [[nodiscard]] bool Redirect(std::string leader);

    /// A round was accepted at `Target()`, committing it for future rounds.
    ///
    /// Committing the configured endpoint means *forgetting* any remembered
    /// leader, which is how a fleet that re-elects back to the original scheduler
    /// stops paying a redirect per heartbeat.
    void Accepted();

    /// `Target()` could not be reached, or refused for something that is not a
    /// redirect.
    /// @return The endpoint to try instead **right now** -- the configured one,
    ///         when a remembered leader was what just failed -- or nothing when
    ///         there is nothing left to fall back to this round.
    [[nodiscard]] std::optional<std::string> Lost();

  private:
    std::string _configured;
    /// The leader a round has been accepted at; empty until one has been.
    std::optional<std::string> _learned;
    /// Where this round is dialling right now.
    std::string _current;
    /// Redirects followed this round.
    int _hops = 0;
};

/// What one announcement round did, and how loudly to say it.
///
/// **A heartbeat and a registration are different events and one tally cannot carry
/// both** (#999). `AnnounceOnce` counted only `accepted`, incremented on both paths,
/// so a steady round of six heartbeats reported "6 of 6 toolchain(s) registered" every
/// interval forever -- which reads as a node re-announcing its whole toolchain set on a
/// timer. Measured on a live install: six registrations in the startup second, and every
/// round after that heartbeats. The line described work nobody was doing, and cost an
/// operator the question.
///
/// A pure function, for `RecheckDepthFor`'s reason: `main.cpp` is in no test target
/// (#909), so a rule left as an expression in the announce loop can be checked only by
/// reading it -- and the wording is the whole defect here, so it has to be assertable.
struct RoundReport
{
    LogLevel level;      ///< How loudly to say it.
    std::string message; ///< What to say, without the endpoint prefix.
};

/// Describe one announcement round.
///
/// Four outcomes, not two, and the third is the one worth catching: a heartbeat that
/// FELL THROUGH to a registration means the scheduler had forgotten this worker, which
/// is a real event wearing a steady round's clothes.
///
/// The all-heartbeat round is `Trace`, beside `Op::Heartbeat`'s own level (#992) and for
/// #993's reason: it is narration, it repeats identically forever, and after #992 moved
/// the heartbeat exchange to `Trace` this line became the only per-round output at
/// `Debug` -- so it inherited the exact property that argued the heartbeat down.
///
/// @param beats         Registrars that heartbeated successfully.
/// @param registrations Registrars that registered this round.
/// @param total         Registrars attempted.
/// @param leaderKnown   Whether a `NotLeader` named somewhere to go next.
/// @return The level and the sentence.
[[nodiscard]] inline RoundReport DescribeAnnounceRound(std::size_t beats,
                                                       std::size_t registrations,
                                                       std::size_t total,
                                                       bool leaderKnown)
{
    auto const accepted = beats + registrations;

    // **The wording of the ACCEPTED and SHORTFALL forms is a fixture contract** and is
    // deliberately unchanged. `E2eRegisteredMarker` in `scripts/lib/e2e-common.sh` is
    // "1 of 1 toolchain(s) registered", and its whole purpose is to tell an accepted
    // worker from one the scheduler turned away -- `0 of 1` matching the bare word
    // `registered` is #445, a wait that returned for a worker that never got in.
    // Rewording either form breaks that distinction in a fixture rather than in a
    // build, which is a timeout with no failed assertion. #999 is about the STEADY
    // round, which no fixture waits on, so only that gains a new sentence.
    if (accepted < total)
        return { .level = leaderKnown ? LogLevel::Debug : LogLevel::Warn,
                 .message = std::format("{} of {} toolchain(s) registered", accepted, total) };

    // A heartbeat that fell through to a registration: the scheduler had forgotten this
    // worker and it re-announced itself. Named rather than folded into either pure case,
    // because it is the one an operator would want to see in a fleet that keeps losing
    // workers -- and a round can be entirely successful and still be this. This form is
    // NEW, so it carries the breakdown the other two do not need.
    if (registrations > 0 && beats > 0)
        return { .level = LogLevel::Info,
                 .message = std::format("{} of {} toolchain(s) re-registered after a heartbeat was refused, {} still live",
                                        registrations,
                                        total,
                                        beats) };

    if (registrations > 0)
        return { .level = LogLevel::Info, .message = std::format("{} of {} toolchain(s) registered", registrations, total) };

    return { .level = LogLevel::Trace, .message = std::format("{} toolchain(s) still registered", beats) };
}

} // namespace FastCache::Node
