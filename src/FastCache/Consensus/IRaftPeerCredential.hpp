// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <cstdint>

namespace FastCache::Consensus
{

/// Which MAC a Raft peer connection is asking for.
///
/// **Private: never transmitted, never persisted.** Its ordinals mean nothing outside
/// this build -- each value is mapped to a signing domain by the credential, and it is
/// the domain's LABEL that goes into a MAC. So it carries no explicit values, and an
/// insertion shifts nothing that anything outside this process has seen.
enum class RaftPeerMac : std::uint8_t
{
    /// The dialler's proof over the acceptor's challenge.
    DiallerProof,

    /// The acceptor's signed verdict on that proof.
    AcceptorVerdict,

    /// One session frame, bound to its connection's nonces and its position.
    Frame,

    Last, ///< Not a MAC, and has no row: the length of a table keyed by one.
};

/// What a Raft peer connection signs and checks its MACs with.
///
/// ## Why a seam, rather than `Cluster::SignFields` called directly
///
/// The construction lives in `Cluster/ClusterSigning.hpp`, and `Cluster/` already
/// includes `Consensus/` -- `ClusterState` carries Raft types and `ClusterStateMachine`
/// is an `IRaftStateMachine`. A consensus header reaching back into it would make the
/// two directories include each other. So consensus states WHAT it needs signed, and
/// `Cluster::PskRaftPeerCredential` answers with the one construction the cluster key
/// has (#402, #1308).
///
/// It is also the seam #178 will need. A pre-shared key proves "holds the cluster
/// key", not WHICH holder; every field a Raft handshake MACs already names the ids it
/// is about, so a credential keyed per node can replace this one without the wire
/// moving.
///
/// ## What an implementation owes
///
/// - `Verify` compares in constant time. A caller never compares a tag itself -- there
///   is no tag-returning check to compare against -- which is `VerifyFields`' rule
///   carried across the seam.
/// - Each `RaftPeerMac` maps to a DIFFERENT signing domain, so a tag minted for one
///   purpose never verifies as another.
/// - Both calls are safe from any thread at once: the transport's senders and the
///   server's readers share one credential.
class IRaftPeerCredential
{
  public:
    IRaftPeerCredential() = default;
    IRaftPeerCredential(IRaftPeerCredential const&) = delete;
    IRaftPeerCredential(IRaftPeerCredential&&) = delete;
    IRaftPeerCredential& operator=(IRaftPeerCredential const&) = delete;
    IRaftPeerCredential& operator=(IRaftPeerCredential&&) = delete;
    virtual ~IRaftPeerCredential() = default;

    /// The tag a holder of this credential produces for @p fields.
    /// @param purpose Which MAC this is.
    /// @param fields The message's fields, in wire order.
    /// @return The tag.
    [[nodiscard]] virtual Sha256::Digest Sign(RaftPeerMac purpose, WireFields::FieldList fields) const = 0;

    /// Whether @p presented is the tag @p fields carry for @p purpose.
    /// @param purpose Which MAC this is.
    /// @param fields The message's fields, in wire order.
    /// @param presented The tag the peer sent.
    /// @return True when it authenticates.
    [[nodiscard]] virtual bool Verify(RaftPeerMac purpose,
                                      WireFields::FieldList fields,
                                      Sha256::Digest const& presented) const = 0;
};

} // namespace FastCache::Consensus
