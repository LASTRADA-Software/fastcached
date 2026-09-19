// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>

#include <map>
#include <shared_mutex>
#include <span>
#include <vector>

namespace FastCache::Cluster
{

/// The keys a node's Raft peer wire is proved with: its own pair, and what the cluster says
/// every other member proves itself with (#178).
///
/// ## Two sources, and which one wins
///
/// - **The bootstrap members** -- `--raft-peer id=host:port@<key>` -- are what a node knows
///   before the cluster has told it anything. They are the only keys a cluster that has not
///   yet elected can verify, so a fresh cluster whose members were given none cannot form, and
///   says so in its refusal counters rather than trusting whoever answers first.
/// - **The replicated state** (`ClusterState::members` and `revokedKeys`) is what the cluster
///   agreed, and it WINS wherever it says anything: a member re-admitted under a new key is
///   proved with the new one whatever a command line typed a year ago.
///
/// A bootstrap key the state has REVOKED is revoked, whatever the command line says: a
/// revocation that a restart with the original command line could undo would be removal
/// failing open. A member the state records with no key -- admitted before it stated one --
/// falls back to its bootstrap key only when that key is not revoked.
///
/// ## A forgotten member keeps its key here until the configuration drops it
///
/// A forget revokes a member's key in the committed entry, and the configuration stops
/// counting the member a reconcile pass LATER (`Cluster::NextQuorumChange`, #1555). Cut off
/// at the revocation, the member would be a counted voter that can no longer vote for that
/// pass -- and a cluster losing one more voter inside it can WEDGE for good: four voters, one
/// forgotten and cut off, the leader lost, leaves two of four, which elects nobody, so nobody
/// ever proposes the removal that would have made two a majority. So a key revoked under an
/// id stays live FOR THAT ID while this node's configuration counts it (`AdoptConfiguration`),
/// and is refused the moment the configuration drops it -- which is Raft's own rule for a
/// removed server, stated about keys. Nothing else is graced: the same key under any other id
/// is refused, a forgotten member the configuration never counted is refused at once, and so
/// is every principal, which consensus never counts.
///
/// Principals are NOT read: a principal never joins consensus (`ClusterPrincipal`), so an id
/// that names one is a stranger on this wire.
///
/// ## Every revoked key, whatever id is asked about
///
/// `KeysOf` hands back EVERY key the state has revoked, not the ones revoked under the id asked
/// about. The id a revocation carries is whose the key WAS (`RevokedKey`), and a removed machine
/// may claim whatever id it likes. Filtering on that id would report n3 claiming another id as
/// a key nobody gave, when it is the removed machine itself. The list decides a diagnosis and
/// never an acceptance: a revoked key is refused whatever id it claims, because only the id's
/// live key proves it. The cost is one signature check per revoked key on a proof that did not
/// verify under the live key. That is bounded by the keys an operator has ever revoked, as
/// `revokedKeys` is, and never by traffic.
///
/// Thread-safe: the reactor's handshakes and every frame's re-check read it while the state
/// machine's apply thread writes it.
class RosterKeys final: public Consensus::IRaftPeerKeys
{
  public:
    /// @param own This node's identity key pair. Its secret half never leaves this object.
    /// @param bootstrap Every member this node's command line names, with the keys it typed
    ///        for them where it typed any. This node's own entry is ignored: its key is @p own.
    RosterKeys(Ed25519KeyPair own, std::span<ClusterMember const> bootstrap);

    /// Adopt what the cluster now says. Called on every applied change.
    /// @param state The replicated state.
    void Adopt(ClusterState const& state);

    /// Adopt which members this node's consensus configuration counts, either set (#1555).
    ///
    /// A member it counts keeps a key revoked under its own id until it is counted no more --
    /// see *A forgotten member keeps its key here until the configuration drops it*. Nothing
    /// adopted yet counts nobody, which is the direction that refuses.
    /// @param configuration The configuration consensus holds now.
    void AdoptConfiguration(Consensus::Configuration const& configuration);

    [[nodiscard]] Ed25519PublicKey OwnPublicKey() const override;

    [[nodiscard]] Ed25519Signature SignAsSelf(std::span<std::byte const> message) const override;

    [[nodiscard]] Consensus::PeerKeys KeysOf(Consensus::NodeId const& peer) const override;

  private:
    Ed25519KeyPair const _own;

    /// What the command line typed, by id. Never written after construction.
    std::map<Consensus::NodeId, Ed25519PublicKey> const _bootstrap;

    mutable std::shared_mutex _lock;

    /// The members the state records a key for, by id.
    std::map<Consensus::NodeId, Ed25519PublicKey> _stated;

    /// Every key the state has revoked.
    std::vector<RevokedKey> _revoked;

    /// Every member the configuration counts, voters and learners alike.
    std::vector<Consensus::NodeId> _counted;
};

} // namespace FastCache::Cluster
