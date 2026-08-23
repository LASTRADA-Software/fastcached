// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/IRaftStateMachine.hpp>
#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace FastCache::Cluster
{

/// The cluster's configuration, as a replicated state machine.
///
/// What the Raft log carries, and the whole of it: who is a member and where they
/// answer, plus the handful of settings every member must agree on. Deliberately
/// **not** the cache — a log is replicated to every member and kept until it is
/// snapshotted, so what goes in it must change rarely and matter everywhere, and
/// multi-megabyte objects written constantly are the opposite of both.
///
/// ## Idempotence across restarts
///
/// `IRaftStateMachine` documents that a recovered node re-applies from the start of
/// whatever log it holds, because the commit index is not durable. This machine is
/// naturally idempotent — every command is a set-or-replace, and re-applying a
/// prefix reaches the same state — but the *observer* below is not something a
/// caller may assume is called once per change. It is called whenever the state
/// moves, including during that replay, so it must be cheap and must not be a place
/// where side effects accumulate.
class ClusterStateMachine final: public Consensus::IRaftStateMachine
{
  public:
    /// Told the new state whenever it changes.
    ///
    /// A callback rather than a polled getter because the consumers -- the fleet's
    /// membership oracle and the scheduler -- need to be *corrected* the moment a
    /// change commits. Polling would leave a window in which this node had agreed to
    /// admit a peer and was still refusing it, which from the peer's side is
    /// indistinguishable from being refused outright.
    using Observer = std::function<void(ClusterState const&)>;

    /// @param logger Where a malformed entry is reported; must outlive this.
    /// @param observer Told the state after every change; may be empty.
    ClusterStateMachine(ILogger& logger, Observer observer);

    void Apply(Consensus::AppliedEntry const& entry) override;

    [[nodiscard]] std::vector<std::byte> TakeSnapshot() override;

    void RestoreSnapshot(std::span<std::byte const> state) override;

    /// The state as of the last applied entry.
    [[nodiscard]] ClusterState const& State() const noexcept
    {
        return _state;
    }

  private:
    /// Tell the observer, if there is one.
    void Publish() const;

    ILogger& _logger;
    Observer _observer;
    ClusterState _state;
};

} // namespace FastCache::Cluster
