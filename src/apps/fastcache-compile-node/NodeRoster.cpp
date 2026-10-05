// SPDX-License-Identifier: Apache-2.0
#include "NodeRoster.hpp"

// Its own header FIRST and in a group of its own, for `WorkerLease.cpp`'s reason.
#include "NodeMembership.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace FastCache::Node
{

std::expected<std::unique_ptr<NodeRoster>, NodeRefusal> NodeRoster::Build(NodeConfig const& cfg,
                                                                          core::platform::IClock const& clock,
                                                                          NodeConditions* conditions)
{
    // A consensus member's roster is the state it applies, and every serving node is one.
    if (RunsConsensus(cfg))
        return std::unique_ptr<NodeRoster> { new NodeRoster { std::make_unique<Distributed::StateLeaseRoster>(clock),
                                                              conditions } };

    // Nothing to verify a grant against. Legal only where no other machine can present one -- and
    // the startup table refuses a worker that runs no consensus before any tier is built, so this
    // is the belt behind it.
    if (RunsWorker(cfg) && CompileVerbsReachOtherMachines(cfg, RosterPresence::Absent))
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, std::string { RosterlessWorkerRefusal }) };
    return std::unique_ptr<NodeRoster> { new NodeRoster { nullptr, conditions } };
}

NodeRoster::NodeRoster(std::unique_ptr<Distributed::StateLeaseRoster> state, NodeConditions* conditions):
    _state { std::move(state) },
    _conditions { conditions }
{
    // Answered from the START, which counts as contact: a row nobody evaluated until the first
    // consensus pass would read `undecided` on every fresh node. A node verifying no grant leaves
    // the row to its scope -- it runs no consensus, so `Settle` answers it.
    if (_state != nullptr && _conditions != nullptr)
        _conditions->Clear(NodeCondition::ConsensusLeaderSilent);
}

Distributed::ILeaseRoster const* NodeRoster::Lease() const noexcept
{
    return _state.get();
}

ServerStanding NodeRoster::StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const
{
    if (_state == nullptr)
        return ServerStanding::Unchecked;

    // A revocation first, whatever id it was revoked under: the key is the fact, and a removed
    // machine claiming a voter's id is still the removed machine.
    auto const keys = _state->KeysOf(serverId);
    if (std::ranges::contains(keys.revoked, serverKey))
        return ServerStanding::Revoked;
    if (keys.live == serverKey)
        return ServerStanding::Voter;
    // A member whose applied state names no voter yet has nothing to place a server against, so it
    // answers `Unchecked` rather than calling every server a stranger; the revocation above is still
    // asked. That is the one state left here: a voter record always carries its key, since
    // `ClusterMember::publicKey` is required by type, so a keyless voter cannot be built.
    if (!_state->HoldsVoterKeys())
        return ServerStanding::Unchecked;
    return ServerStanding::NotVoter;
}

std::string_view NodeRoster::Expected() const
{
    return "a voter";
}

void NodeRoster::Applied(Cluster::ClusterState const& state)
{
    if (_state != nullptr)
        _state->Adopt(state);
}

void NodeRoster::ConsensusPass(Distributed::LeaderReading const& reading)
{
    if (_state == nullptr)
        return;
    _state->NoteLeaderReading(reading);
    if (_conditions == nullptr)
        return;
    // Live, and from the same reading every grant is checked by, so the row and the refusals it
    // explains cannot disagree.
    if (_state->Isolated())
        _conditions->Raise(NodeCondition::ConsensusLeaderSilent,
                           std::format("no leader this node's fleet counts has spoken to it for over {} minutes; every "
                                       "lease grant is refused until one does",
                                       Distributed::LeaderSilenceBound.count()));
    else
        _conditions->Clear(NodeCondition::ConsensusLeaderSilent);
}

std::optional<Distributed::RosterSummary> NodeRoster::Summary() const
{
    if (_state == nullptr)
        return std::nullopt;
    return _state->Summary();
}

std::optional<Cluster::Roster> NodeRoster::HeldRoster() const
{
    if (_state == nullptr)
        return std::nullopt;
    return _state->Held();
}

} // namespace FastCache::Node
