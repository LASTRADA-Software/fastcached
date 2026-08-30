// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

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

} // namespace FastCache::Node
