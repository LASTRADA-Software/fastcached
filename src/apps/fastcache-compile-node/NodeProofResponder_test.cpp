// SPDX-License-Identifier: Apache-2.0
#include "NodeProofResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/NodeProofFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The address every case here dials from: on no member list, which is the whole subject.
constexpr std::string_view StrangerAddress = "10.9.9.9";

/// The one address this fixture's oracle admits, so a refusal is about WHO asked.
constexpr std::string_view ListedAddress = "10.0.0.1";

/// The label a proving node gives itself.
constexpr std::string_view ProvingNodeId = "node-7";

/// This cluster's key, and one that is not it.
constexpr std::string_view RightKey = "the-cluster-key-0123456789abcdef";
constexpr std::string_view WrongKey = "a-different-key-0123456789abcdef";

/// One node with a cluster key, its prover, and a scheduler that admits one address.
struct ProvingNode
{
    AtomicMetricsSink metrics;
    NullLogger logger;
    ManualClock clock;
    ManualWallClock wallClock;
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, {}, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };

    /// One listed host, and it is NOT the address the cases dial from. That is what makes
    /// every admission below attributable to the proof rather than to the list.
    Distributed::ClusterMembership membership { Distributed::MembershipParticipant::FleetMemberList,
                                                { std::string { ListedAddress } + ":7000" } };
    SchedulerResponder scheduler { protocol, membership, metrics };

    Testing::ScriptedClusterKey key { RightKey };
    Testing::FixedRandomSource random;
    NodeProofResponder prover { key, random, metrics, logger };

    ProvingNode()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    }

    /// The challenge this node's prover issues, which is fixed by `FixedRandom`.
    [[nodiscard]] Nonce Challenge()
    {
        return prover.IssueChallenge();
    }
};

/// The error code a refusal carries, or nullopt when the reply is not one.
/// @param reply The whole reply frame.
/// @return The code.
[[nodiscard]] std::optional<Wire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error)
        return std::nullopt;
    auto const refusal = Wire::DecodeErrorPayload(reply.subspan(Wire::ReplyHeaderSize, header->payloadLength));
    if (!refusal.has_value())
        return std::nullopt;
    return refusal->first;
}

/// A lease request, which is a verb the scheduler gates on membership.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> LeaseFrame()
{
    return Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
}

/// A `ProveNode` payload for @p nodeId under @p key against @p challenge.
/// @param key The key to sign with.
/// @param challenge The server's nonce.
/// @param nodeId The label to bind.
/// @return The payload, without the request header.
[[nodiscard]] std::vector<std::byte> ProofPayload(std::string_view key,
                                                  std::span<std::byte const> challenge,
                                                  std::string_view nodeId)
{
    auto const keyBytes = Testing::ProofBytesOf(key);
    auto const tag = Distributed::MintNodeProof(keyBytes, challenge, nodeId);
    auto const frame = Wire::EncodeProveNode(nodeId, tag);
    return std::vector<std::byte> { frame.begin() + Wire::RequestHeaderSize, frame.end() };
}

} // namespace

TEST_CASE("A key holder is admitted at an address on no list, and a stranger at that address is not",
          "[node][proof][admission]")
{
    // **#1428's acceptance clause, and it is one case rather than two because the pair is the
    // assertion.** The two calls differ in exactly one thing -- whether the connection proved
    // the cluster key -- and every other input, the address included, is identical. A test that
    // asserted only the admission would pass on a build that admitted everybody.
    ProvingNode node;

    // The stranger: refused `NotAMember`, which is the gate answering rather than the fleet.
    //
    // **The CODE here and the COUNTER in `MembershipGate_test`**, because the scheduler's
    // `NotAMember` is deliberately uncounted -- `SchedulerProtocol::RefusePeer` argues that
    // counting a policy answer beside the capacity refusals puts noise into the numbers a fleet
    // is sized from, and that the early and the late path are byte-identical so counting one
    // would under-report (#592). The row that moves for this decision is asserted where the
    // fold every surface reaches lives.
    auto const refused =
        SyncRun(node.scheduler.Answer(LeaseFrame(), PeerIdentity { .host = std::string { StrangerAddress } })).bytes;
    CHECK(ErrorOf(refused) == Wire::ErrorCode::NotAMember);

    // The same address, having PROVED the key: past the gate, and answered by the fleet itself.
    // `NoWorker` is the fleet answering the question -- there is no worker registered here --
    // which is the only reply that says the caller got in. `NotAMember` would be the regression.
    auto const admitted = SyncRun(node.scheduler.Answer(LeaseFrame(),
                                                        PeerIdentity { .host = std::string { StrangerAddress },
                                                                       .provenNodeId = std::string { ProvingNodeId } }))
                              .bytes;
    CHECK(ErrorOf(admitted) == Wire::ErrorCode::NoWorker);
    CHECK(ErrorOf(admitted) != Wire::ErrorCode::NotAMember);

    // **#178 item 1**: the address changes between sessions and nothing is edited. A SECOND
    // unlisted address, also proved, is admitted on the same terms -- which is what says the
    // first admission was about the proof rather than about that one host.
    auto const movedAddress =
        SyncRun(node.scheduler.Answer(
                    LeaseFrame(), PeerIdentity { .host = "198.51.100.4", .provenNodeId = std::string { ProvingNodeId } }))
            .bytes;
    CHECK(ErrorOf(movedAddress) == Wire::ErrorCode::NoWorker);
}

