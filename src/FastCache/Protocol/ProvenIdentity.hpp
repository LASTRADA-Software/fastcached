// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache
{

/// Which machine a connection PROVED it is (#178): the node id it claimed and the identity key it
/// signed the handshake with.
///
/// **Both, and the KEY is the fact.** The signature verified under this key, so the key is what the
/// connection holds; the id is what it claimed, and a roster says whether that key is the one the
/// cluster admitted under that id, one it REVOKED, or neither. Admission is re-asked of the roster
/// on every verb (`Distributed::IMembershipOracle::ExplainKey`) rather than frozen at the
/// handshake, so a key revoked while the connection is open refuses its very next verb.
///
/// Engaged on a connection only after its proof VERIFIED -- including under a revoked key, which is
/// what lets every later verb on that connection be refused as the forgotten machine's rather than
/// judged by its address. Owned: a gate reads it once per tick of a subscription, inside a
/// coroutine, for as long as the connection lives.
struct ProvenIdentity
{
    std::string id;       ///< The node id the connection claimed, inside the signature.
    Ed25519PublicKey key; ///< The key the signature verified under.

    [[nodiscard]] friend bool operator==(ProvenIdentity const&, ProvenIdentity const&) = default;
};

/// A key this node's roster has REVOKED, under which one connection presented a genuinely signed
/// machine ticket: evidence of exactly one fact -- the connection speaks for a machine the cluster
/// forgot (#1555).
///
/// **It can only ever answer `Forgotten`**, and that is what the type is for. Nothing converts it
/// back into a `ProvenIdentity`, so it cannot be asked as a LIVE key: the roster that verified the
/// ticket and the key roster admission folds are published at different moments, and a key the one
/// has revoked may still read live to the other -- which, asked through `ExplainKey`'s live arm,
/// would ADMIT the machine a forget was meant to refuse. A revocation is permanent (`KeyRevoked`),
/// so the fact this carries never goes stale.
///
/// Why a refused ticket is evidence at all: without it a forgotten machine's pipelined commands are
/// judged by their ADDRESS, and on a `--fleet-open` node an address admits anybody, so the forget
/// would stop biting exactly the honest launcher that shows its ticket every time.
///
/// **A ticket's evidence, and only a ticket's**: a proof under a revoked key is carried as the
/// proven identity itself. It refuses as firmly as a proof does and TELLS less -- a ticket is bytes
/// anybody may have captured, so the connection is answered as a stranger is
/// (`Distributed::DecisionOf` says it was shown by a ticket, and `RevocationIsProven` reads that).
class RevokedKeyEvidence
{
  public:
    /// @param identity The id the ticket named and the revoked key its signature verified under.
    explicit RevokedKeyEvidence(ProvenIdentity identity):
        _identity { std::move(identity) }
    {
    }

    /// @return The id the ticket named, for a log line or a report; never an admission.
    [[nodiscard]] std::string_view Id() const noexcept
    {
        return _identity.id;
    }

    [[nodiscard]] friend bool operator==(RevokedKeyEvidence const&, RevokedKeyEvidence const&) = default;

  private:
    ProvenIdentity _identity;
};

/// What one connection has established about who is at the other end: the three facts every
/// admission gate folds (`Distributed::ExplainConnection`), whichever surface holds them.
///
/// **One struct, where there were two with the same two fields** -- a node connection's
/// `PeerIdentity` and a live subscription's `LiveWatcher` -- so a third fact is added once and
/// reaches every gate, rather than arriving on one path and silently not on the other. Both names
/// survive as aliases, because each says where the facts came from at the call site.
///
/// **It OWNS every fact and is deliberately not named `*View`** (#366): a gate reads it inside a
/// coroutine, once per verb and once per tick of a subscription, long after the frame that carried
/// it is gone -- and a wrong view here is an admission decided from freed memory.
struct ConnectionFacts
{
    /// The kernel's peer HOST, as `getpeername` reported it. Never a name the peer chose, and never
    /// an endpoint: a peer dials from an ephemeral source port. It can legitimately be empty -- an
    /// unnameable peer -- so a reader decides what that means rather than assuming a host.
    std::string host {};

    /// Engaged only by a node proof this connection VERIFIED (`ProveNode`, sealed): the id it
    /// claimed and the key its signature verified under. Engaged for a revoked key too, so every
    /// later verb is refused as the forgotten machine's whatever the address says.
    std::optional<ProvenIdentity> proven {};

    /// Engaged only by a machine ticket this connection's AUTH VERIFIED -- never by an AUTH that
    /// merely answered `Ok`: the machine the ticket names and the key that signed it. A ticket
    /// admits a CALLER and never stands in for a proof (`CallerContext::provenNodeId`), and like a
    /// proof it is re-asked of the roster on every verb, so a key revoked while it is open refuses
    /// the next one.
    std::optional<ProvenIdentity> authenticatedMachine {};

    /// Engaged by a machine ticket this connection presented that verified under a key this node's
    /// roster REVOKED, and never cleared: the connection is the forgotten machine's, so every later
    /// verb on it is refused as that machine's whatever the address or a later AUTH says. The one
    /// fact here that can only refuse (`RevokedKeyEvidence`).
    std::optional<RevokedKeyEvidence> revokedMachine {};

    [[nodiscard]] friend bool operator==(ConnectionFacts const&, ConnectionFacts const&) = default;
};

} // namespace FastCache
