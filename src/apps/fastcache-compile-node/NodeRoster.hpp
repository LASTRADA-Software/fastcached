// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeProofClient.hpp"
#include "NodeRefusal.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/StateLeaseRoster.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>

/// @file NodeRoster.hpp
/// The roster a node verifies lease grants against (#178).
///
/// Every grant is signed by the voter that issued it, with its own identity key, so a worker
/// asks "is this signer one of the cluster's voters, unrevoked" -- and the answer is the state
/// the node's own consensus applied (`Distributed::StateLeaseRoster`), which needs no certificate
/// and never expires. Every serving node runs consensus, a learner included, so that is the only
/// roster there is; a node no other machine can reach runs none and checks no grant -- the rule
/// the startup table and `CompileVerbsReachOtherMachines` hold it to.

namespace FastCache::Node
{

/// The roster this node verifies grants against, as one object (#178).
class NodeRoster final: public IServerTrust
{
  public:
    /// Build the roster this configuration calls for.
    ///
    /// The applied state on a node that runs consensus; none on a node no other machine can reach;
    /// and a REFUSAL for a worker other machines can reach that runs no consensus, which is where
    /// `RosterlessWorkerRefusal` is answered: it could verify no grant. The startup table refuses
    /// that shape first (a worker that runs no consensus), so the refusal here is its belt.
    /// @param cfg The parsed configuration.
    /// @param clock The steady clock consensus stamps leader contact on; must outlive the roster.
    /// @param conditions Where `consensus-leader-silent` is answered; null when nobody reads it. Must
    ///        outlive the roster.
    /// @return The roster, or why the node must not start (`EarlierRule`: the configuration decides).
    [[nodiscard]] static std::expected<std::unique_ptr<NodeRoster>, NodeRefusal> Build(NodeConfig const& cfg,
                                                                                       core::platform::IClock const& clock,
                                                                                       NodeConditions* conditions);

    /// @return What a grant is verified against, or null when this node verifies none.
    [[nodiscard]] Distributed::ILeaseRoster const* Lease() const noexcept;

    /// @copydoc IServerTrust::StandingOf
    ///
    /// The roster a GRANT is verified against is the roster a server is (#178): a machine may
    /// prove itself only to a scheduler whose grants it would accept, so both questions read the
    /// one `Lease()`. A node verifying no grants checks nothing and says so, and the seal is what
    /// still protects the connection.
    [[nodiscard]] ServerStanding StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const override;

    /// @copydoc IServerTrust::Expected
    ///
    /// A scheduler is any voter, so the expectation is the kind of machine, never an id.
    [[nodiscard]] std::string_view Expected() const override;

    /// Adopt what the cluster now says. A no-op on a node that verifies no grant.
    /// @param state The replicated state.
    void Applied(Cluster::ClusterState const& state);

    /// Take one consensus pass's reading of who leads and when it last spoke, count it only for a
    /// leader the APPLIED configuration counts (`Distributed::StateLeaseRoster::NoteLeaderReading`),
    /// and answer `consensus-leader-silent` from the result. A no-op on a node that verifies no grant.
    /// @param reading The pass's raw reading.
    void ConsensusPass(Distributed::LeaderReading const& reading);

    /// @return The roster held, summarised for `--node-status`; nothing on a node that holds none.
    [[nodiscard]] std::optional<Distributed::RosterSummary> Summary() const;

    /// The roster held, which `explain-admission <machine>` answers from.
    /// @return The applied state, projected; nothing on a node that holds none.
    [[nodiscard]] std::optional<Cluster::Roster> HeldRoster() const;

    NodeRoster(NodeRoster const&) = delete;
    NodeRoster(NodeRoster&&) = delete;
    NodeRoster& operator=(NodeRoster const&) = delete;
    NodeRoster& operator=(NodeRoster&&) = delete;
    ~NodeRoster() override = default;

  private:
    /// @param state The applied state's roster, or null on a node that verifies no grant.
    /// @param conditions Where `consensus-leader-silent` is answered; may be null.
    NodeRoster(std::unique_ptr<Distributed::StateLeaseRoster> state, NodeConditions* conditions);

    /// The applied state's roster; null on a node that verifies no grant.
    std::unique_ptr<Distributed::StateLeaseRoster> _state;

    NodeConditions* _conditions; ///< Where the silence is answered; may be null.
};

} // namespace FastCache::Node
