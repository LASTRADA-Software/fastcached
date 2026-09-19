// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>

#include <optional>
#include <span>
#include <vector>

namespace FastCache::Consensus
{

/// What this node's roster says about the keys one peer id may prove itself with (#178).
struct PeerKeys
{
    /// The key the id proves itself with NOW, or disengaged when the roster records none.
    std::optional<Ed25519PublicKey> live;

    /// Every key the roster has revoked, whatever id it was revoked under. Never accepted: a
    /// signature that verifies under one is REPORTED as a revoked key rather than as an unknown
    /// one or a forgery, which is what an operator reading the refusal needs to know. The list
    /// is not narrowed to the claimed id. The question is WHO SIGNED, which only the signature
    /// answers; a claim or a revocation's label cannot.
    std::vector<Ed25519PublicKey> revoked;
};

/// The key material a Raft peer connection is proved with (#178).
///
/// **Two halves, and only one of them is secret.** This node's own private key signs and never
/// leaves the implementation -- there is no accessor for it, only `SignAsSelf` -- while every
/// other member's PUBLIC key comes from the roster, which is replicated state. So a machine
/// holding every byte it ever received still cannot sign as anybody but itself, and revoking one
/// machine is one roster entry rather than a secret rotated on every other (#178's
/// requirements (b) and (c)).
///
/// A seam rather than a key pair and a map passed in, because the roster MOVES while the node
/// runs: an applied forget has to reach every connection its revoked key proved, and a handshake
/// judged a moment later has to read the roster as it is then. Consensus cannot read
/// `Cluster::ClusterState` itself -- `Cluster/` includes `Consensus/`, not the reverse -- so it
/// states what it needs, and the node answers from the replicated state.
///
/// ## What an implementation owes
///
/// - Every call is safe from any thread at once: the transport's senders, the server's readers
///   and the state machine's apply thread all reach one instance.
/// - `KeysOf` answers from the roster as it is at the moment of the call, never a snapshot taken
///   at construction, or a revocation reaches no session that was open before it.
class IRaftPeerKeys
{
  public:
    IRaftPeerKeys() = default;
    IRaftPeerKeys(IRaftPeerKeys const&) = delete;
    IRaftPeerKeys(IRaftPeerKeys&&) = delete;
    IRaftPeerKeys& operator=(IRaftPeerKeys const&) = delete;
    IRaftPeerKeys& operator=(IRaftPeerKeys&&) = delete;
    virtual ~IRaftPeerKeys() = default;

    /// This node's own public key: what a peer claiming this node's id must verify under,
    /// which is how a copied state directory is told apart from a stranger borrowing a name.
    /// @return The key.
    [[nodiscard]] virtual Ed25519PublicKey OwnPublicKey() const = 0;

    /// Sign @p message with this node's own private key.
    /// @param message What is signed.
    /// @return The signature.
    [[nodiscard]] virtual Ed25519Signature SignAsSelf(std::span<std::byte const> message) const = 0;

    /// What the roster says about @p peer's keys, now.
    /// @param peer A member id, as a handshake claimed it.
    /// @return Its live key, if any, and every revoked key.
    [[nodiscard]] virtual PeerKeys KeysOf(NodeId const& peer) const = 0;
};

} // namespace FastCache::Consensus
