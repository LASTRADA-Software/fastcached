// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ClusterKeySource.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Node
{

/// @file NodeProofResponder.hpp
/// Where a caller shows it holds the cluster key, and is admitted for having shown it.
///
/// ## The problem, in one sentence
///
/// Node-to-node admission on the `0xFC` surface is decided by the caller's SOURCE ADDRESS --
/// `ClusterMembership` against the committed endpoints, `--fleet-member` against a local list.
/// An address is a stand-in for *this is one of our nodes*, and it stops being one the moment
/// an address is not stable: a worker that joins over a VPN gets a different one each session
/// and no literal host match can follow it
/// ([#178](https://github.com/LASTRADA-Software/fastcached/issues/178) item 1,
/// [#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428)).
///
/// Every caller that matters here already holds the cluster key -- #282 refuses a
/// network-facing keyless worker at startup and #1308 refuses keyless consensus -- so it can
/// PROVE membership rather than being inferred from where it dialled from.
///
/// ## Why a component of its own
///
/// `VerbFamily::NodeProof` carries that argument in full. The short of it: the credential the
/// scheduler owns is `--scheduler-token-file`, an operator's token, and the cluster key is a
/// different secret in a different file that every member holds -- so folding these verbs into
/// the `Session` family would put the fleet's shared key in the component that owns the
/// operator's, and would leave a pure worker, which is an ordinary deployment, unprovable.
///
/// ## What it does NOT decide
///
/// Membership. It establishes one fact about one connection and writes it nowhere: the endpoint
/// records the proven id on the connection, and `Node::RefuseUnlessMember` folds it into the
/// admission answer through `Distributed::ExplainConnection`. So this component holds no state
/// at all between requests, which is what lets one object serve every connection on the port.
///
/// ## It is built only on a node that HOLDS a cluster key
///
/// A node with no `--cluster-key-file` has nothing to verify against, so `main` leaves the
/// component null and `MergedResponder` answers the whole family `NoCluster` -- naming the flag,
/// rather than `UnimplementedVerb`, which a caller reads as *this node's build is too old* and
/// acts on by upgrading a machine that is already current.

/// Serves `NodeChallenge` and `ProveNode`.
///
/// Both verbs are terminated by the ENDPOINT, which owns the connection state they change; this
/// class is what the endpoint asks. `Answer` is therefore reachable only by a caller that went
/// round the endpoint, and says so rather than pretending to serve.
class NodeProofResponder final: public IFrameResponder, public INodeProver
{
  public:
    /// @param key Where the cluster key is read from at each verification; must outlive this.
    /// @param random Where a challenge's bytes come from; must outlive this. A test scripts it
    ///        to fix the nonce, which is the only way a proof is reproducible at all.
    /// @param metrics Where every outcome of the exchange is recorded; must outlive this.
    /// @param logger Where a key file that has stopped being readable is reported; must outlive
    ///        this. That one condition is an operator's to fix and no counter can carry WHY.
    /// @param policy The credential this surface requires, or nullptr for none. Shared rather
    ///        than referenced because "there is no credential" has to be representable.
    NodeProofResponder(IClusterKeySource const& key,
                       IRandomSource& random,
                       IMetricsSink& metrics,
                       ILogger& logger,
                       std::shared_ptr<AuthPolicy const> policy = nullptr) noexcept:
        _key { key },
        _random { random },
        _metrics { metrics },
        _logger { logger },
        _policy { std::move(policy) }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// **Both verbs are answered by the endpoint, so this is reached only from a caller that
    /// bypassed it** -- which a test can do, and which is why this answers rather than asserts.
    /// It refuses without a counter: a rise would say something about this process's own wiring
    /// and nothing about the fleet, and `LiveStatsResponder` answers the same way for the same
    /// reason about the verb IT does not terminate here.
    [[nodiscard]] Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// **Nobody is refused, and that is the one door this surface opens.** The machine asking is
    /// by construction on no list -- being on none is the entire problem being solved -- so a
    /// membership test here would refuse exactly the population the verb exists for.
    ///
    /// What stands in place of the list is the MAC: a caller that cannot produce a tag over this
    /// connection's challenge under this cluster's key learns nothing and is admitted to nothing,
    /// and its connection goes on being judged by its address exactly as before. The credential
    /// gate is untouched as well -- both verbs are `RequiresAuth` (`VerbFamily::NodeProof` says
    /// why), so a fleet with `--scheduler-token-file` set still requires the token here.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// The surface-wide answer, and the opcode is deliberately ignored: which verb is reachable
    /// before a credential is `OpTable::preAuth`'s column and `DecidePrePayload` reads it, so
    /// answering per verb here would be a second spelling of the pre-auth set -- one a reviewer
    /// cannot see from the table, and one that can disagree with it.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _policy != nullptr && _policy->Enabled();
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// Delegates to this surface's own policy, which is the scheduler's object. Unreachable
    /// through `MergedResponder` -- `AUTH` is a `Session` verb and routes to the scheduler -- and
    /// answered properly rather than stubbed, because a surface that inherits an answer inherits
    /// an open door by saying nothing.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(_policy.get(), payload);
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
    /// The endpoint's own header window. Both verbs are answered from memory and one HMAC, so
    /// what this bounds is a peer dribbling a payload it already has in hand.
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
    /// None. A peer cannot realistically vanish inside one HMAC, and the write that would
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

    /// @copydoc INodeProver::IssueChallenge
    [[nodiscard]] Nonce IssueChallenge() override
    {
        return DrawNonce(_random);
    }

    /// @copydoc INodeProver::Verify
    [[nodiscard]] std::expected<std::string, std::vector<std::byte>> Verify(std::span<std::byte const> challenge,
                                                                            std::span<std::byte const> payload) override;

  private:
    IClusterKeySource const& _key;
    IRandomSource& _random;
    IMetricsSink& _metrics;

    /// Where the one condition no counter can explain is said out loud: this node's own key file
    /// has stopped being readable. A counter would tell an operator that proofs are failing and
    /// not that the failure is on THIS machine, which is the whole of the diagnosis.
    ILogger& _logger;

    std::shared_ptr<AuthPolicy const> _policy;
};

} // namespace FastCache::Node
