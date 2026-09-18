// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/IRaftStateMachine.hpp>
#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <shared_mutex>
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

    /// Whether @p command decodes as a command this build applies.
    ///
    /// `DecodeCommand`'s refusal, restated for bytes this node HOLDS (#1542): another
    /// build's encoding -- an older command version, or a verb this build lacks -- is
    /// `UnsupportedFormatVersion` with both versions stated, and only bytes that are not
    /// a command at all are `StorageFailure`.
    /// @param command An entry's payload.
    /// @return Nothing when `Apply` would act on it; otherwise why not.
    [[nodiscard]] std::expected<void, ConsensusError> CanRead(std::span<std::byte const> command) const override;

    /// Whether @p state decodes as a state this build holds, in `CanRead`'s two codes.
    /// @param state A snapshot's bytes.
    /// @return Nothing when `RestoreSnapshot` would replace the state with it; otherwise why not.
    [[nodiscard]] std::expected<void, ConsensusError> CanRestore(std::span<std::byte const> state) const override;

    [[nodiscard]] std::vector<std::byte> TakeSnapshot() override;

    /// Replace the state with the one @p state encodes, or refuse and keep it.
    ///
    /// A refusal is logged, naming which snapshot it was and what happens next from
    /// `Consensus::SnapshotOriginTable` -- a node's own recovered snapshot and a
    /// leader's installed one lead to different places, and "arrived" read wrong for
    /// the first. It is refused in `CanRead`'s two codes, for `CanRead`'s reason.
    /// @param state A snapshot `TakeSnapshot` produced, on some node.
    /// @param origin Which of the two callers this is.
    /// @return Nothing once replaced; otherwise why not, with nothing changed.
    [[nodiscard]] std::expected<void, ConsensusError> RestoreSnapshot(std::span<std::byte const> state,
                                                                      Consensus::SnapshotOrigin origin) override;

    /// The state as of the last applied entry.
    ///
    /// **By value, and that is not a copy nobody needed.** Entries are applied on
    /// whichever thread the driver advanced the node from, while this is read by
    /// whoever wants to know what the cluster currently says -- a different thread
    /// by construction, since the driver's own observer is called from inside
    /// `Deliver` and so is not somewhere a caller can do its own work. A reference
    /// would hand that caller a member being rewritten underneath it. The state is
    /// a member list and a handful of settings, so the copy costs nothing next to
    /// the replication round that produced it.
    [[nodiscard]] ClusterState State() const;

  private:
    /// Tell the observer, if there is one.
    ///
    /// Takes the state by value from the caller rather than reading `_state`, so it
    /// can be called with the lock released. An observer runs arbitrary code -- it
    /// republishes the fleet's membership oracle, which takes a lock of its own --
    /// and calling it under this one would order two unrelated locks.
    /// @param state What to report.
    void Publish(ClusterState const& state) const;

    ILogger& _logger;
    Observer _observer;

    /// Guards `_state` against the reader above.
    ///
    /// Shared rather than exclusive because reads outnumber writes by whatever the
    /// reconcile interval divided by the rate of cluster configuration changes is
    /// -- which for a healthy fleet is every read against no writes at all.
    mutable std::shared_mutex _mutex;
    ClusterState _state;
};

} // namespace FastCache::Cluster
