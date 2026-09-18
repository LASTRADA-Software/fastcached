// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterStateMachine.hpp>

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// A decoder's refusal, restated as what it says about bytes this node HOLDS (#1542).
    ///
    /// The decoders speak the peer wire's vocabulary, in three codes: another build's
    /// encoding is `UnsupportedVersion`, a verb this build lacks is `UnknownMessageType`,
    /// and bytes that are not a state or a command at all are `MalformedFrame`. The first
    /// two are INTACT bytes a different build laid out, which the storage rule calls
    /// `UnsupportedFormatVersion` -- never the damage code, because damage is what gets a
    /// healthy state deleted rather than an upgrade finished. Only the third is damage.
    /// @param decoded What the decoder said; its context is kept.
    /// @return The same refusal, in the two codes `IRaftStateMachine` refuses with.
    [[nodiscard]] ConsensusError AsHeldStateRefusal(ConsensusError decoded)
    {
        decoded.code = decoded.code == ConsensusErrorCode::MalformedFrame ? ConsensusErrorCode::StorageFailure
                                                                          : ConsensusErrorCode::UnsupportedFormatVersion;
        return decoded;
    }
} // namespace

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
        // proposer. Not from this node's OWN log: a command it held when it started was
        // asked `CanRead` first, and one it could not read kept it from starting (#1542).
        _logger.Logf(LogLevel::Error,
                     "cluster: entry {} carries a command this build cannot decode ({}); skipping it",
                     entry.index.value,
                     command.error().context);
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

std::expected<void, ConsensusError> ClusterStateMachine::CanRead(std::span<std::byte const> command) const
{
    return DecodeCommand(command).transform([](Command const&) {}).transform_error(AsHeldStateRefusal);
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

std::expected<void, ConsensusError> ClusterStateMachine::RestoreSnapshot(std::span<std::byte const> state,
                                                                         Consensus::SnapshotOrigin origin)
{
    auto restored = DecodeState(state);
    if (!restored.has_value())
    {
        // Left alone rather than cleared. Replacing what this node holds with nothing
        // would turn "I cannot read your state" into "the cluster has no members" --
        // after which this node would refuse every peer it had been serving a moment
        // earlier. At install that is the whole answer; at recovery there is nothing
        // held yet, and the refusal is what stops the node starting on a state that
        // lost its members and its tombstones (#1542).
        //
        // The reason is named, because the two causes send an operator to different
        // places: another build's encoding (both versions stated) is an upgrade still
        // in progress, and bytes that are not a state at all are damage. And WHICH
        // snapshot is named, because the two origins lead to different places too.
        auto const& traits = Consensus::TraitsOf(origin);
        _logger.Logf(LogLevel::Error,
                     "cluster: this build cannot decode {} ({}); {}",
                     traits.described,
                     restored.error().context,
                     traits.consequence);
        return std::unexpected { AsHeldStateRefusal(std::move(restored).error()) };
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
    return {};
}

void ClusterStateMachine::Publish(ClusterState const& state) const
{
    if (_observer)
        _observer(state);
}

} // namespace FastCache::Cluster