TEST_CASE("A proof names every route that admitted it, and a listed host that also proves names both",
          "[node][proof][admission]")
{
    // The fold is `AnyOfMembership`'s, so a tie UNIONS rather than choosing: a host on
    // `--fleet-member` that also holds the key is admitted by both, and an operator who removes
    // it from the list has to be told the key still admits it. Reporting one route would be
    // #1471's defect arriving through a route no file mentions.
    Distributed::ClusterMembership const listed { Distributed::MembershipParticipant::FleetMemberList,
                                                  { std::string { ListedAddress } + ":7000" } };

    auto const provedOnly = Distributed::ExplainConnection(listed, StrangerAddress, /*provedClusterKey=*/true);
    CHECK(provedOnly.verdict == Distributed::Membership::Member);
    CHECK(provedOnly.decidedBy.Has(Distributed::MembershipParticipant::ProvenKeyHolder));
    CHECK_FALSE(provedOnly.decidedBy.Has(Distributed::MembershipParticipant::FleetMemberList));

    auto const both = Distributed::ExplainConnection(listed, ListedAddress, /*provedClusterKey=*/true);
    CHECK(both.verdict == Distributed::Membership::Member);
    CHECK(both.decidedBy.Has(Distributed::MembershipParticipant::ProvenKeyHolder));
    CHECK(both.decidedBy.Has(Distributed::MembershipParticipant::FleetMemberList));
    CHECK(both.decidedBy.Count() == 2);

    // The control, and it is what says the proof is the thing being measured: the same address
    // with nothing proved is an outsider whom NO route decided about.
    auto const neither = Distributed::ExplainConnection(listed, StrangerAddress, /*provedClusterKey=*/false);
    CHECK(neither.verdict == Distributed::Membership::Outsider);
    CHECK(neither.decidedBy.Empty());
}

TEST_CASE("A forgotten host is refused although it proves the cluster key", "[node][proof][admission]")
{
    // **A proof does not resurrect a forgotten host**, and that is the composition's existing
    // direction rather than a softening of what a proof means: under a shared key a forget
    // cannot revoke a key holder at all, which `Distributed::NodeProof`'s header says and says
    // what closing would need (#178). What is testable is that the precedence is unchanged.
    Distributed::ForgottenMembership tombstoned;
    tombstoned.Publish({ std::string { StrangerAddress } });

    auto const decision = Distributed::ExplainConnection(tombstoned, StrangerAddress, /*provedClusterKey=*/true);
    CHECK(decision.verdict == Distributed::Membership::Forgotten);
    CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::ClientTombstone));
}

TEST_CASE("A proof under the wrong key is refused, and counted apart from one that will not decode", "[node][proof]")
{
    // Two refusals, two counters, because the remedies are opposite: a peer that cannot form the
    // frame is a client-library mismatch, and one forming it correctly with the wrong key is
    // either a machine with the wrong `--cluster-key-file` or somebody guessing. Summed, the
    // second hides inside the first whenever an old client is in the fleet.
    ProvingNode node;
    auto const challenge = node.Challenge();

    auto const wrong = node.prover.Verify(challenge, ProofPayload(WrongKey, challenge, ProvingNodeId));
    REQUIRE_FALSE(wrong.has_value());
    CHECK(ErrorOf(wrong.error()) == Wire::ErrorCode::NodeProofRejected);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRejected) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsMalformed) == 0);

    // A payload that is not an id and a 32-byte tag at all.
    auto const malformed = node.prover.Verify(challenge, Testing::ProofBytesOf("not a proof"));
    REQUIRE_FALSE(malformed.has_value());
    CHECK(ErrorOf(malformed.error()) == Wire::ErrorCode::MalformedFrame);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsMalformed) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRejected) == 1);

    // And the right key, so the two refusals above are about the key rather than about a
    // verifier that refuses everything -- which is what both would look like on their own.
    auto const right = node.prover.Verify(challenge, ProofPayload(RightKey, challenge, ProvingNodeId));
    REQUIRE(right.has_value());
    CHECK(*right == ProvingNodeId);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 1);
}

