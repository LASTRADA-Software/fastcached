// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Protocol/SealedFrameSocket.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace FastCache::Cc
{
struct Credential;
class CredentialNotice;
} // namespace FastCache::Cc

namespace FastCache::Node
{

/// @file NodeProofClient.hpp
/// The proving half of the node handshake: what a machine does on every connection it opens to a
/// scheduler, before the first verb it came to send (#178).
///
/// ## Why before anything else
///
/// The verbs a joining machine sends -- `Register`, `NodeAnnounce`, `Heartbeat`, `Withdraw` --
/// require a proven identity (`CompileCacheWire::IdentityRequirement`), and what a proof
/// establishes is connection state. So a round that dials proves once, and every verb on that
/// connection is sent sealed, as the machine the cluster admitted.
///
/// ## It checks the SERVER too
///
/// The server signs first. A machine that holds a roster -- a consensus member, or a worker that
/// has adopted a certified one -- refuses to prove itself to a server whose key is not a live
/// voter's: a revoked ex-scheduler still named in its `--scheduler` gets nothing, not even a proof
/// it could relay. A machine with no roster yet takes the server's word for its key, and the seal
/// is what still stops a relay from injecting verbs on the connection the proof admitted.

/// What a caller knows about the server that signed a handshake.
///
/// **PRIVATE: persisted and transmitted nowhere.** Four answers, because the two refusals are
/// opposite diagnoses and the two acceptances are not equally strong.
enum class ServerStanding : std::uint8_t
{
    Voter,     ///< Its key is a live voter's in the roster this machine holds.
    Unchecked, ///< This machine holds no roster yet, so the key could be checked against nothing.
    NotVoter,  ///< The roster holds no such voter key: a learner, a stranger, or a stale address.
    Revoked,   ///< The roster REVOKED this key: the machine the cluster forgot.
};

/// Whom this machine may prove itself to (#178).
///
/// A seam, because the answer is the roster this node holds -- the applied state on a consensus
/// member, a certified roster on a worker, nothing yet on a fresh one -- and that is read per
/// handshake, so a revocation reaches the next round rather than the next restart.
class IServerTrust
{
  public:
    IServerTrust() = default;
    IServerTrust(IServerTrust const&) = delete;
    IServerTrust(IServerTrust&&) = delete;
    IServerTrust& operator=(IServerTrust const&) = delete;
    IServerTrust& operator=(IServerTrust&&) = delete;
    virtual ~IServerTrust() = default;

    /// @param serverId The id the server claimed.
    /// @param serverKey The key its signature verified under.
    /// @return What this machine's roster says about it.
    [[nodiscard]] virtual ServerStanding StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const = 0;
};

/// What one attempt to prove this machine's identity over one connection learned.
///
/// **Four outcomes, and the reason is the rulebook's**: the four call for four different actions,
/// and a `bool` would fold the one an operator acts on into the others.
enum class NodeProofResult : std::uint8_t
{
    /// The server accepted the proof; the connection is sealed both ways from here on.
    Proved,
    /// The server serves no proof: it runs no consensus, so it is not a scheduler of this fleet.
    NotOffered,
    /// This machine refused to prove itself to the server: its key is not one it may trust.
    Untrusted,
    /// The server refused the proof, or the exchange did not complete.
    Refused,
};

/// What the attempt learned, and what to say about it.
struct NodeProofAttempt
{
    NodeProofResult result { NodeProofResult::Refused }; ///< What happened.

    /// The server's own words when it refused, or what this machine concluded about the server.
    /// Empty only for `Proved`.
    std::string reason {};
};

/// This machine's identity, as it proves it on every connection to a scheduler.
///
/// Holds references: the key pair is read once at startup and lives for the process, and the
/// trust and the randomness are the node's own seams.
class NodeProofClient
{
  public:
    /// @param nodeId The id this machine minted into its `--cluster-dir`, as the cluster admitted it.
    /// @param key Its identity key pair; must outlive this.
    /// @param trust Whom it may prove itself to; must outlive this.
    /// @param random Where each handshake's nonce and ephemeral key come from; must outlive this.
    NodeProofClient(std::string nodeId, Ed25519KeyPair const& key, IServerTrust const& trust, ISecureRandom& random) noexcept
        :
        _nodeId { std::move(nodeId) },
        _key { key },
        _trust { trust },
        _random { random }
    {
    }

    /// Prove this machine's identity over @p peer, and seal it.
    ///
    /// On `Proved` the socket is sealed both ways and every later exchange over it is too. On any
    /// other outcome nothing verb-worthy may follow on this connection: a receiving seal may be
    /// engaged already, and a server that refused the proof refuses every joining verb anyway.
    /// @param peer A fresh connection to a scheduler, wrapped so it can be sealed.
    /// @param notice Where an unwanted credential is reported.
    /// @param credential What the connection presents where the server asks for one.
    /// @return What was learned.
    [[nodiscard]] NodeProofAttempt Prove(SealedFrameSocket& peer,
                                         Cc::CredentialNotice& notice,
                                         Cc::Credential const& credential) const;

    /// @return The id this machine proves.
    [[nodiscard]] std::string_view NodeId() const noexcept
    {
        return _nodeId;
    }

  private:
    std::string _nodeId;
    Ed25519KeyPair const& _key;
    IServerTrust const& _trust;
    ISecureRandom& _random;
};

} // namespace FastCache::Node
