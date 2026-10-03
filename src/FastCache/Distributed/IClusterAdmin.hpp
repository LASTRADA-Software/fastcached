// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <expected>
#include <string>

namespace FastCache::Distributed
{

/// What a scheduler needs in order to serve the cluster-administration verbs.
///
/// A seam rather than a call into `Node::ConsensusTier`, and for the ordinary
/// reason: the scheduler is a library type that has to be testable without a Raft
/// cluster, two threads and a durable log behind it. The whole verb surface is then
/// a fake that records what it was asked to propose.
///
/// It is deliberately **thin**. Everything a change can be refused for is already
/// decided elsewhere -- `Cluster::Validate` refuses a command nobody could apply,
/// and the consensus layer refuses one this node may not make -- so this adds no
/// policy of its own. What it adds is the ability to ask, from a class that must
/// not know how consensus is arranged.
class IClusterAdmin
{
  public:
    IClusterAdmin() = default;
    IClusterAdmin(IClusterAdmin const&) = delete;
    IClusterAdmin(IClusterAdmin&&) = delete;
    IClusterAdmin& operator=(IClusterAdmin const&) = delete;
    IClusterAdmin& operator=(IClusterAdmin&&) = delete;
    virtual ~IClusterAdmin() = default;

    /// What the cluster has agreed, as this node last applied it.
    ///
    /// By value, because the applying thread is not the one asking: see
    /// `Cluster::ClusterStateMachine::State`.
    /// @return The state.
    [[nodiscard]] virtual Cluster::ClusterState ClusterState() const = 0;

    /// Offer a change to the cluster.
    ///
    /// Refused when this node does not lead and when the command is one nothing
    /// could apply. Success means the entry was **appended**, not that it has
    /// committed -- which is the honest thing to report, since a leader cannot know
    /// the difference until a majority answers, and an operator who wants to see the
    /// result asks for the state again.
    /// @param command The change.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] virtual std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) = 0;

    /// A member PROVED its identity on its connection and announced where its `0xFC` port answers.
    ///
    /// Called by the leader's scheduler only, and only under the id the caller PROVED -- never one a
    /// payload names, so a member can only ever speak for itself. An implementation RECONCILES: it
    /// keeps the latest announcement per member and proposes on its own pass
    /// (`Cluster::AnnouncedEndpointDesires`), never inline, so an announcement costs a log entry only
    /// when the record differs and no change for that member is already in flight.
    /// @param member The id the caller proved.
    /// @param endpoint Where it says its `0xFC` port answers.
    virtual void NoteAnnouncedEndpoint(Consensus::NodeId const& member, std::string endpoint) = 0;
};

} // namespace FastCache::Distributed