TEST_CASE("A proof binds the id it names, so a captured tag cannot be relabelled", "[node][proof]")
{
    // What binding the id buys, and the only thing it buys: a tag minted for one label does not
    // authenticate for another, so the id this server records is the one the holder claimed
    // rather than whatever a relay substituted. It does NOT establish WHICH holder is speaking
    // -- under a shared key every holder can mint any id's tag -- and the header says so.
    ProvingNode node;
    auto const challenge = node.Challenge();

    auto const forOtherId = ProofPayload(RightKey, challenge, "node-9");
    auto const relabelled = Wire::DecodeProveNodePayload(forOtherId);
    REQUIRE(relabelled.has_value());

    // The same tag, presented under a different label.
    auto const swapped = Wire::EncodeProveNode(ProvingNodeId, Testing::Unwrap(relabelled).tag);
    auto const swappedPayload = std::vector<std::byte> { swapped.begin() + Wire::RequestHeaderSize, swapped.end() };
    auto const refused = node.prover.Verify(challenge, swappedPayload);
    REQUIRE_FALSE(refused.has_value());
    CHECK(ErrorOf(refused.error()) == Wire::ErrorCode::NodeProofRejected);

    // And unswapped it authenticates, so the refusal above is the LABEL rather than the tag.
    auto const accepted = node.prover.Verify(challenge, forOtherId);
    REQUIRE(accepted.has_value());
    CHECK(*accepted == "node-9");
}

TEST_CASE("A proof over a different challenge is refused", "[node][proof]")
{
    // The challenge is the SERVER's and is per connection, which is what makes a captured proof
    // useless on a second one. Asserted by verifying a well-formed tag against a nonce it was
    // not minted for -- the same shape a replay onto another connection has.
    ProvingNode node;
    auto const mine = node.Challenge();
    auto const somebodyElses = Testing::ProofBytesOf("a different challenge, 32 bytes.");
    REQUIRE(somebodyElses.size() == Wire::NodeChallengeBytes);

    auto const refused = node.prover.Verify(mine, ProofPayload(RightKey, somebodyElses, ProvingNodeId));
    REQUIRE_FALSE(refused.has_value());
    CHECK(ErrorOf(refused.error()) == Wire::ErrorCode::NodeProofRejected);
}

TEST_CASE("A node that holds no cluster key proves nothing, and says which flag is missing", "[node][proof]")
{
    // A key file that has stopped being readable is a fact about THIS machine, so the refusal
    // must not say the caller's key is wrong: whoever reads `NodeProofRejected` goes off to
    // check a key that is fine. `NoCluster` and a Warn, which is where the path and the error
    // can be named -- and uncounted, because a counter could carry neither.
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    Testing::ScriptedClusterKey const absent { "", "open: no such file or directory" };
    Testing::FixedRandomSource random;
    NodeProofResponder prover { absent, random, metrics, logger };

    auto const challenge = prover.IssueChallenge();
    auto const refused = prover.Verify(challenge, ProofPayload(RightKey, challenge, ProvingNodeId));
    REQUIRE_FALSE(refused.has_value());
    CHECK(ErrorOf(refused.error()) == Wire::ErrorCode::NoCluster);
    CHECK(ErrorOf(refused.error()) != Wire::ErrorCode::NodeProofRejected);

    // Neither refusal counter moved: this is not a caller's failure.
    CHECK(metrics.Read(IMetricsSink::Counter::NodeProofsRejected) == 0);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeProofsMalformed) == 0);

    // And the diagnosis is in the log, naming the underlying error rather than paraphrasing it.
    auto const lines = logger.Snapshot();
    CHECK(std::ranges::any_of(lines, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Warn && record.message.contains("no such file or directory");
    }));
}

