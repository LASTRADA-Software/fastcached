// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterStateMachine.hpp>

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace FastCache::Cluster
{

ClusterStateMachine::ClusterStateMachine(ILogger& logger, Observer observer):
    _logger { logger },
    _observer { std::move(observer) }
{
}

void ClusterStateMachine::Apply(Consensus::AppliedEntry const& entry)
{
    auto const command = DecodeCommand(entry.payload);
    if (!command.has_value())
    {
        // Skipped, logged, and NOT fatal. An entry is applied only after it is
        // committed, so it is already in every future leader's log and there is
        // nothing left to refuse it to; stopping here would mean this node alone
        // stopped following a cluster the rest of which carried on, which is a
        // partition this node created for itself. The honest failure is to say so
        // and keep the ordering intact.
        //
        // It is reachable only from a peer running a build whose command format this
        // one does not know, which `Validate` cannot prevent because it runs on the
        // proposer.
        _logger.Logf(
            LogLevel::Error, "cluster: entry {} carries a command this build cannot decode; skipping it", entry.index.value);
        return;
    }

    auto published = ClusterState {};
    {
        auto const guard = std::unique_lock { _mutex };
        Cluster::Apply(_state, *command);
        published = _state;
    }
    Publish(published);
}

std::vector<std::byte> ClusterStateMachine::TakeSnapshot()
{
    auto const guard = std::shared_lock { _mutex };
    return Encode(_state);
}

ClusterState ClusterStateMachine::State() const
{
    auto const guard = std::shared_lock { _mutex };
    return _state;
}

void ClusterStateMachine::RestoreSnapshot(std::span<std::byte const> state)
{
    auto restored = DecodeState(state);
    if (!restored.has_value())
    {
        // Left alone rather than cleared. A snapshot that will not decode is a leader
        // running a format this build does not know, and replacing what this node
        // holds with nothing would turn "I cannot read your state" into "the cluster
        // has no members" -- after which this node would refuse every peer it had
        // been serving a moment earlier.
        _logger.Logf(LogLevel::Error, "cluster: a snapshot arrived that this build cannot decode; keeping current state");
        return;
    }

    // Replace, never merge: the snapshot is the complete state as of its index, and
    // folding it into what this machine already holds would keep members the cluster
    // has since removed -- which for a membership set means counting a node that is
    // gone towards quorum.
    auto published = ClusterState {};
    {
        auto const guard = std::unique_lock { _mutex };
        _state = *std::move(restored);
        published = _state;
    }
    Publish(published);
}

void ClusterStateMachine::Publish(ClusterState const& state) const
{
    if (_observer)
        _observer(state);
}

} // namespace FastCache::Cluster
