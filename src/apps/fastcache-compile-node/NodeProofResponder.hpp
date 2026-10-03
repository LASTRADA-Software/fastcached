// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ConsensusStanding.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Node
{

/// @file NodeProofResponder.hpp
/// Where a caller proves WHICH machine it is, and its connection is sealed for having done so
/// (#178).
///
/// ## The problem, in one sentence
///
/// Node-to-node admission on the `0xFC` surface was decided by the caller's SOURCE ADDRESS, and
/// then by a MAC under a key every member shared -- so an address that changes per session could
/// not be followed, and a machine the cluster removed kept every byte it needed to prove itself
/// ([#178](https://github.com/LASTRADA-Software/fastcached/issues/178),
/// [#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428)). Each machine now signs
/// with its own identity key, and a revocation is one roster entry. `Distributed::NodeProof`
/// carries the handshake and why every frame after it is sealed.
///
/// ## Why a component of its own
///
/// `VerbFamily::NodeProof` carries that argument in full. The short of it: a proof is the caller's
/// OWN signature over this connection, while the `Session` family's `AUTH` carries a ticket -- a
/// statement another machine made about the caller -- so folding these verbs into it would let one
/// stand in for the other.
///
/// ## What it does NOT decide
///
/// Admission. It establishes one fact about one connection and writes it nowhere: the endpoint
/// records the proven identity on the connection, and `Node::RefuseUnlessMember` folds it into the
/// admission answer through `Distributed::ExplainConnection` on every verb. So this component
/// holds no state at all between requests, which is what lets one object serve every connection
/// on the port.
///
/// ## It is built only on a node that runs CONSENSUS
///
/// A proof is judged against the cluster's applied roster -- members, enrolled principals and
/// revoked keys -- which only a node running consensus holds. Every other node leaves the component
/// null and `MergedResponder` answers the whole family `NoCluster` -- never `UnimplementedVerb`,
/// which a caller reads as *this node's build is too old* and acts on by upgrading a machine that
/// is already current.

/// Serves `NodeChallenge` and `ProveNode`.
///
/// Both verbs are terminated by the ENDPOINT, which owns the connection state they change; this
/// class is what the endpoint asks. `Answer` is therefore reachable only by a caller that went
/// round the endpoint, and says so rather than pretending to serve.
class NodeProofResponder final: public IFrameResponder, public INodeProver
{
  public:
    /// @param nodeId This node's id, as it names itself in every reply.
    /// @param identity This node's identity key pair, read once at startup; must outlive this.
    /// @param roster Which identity keys the cluster holds live and which it revoked -- the node's
    ///        admission oracle, whose `ExplainKey` is the one door to that answer; must outlive this.
    /// @param consensus Whether the state that roster is published from has caught up with the log
    ///        this node recovered at start: a key it lacks while it has not is refused
    ///        `RosterNotYetApplied`, never `NodeKeyUnknown`. The node's `ConsensusStandingSlot`, which
    ///        answers `Unknown` until the tier is attached; must outlive this.
    /// @param random Where a handshake's nonce and ephemeral key come from; must outlive this. A
    ///        test scripts it to fail, which is how a challenge this node cannot draw is shown to
    ///        be refused.
    /// @param metrics Where every outcome of the exchange is recorded; must outlive this.
    /// @param logger Where a draw this node cannot make is reported; must outlive this. That one
    ///        condition is this machine's to fix and no counter can carry WHY.
    NodeProofResponder(std::string nodeId,
                       Ed25519KeyPair const& identity,
                       Distributed::IMembershipOracle const& roster,
                       IConsensusStandingSource const& consensus,
                       ISecureRandom& random,
                       IMetricsSink& metrics,
                       ILogger& logger) noexcept:
        _nodeId { std::move(nodeId) },
        _identity { identity },
        _roster { roster },
        _consensus { consensus },
        _random { random },
        _metrics { metrics },
        _logger { logger }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// **Both verbs are answered by the endpoint, so this is reached only from a caller that
    /// bypassed it** -- which a test can do, and which is why this answers rather than asserts.
    /// It refuses without a counter: a rise would say something about this process's own wiring
    /// and nothing about the fleet, and `LiveStatsResponder` answers the same way for the same
    /// reason about the verb IT does not terminate here.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// **Nobody is refused, and that is the one door this surface opens.** The machine asking is
    /// by construction on no list -- being on none is the entire problem being solved -- so a
    /// membership test here would refuse exactly the population the verb exists for.
    ///
    /// What stands in place of the list is the signature and the roster: a caller that cannot
    /// sign this connection's handshake under a key the cluster holds live learns nothing and is
    /// admitted to nothing, and its connection goes on being judged by its address exactly as
    /// before.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
    }

    /// @copydoc IFrameResponder::RefusalReply
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// The endpoint's own header window. Both verbs are answered from memory and one signature,
    /// so what this bounds is a peer dribbling a payload it already has in hand.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// The larger of the two verbs' own ceilings, which is `MaxNodeProofPayload`.
    ///
    /// **Not read on the merged listener**: `VerbFamily::NodeProof`'s `FamilyRoutes` row says
    /// `SessionCeilings::NotRead`, so `MergedResponder::Largest` does not fold this. What bounds
    /// a caller here is the VERB's own row, which `DecidePrePayload` takes when the verb declares
    /// one -- and both of these do. The number below is this responder's honest answer and
    /// nothing more; it is a pure virtual, so it cannot simply be dropped.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxNodeProofPayload;
    }

    /// A handful: the proof rides the connection a node was going to open anyway.
    ///
    /// **This number protects nothing, and must not be read as a narrowing**, for the reason
    /// `EnrollmentResponder` states at the same member: `MergedResponder::Largest` does not fold
    /// this row, and `Largest` is a MAXIMUM -- folded in, a small value would change nothing
    /// while a larger one would widen the cache and compile surfaces too.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 64;
    }

    /// The connection cap times the request cap, so the byte budget never refuses anything the
    /// connection cap would have allowed. Derived from the two rows above and unread for the
    /// same reason they are.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return 64 * CompileCacheWire::MaxNodeProofPayload;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: both verbs are answered from memory in microseconds, so the endpoint's reservation is
    /// released almost as soon as it is taken and a second accounting would add nothing.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None. A peer cannot realistically vanish inside one signature, and the write that would
    /// discover it is the next statement anyway.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// None: nothing here waits on anything, so a pulse would be a frame that cannot arrive
    /// before the reply it precedes.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream**: the exchange is two requests and two replies, each answered at once.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **This surface IS the prover**, which is what makes the endpoint's handling of the two
    /// verbs agree with the door by construction: `MergedResponder` answers both questions from
    /// the one `FamilyRoutes` row, so a node that refuses the family has no prover and a node
    /// that has a prover serves the family.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return this;
    }

    /// @copydoc INodeProver::Challenge
    [[nodiscard]] std::expected<NodeChallengeIssued, std::vector<std::byte>> Challenge(
        std::span<std::byte const> payload) override;

    /// @copydoc INodeProver::Verify
    [[nodiscard]] NodeProofVerdict Verify(NodeHandshake const& handshake, std::span<std::byte const> payload) override;

  private:
    /// Why a proof that verified is refused as unknown, with the remedy that fits WHO proved it.
    ///
    /// **A remedy that sends its reader somewhere the refusal persists is worse than none**, and
    /// "admit it" does exactly that for the two callers that carry THIS node's own id: this node
    /// itself, proving to its own scheduler before its own consensus has recorded it -- nothing to
    /// admit, the record is on its way -- and another machine holding a copy of this node's state
    /// directory, which admitting would not separate from this one. Only a stranger's own id is
    /// told how a machine gets admitted. And this node's own id and key, recorded under ANOTHER
    /// key, is a `node-key` replaced while its id survived, which waiting does not fix: answered
    /// with the words this node's prover gives itself (`ReplacedNodeKeyDiagnosis`). Never for a
    /// stranger's id, where the same shape may be another machine claiming a member's id.
    /// @param proven The identity the proof verified under.
    /// @return The refusal's words.
    [[nodiscard]] std::string UnknownKeyReason(ProvenIdentity const& proven) const;

    std::string _nodeId;
    Ed25519KeyPair const& _identity;
    Distributed::IMembershipOracle const& _roster;
    IConsensusStandingSource const& _consensus;
    ISecureRandom& _random;
    IMetricsSink& _metrics;

    /// Where the one condition no counter can explain is said out loud: this node's generator
    /// cannot draw a handshake. A counter would tell an operator that proofs are failing and not
    /// that the failure is on THIS machine, which is the whole of the diagnosis.
    ILogger& _logger;
};

} // namespace FastCache::Node