TEST_CASE("An empty label is a proof, because an identity is minted only by a consensus node", "[node][proof]")
{
    // **The case a node that runs no consensus is in**, which is every ordinary worker: it holds
    // the cluster key -- that is what signs its lease grants -- and has minted no id, so a proof
    // that required one would refuse exactly the population #1428 exists for.
    //
    // What carries *this connection proved the key* is that `PeerIdentity::provenNodeId` is
    // ENGAGED, which an empty string still is. Asserted here as an accepted proof, and the
    // admission half is asserted through the fold's `bool`.
    ProvingNode node;
    auto const challenge = node.Challenge();

    auto const proved = node.prover.Verify(challenge, ProofPayload(RightKey, challenge, ""));
    REQUIRE(proved.has_value());
    CHECK(proved->empty());
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 1);
}

TEST_CASE("The node-proof surface refuses nobody at its door, and requires the credential when one is set", "[node][proof]")
{
    // The one hole this surface opens, and the gate beside it. The machine asking is on no list
    // -- being on none is the problem being solved -- so a membership test at the door would
    // refuse the population the verbs exist for. Asserting only that would leave the pair
    // untested in the direction that matters, so the credential answer is asserted too: these
    // verbs are `RequiresAuth` in `OpTable`, which is what makes the hole one gate wide.
    ProvingNode node;
    for (auto const op: { Wire::Op::NodeChallenge, Wire::Op::ProveNode })
    {
        CHECK_FALSE(
            node.prover.RefusePeer(PeerIdentity { .host = std::string { StrangerAddress } }, static_cast<std::uint8_t>(op))
                .has_value());
        // `PreAuth` has no `operator==` -- a row must STATE its classification, so the type has
        // no default and no comparison; `Allowed()` is how the column is read.
        CHECK_FALSE(Wire::FindOp(static_cast<std::uint8_t>(op))->preAuth.Allowed());
    }

    // With no policy configured there is nothing to require, which is the single-machine default.
    CHECK_FALSE(node.prover.AuthRequired(static_cast<std::uint8_t>(Wire::Op::ProveNode)));

    // And with one, both verbs are behind it -- asked of the surface, which is where
    // `DecidePrePayload` asks.
    auto const policy = std::make_shared<AuthPolicy const>(std::string {}, std::string { "token" });
    NodeProofResponder gated { node.key, node.random, node.metrics, node.logger, policy };
    CHECK(gated.AuthRequired(static_cast<std::uint8_t>(Wire::Op::ProveNode)));
    CHECK(gated.AuthRequired(static_cast<std::uint8_t>(Wire::Op::NodeChallenge)));
}

TEST_CASE("A node holding no cluster key refuses the whole node-proof family by naming the flag", "[node][proof]")
{
    // `NoCluster` rather than `UnimplementedVerb`, for the enrollment family's reason: a caller
    // told the latter reads it as *this node's build is too old* and acts on it by upgrading a
    // machine that is already current. Asserted as NOT `UnimplementedVerb`, since both refuse.
    AtomicMetricsSink metrics;
    NullLogger logger;
    ManualClock clock;
    ManualWallClock wallClock;
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, {}, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };
    Distributed::ClusterMembership const membership { Distributed::MembershipParticipant::FleetMemberList, {} };
    SchedulerResponder scheduler { protocol, membership, metrics };

    // Every component but the prover, which is what a node with no `--cluster-key-file` is.
    MergedResponder merged { SurfaceComponents { .scheduler = &scheduler, .nodeProof = nullptr } };

    for (auto const op: { Wire::Op::NodeChallenge, Wire::Op::ProveNode })
    {
        auto const refused =
            merged.RefusePeer(PeerIdentity { .host = std::string { StrangerAddress } }, static_cast<std::uint8_t>(op));
        REQUIRE(refused.has_value());
        CHECK(ErrorOf(Testing::Unwrap(refused)) == Wire::ErrorCode::NoCluster);
        CHECK(ErrorOf(Testing::Unwrap(refused)) != Wire::UnimplementedVerb);
    }

    // And the surface offers no prover, which is what keeps the endpoint's handling of the two
    // verbs in step with the door: both read the one `FamilyRoutes` row.
    CHECK(merged.NodeProver() == nullptr);

    // The control: with a prover, the family is served and the door admits it.
    Testing::ScriptedClusterKey key { RightKey };
    Testing::FixedRandomSource random;
    NodeProofResponder prover { key, random, metrics, logger };
    MergedResponder served { SurfaceComponents { .scheduler = &scheduler, .nodeProof = &prover } };
    CHECK(served.NodeProver() == &prover);
    CHECK_FALSE(served
                    .RefusePeer(PeerIdentity { .host = std::string { StrangerAddress } },
                                static_cast<std::uint8_t>(Wire::Op::ProveNode))
                    .has_value());
}
