// SPDX-License-Identifier: Apache-2.0
#include "MembershipGate.hpp"
#include "NodeProofResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/LeaseRosterFakes.hpp>
#include <tests/NodeProofFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ErrorOf;
using FastCache::Testing::StatusOf;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The address every case here dials from: on no member list, which is the whole subject.
constexpr std::string_view StrangerAddress = "10.9.9.9";

/// The one address this fixture's `--fleet-member` list admits, so a refusal is about WHO asked.
constexpr std::string_view ListedAddress = "10.0.0.1";

/// A machine the cluster admitted: its key is live under its own id.
constexpr std::string_view AdmittedNode = "node-7";

/// A second admitted machine, whose key is live under ITS id and nobody else's.
constexpr std::string_view OtherNode = "node-9";

/// A machine the cluster forgot: its key is revoked.
constexpr std::string_view ForgottenNode = "retired";

/// A machine the cluster never heard of.
constexpr std::string_view UnknownNode = "stranger";

/// The identity @p machine proves when it signs with its own test key under its own id.
/// @param machine The machine.
/// @return What its connection proved.
[[nodiscard]] ProvenIdentity IdentityOf(std::string_view machine)
{
    return ProvenIdentity { .id = std::string { machine }, .key = TestKeyPair(std::string { machine }).PublicKey() };
}

/// One scheduler, its `--fleet-member` list, the cluster's key roster, and the prover beside them.
///
/// The oracle is composed exactly as `NodeMembership` composes it -- the listed hosts and the key
/// roster under one fold -- so what a case learns about a proven identity is what every surface
/// that binds the node's oracle would learn.
struct ProvingNode
{
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    ManualClock clock;
    ManualWallClock wallClock;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };

    /// One listed host, and it is NOT the address most cases dial from. That is what makes every
    /// admission below attributable to the identity rather than to the list.
    Distributed::ClusterMembership listed { Distributed::MembershipParticipant::FleetMemberList,
                                            { std::string { ListedAddress } + ":7000" } };
    Distributed::KeyRosterMembership keys;
    Distributed::AnyOfMembership membership { { &listed, &keys } };
    SchedulerResponder scheduler { protocol, membership, metrics };

    Ed25519KeyPair const identity = TestKeyPair("scheduler");
    Testing::ScriptedSecureRandom random { Testing::ServerHandshakeScript() };
    NodeProofResponder prover { "scheduler", identity, membership, random, metrics, logger };

    ProvingNode()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        Testing::PublishKeyRoster(
            keys, { std::string { AdmittedNode }, std::string { OtherNode } }, { std::string { ForgottenNode } });
    }
};

/// A caller's opening and the challenge @p node answered it with.
struct Challenged
{
    Testing::CallerHandshake caller; ///< What the caller sent, and its secret.
    NodeChallengeIssued issued;      ///< What the server holds and what it answered.
};

/// Open a handshake with @p node as a caller would.
/// @param node The node.
/// @return Both halves.
[[nodiscard]] Challenged Challenge(ProvingNode& node)
{
    Testing::ScriptedSecureRandom callerRandom { Testing::CallerHandshakeScript() };
    auto opening = Testing::OpenHandshake(callerRandom);
    auto issued = node.prover.Challenge(Testing::RequestPayloadOf(Wire::EncodeNodeChallenge(opening.request)));
    REQUIRE(issued.has_value());
    return Challenged { .caller = std::move(opening), .issued = *std::move(issued) };
}

/// @p machine's proof, claiming @p claimedId, over @p challenged's handshake.
/// @param challenged The handshake.
/// @param machine Whose key signs.
/// @param claimedId The id it names.
/// @return The payload.
[[nodiscard]] std::vector<std::byte> ProofOver(Challenged const& challenged,
                                               std::string_view machine,
                                               std::string_view claimedId)
{
    return Testing::ProofPayload(
        std::string { machine }, claimedId, challenged.caller.request, challenged.issued.handshake.reply);
}

/// A lease request: a CLIENT verb, gated on membership and not on identity.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> LeaseFrame()
{
    return Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
}

/// The row a surface answers a host nobody listed with, for the gate cases.
constexpr Cc::SurfaceRefusal Stranger {
    .code = Wire::ErrorCode::NotAMember,
    .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember,
};

} // namespace

TEST_CASE("A machine proving an admitted identity is admitted at an address on no list, and a stranger there is not",
          "[node][proof][admission]")
{
    // **#1428's acceptance clause, kept under #178, and one case rather than two because the pair
    // is the assertion.** The calls differ in exactly one thing -- whether the connection proved an
    // identity the cluster holds -- and every other input, the address included, is identical.
    ProvingNode node;

    auto const refused =
        SyncRun(node.scheduler.Answer(LeaseFrame(), PeerIdentity { .host = std::string { StrangerAddress } })).bytes;
    CHECK(ErrorOf(refused) == Wire::ErrorCode::NotAMember);

    // The same address, having proved: past the gate, and answered by the fleet itself. `NoWorker`
    // is the fleet answering -- there is no worker registered here -- which is the only reply that
    // says the caller got in.
    auto const admitted = SyncRun(node.scheduler.Answer(LeaseFrame(),
                                                        PeerIdentity { .host = std::string { StrangerAddress },
                                                                       .proven = IdentityOf(AdmittedNode) }))
                              .bytes;
    CHECK(ErrorOf(admitted) == Wire::ErrorCode::NoWorker);

    // **#178 item 1**: the address changes between sessions and nothing is edited.
    auto const moved =
        SyncRun(
            node.scheduler.Answer(LeaseFrame(), PeerIdentity { .host = "198.51.100.4", .proven = IdentityOf(AdmittedNode) }))
            .bytes;
    CHECK(ErrorOf(moved) == Wire::ErrorCode::NoWorker);

    // And an identity the cluster does NOT hold admits nothing: a caller that signs under its own
    // key is who it says, and still a stranger.
    auto const unknown = SyncRun(node.scheduler.Answer(LeaseFrame(),
                                                       PeerIdentity { .host = std::string { StrangerAddress },
                                                                      .proven = IdentityOf(UnknownNode) }))
                             .bytes;
    CHECK(ErrorOf(unknown) == Wire::ErrorCode::NotAMember);
}

TEST_CASE("A proven identity names every route that admitted it", "[node][proof][admission]")
{
    // The fold UNIONS on a tie rather than choosing: a host on `--fleet-member` that also proves an
    // admitted identity is admitted by both, and an operator who removes it from the list has to be
    // told the identity still admits it.
    ProvingNode node;

    auto const provedOnly = Distributed::ExplainConnection(node.membership, StrangerAddress, IdentityOf(AdmittedNode));
    CHECK(provedOnly.verdict == Distributed::Membership::Member);
    CHECK(provedOnly.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));
    CHECK_FALSE(provedOnly.decidedBy.Has(Distributed::MembershipParticipant::FleetMemberList));
    CHECK(Distributed::RestsOnProvenIdentity(provedOnly));

    auto const both = Distributed::ExplainConnection(node.membership, ListedAddress, IdentityOf(AdmittedNode));
    CHECK(both.verdict == Distributed::Membership::Member);
    CHECK(both.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));
    CHECK(both.decidedBy.Has(Distributed::MembershipParticipant::FleetMemberList));
    CHECK(both.decidedBy.Count() == 2);

    // The control: the same address with nothing proved is an outsider whom NO route decided.
    auto const neither = Distributed::ExplainConnection(node.membership, StrangerAddress, std::nullopt);
    CHECK(neither.verdict == Distributed::Membership::Outsider);
    CHECK(neither.decidedBy.Empty());

    // A key live under ANOTHER id is not that id's: the roster holds a key per id, so node-9
    // claiming node-7's id with node-9's key is nobody the cluster admitted.
    auto const borrowed = ProvenIdentity { .id = std::string { AdmittedNode }, .key = IdentityOf(OtherNode).key };
    CHECK(Distributed::ExplainConnection(node.membership, StrangerAddress, borrowed).verdict
          == Distributed::Membership::Outsider);
}

TEST_CASE("A revoked key is Forgotten although its host is on --fleet-member", "[node][proof][admission][revoke]")
{
    // **#178's revocation, and the reason a forget no longer means rotating a key on every other
    // machine.** The forgotten machine dials from an address `--fleet-member` still lists -- the
    // operator has not edited every node's list, and must not have to -- and proves the key the
    // cluster revoked. The key outranks the listing: `Forgotten`, decided by the key's tombstone.
    ProvingNode node;

    auto const decision = Distributed::ExplainConnection(node.membership, ListedAddress, IdentityOf(ForgottenNode));
    CHECK(decision.verdict == Distributed::Membership::Forgotten);
    CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone));
    CHECK_FALSE(decision.decidedBy.Has(Distributed::MembershipParticipant::FleetMemberList));
    CHECK_FALSE(Distributed::RestsOnProvenIdentity(decision));

    // Loopback too: this machine's own address admits it on every list, and a revoked key is still
    // refused -- the one case where "the address always admits" and "the key is revoked" meet.
    CHECK(Distributed::ExplainConnection(node.membership, "127.0.0.1", IdentityOf(ForgottenNode)).verdict
          == Distributed::Membership::Forgotten);

    // At every gate that folds the connection, counted as the removed MACHINE rather than as a
    // forgotten host or a stranger -- the diagnoses are opposite.
    auto const refusal =
        RefuseUnlessMember(node.membership,
                           node.metrics,
                           PeerIdentity { .host = std::string { ListedAddress }, .proven = IdentityOf(ForgottenNode) },
                           Stranger,
                           "members only");
    REQUIRE(refusal.has_value());
    CHECK(ErrorOf(Unwrap(refusal)) == Wire::ErrorCode::NotAMember);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 1);
    CHECK(node.metrics.Read(HostForgotten.counter) == 0);
    CHECK(node.metrics.Read(Stranger.counter) == 0);

    // And the scheduler, which reads the same fold: refused.
    auto const lease = SyncRun(node.scheduler.Answer(LeaseFrame(),
                                                     PeerIdentity { .host = std::string { ListedAddress },
                                                                    .proven = IdentityOf(ForgottenNode) }))
                           .bytes;
    CHECK(ErrorOf(lease) == Wire::ErrorCode::NotAMember);

    // The control, which is what says the KEY decided: the same address, proving nothing, is
    // admitted by the list -- as a client, which is all an address admits since #178.
    CHECK(Distributed::ExplainConnection(node.membership, ListedAddress, std::nullopt).verdict
          == Distributed::Membership::Member);
    auto const listedClient =
        SyncRun(node.scheduler.Answer(LeaseFrame(), PeerIdentity { .host = std::string { ListedAddress } })).bytes;
    CHECK(ErrorOf(listedClient) == Wire::ErrorCode::NoWorker);
}

TEST_CASE("A forgotten host is refused although it proves an admitted key", "[node][proof][admission]")
{
    // The precedence is unchanged by an identity: a host tombstone outranks a live key exactly as
    // it outranks a listing, so a machine an operator forgot BY ADDRESS stays forgotten there.
    Distributed::ForgottenMembership tombstoned;
    tombstoned.Publish({ std::string { StrangerAddress } });
    Distributed::KeyRosterMembership keys;
    Testing::PublishKeyRoster(keys, { std::string { AdmittedNode } });
    Distributed::AnyOfMembership const oracle { { &tombstoned, &keys } };

    auto const decision = Distributed::ExplainConnection(oracle, StrangerAddress, IdentityOf(AdmittedNode));
    CHECK(decision.verdict == Distributed::Membership::Forgotten);
    CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::ClientTombstone));
}

TEST_CASE("An admitted key proves its identity, and the answer carries the keys that seal the connection", "[node][proof]")
{
    ProvingNode node;
    auto const challenged = Challenge(node);

    // The reply the caller reads is the one the server keeps: a caller verifies what it was SENT.
    auto const sent = Wire::DecodeNodeChallengeReply(Testing::PayloadOf(challenged.issued.reply));
    REQUIRE(sent.has_value());
    CHECK(Unwrap(sent) == challenged.issued.handshake.reply);
    CHECK(Unwrap(sent).serverId == "scheduler");
    CHECK(Distributed::VerifyNodeChallengeReply(challenged.caller.request, Unwrap(sent)));

    auto const verdict = node.prover.Verify(challenged.issued.handshake, ProofOver(challenged, AdmittedNode, AdmittedNode));
    CHECK(StatusOf(verdict.reply) == Wire::Status::Ok);
    CHECK(verdict.identity == std::optional { IdentityOf(AdmittedNode) });
    REQUIRE(verdict.keys.has_value());
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 1);

    // The keys are the caller's too: derived at each end from its own secret, they agree.
    auto const callerKeys = Distributed::DeriveNodeSessionKeys(
        challenged.caller.secret, challenged.caller.request, challenged.issued.handshake.reply, AdmittedNode, true);
    REQUIRE(callerKeys.has_value());
    CHECK(std::ranges::equal(Unwrap(callerKeys).callerToServer.Bytes(), Unwrap(verdict.keys).callerToServer.Bytes()));
    CHECK(std::ranges::equal(Unwrap(callerKeys).serverToCaller.Bytes(), Unwrap(verdict.keys).serverToCaller.Bytes()));
}

TEST_CASE("A signature that does not verify is refused, and counted apart from a proof that will not decode",
          "[node][proof]")
{
    // Two refusals, two counters, because the remedies are opposite: a peer that cannot form the
    // frame is a client-library mismatch, and one forming it correctly with a signature that does
    // not verify is somebody who does not hold the key it presented. Summed, the second hides
    // inside the first whenever an old client is in the fleet.
    ProvingNode node;

    SECTION("a signature under another key than the one presented")
    {
        auto const challenged = Challenge(node);
        auto const decoded = Wire::DecodeProveNodePayload(ProofOver(challenged, UnknownNode, AdmittedNode));
        REQUIRE(decoded.has_value());
        auto forged = Unwrap(decoded);
        std::ranges::copy(IdentityOf(AdmittedNode).key, forged.publicKey.begin());
        auto const verdict =
            node.prover.Verify(challenged.issued.handshake, Testing::RequestPayloadOf(Wire::EncodeProveNode(forged)));
        CHECK(ErrorOf(verdict.reply) == Wire::ErrorCode::NodeProofRejected);
        CHECK_FALSE(verdict.identity.has_value());
        // Sealed all the same: the caller reads every answer to a proof with one grammar.
        CHECK(verdict.keys.has_value());
    }

    SECTION("a signature relabelled with another admitted id")
    {
        auto const challenged = Challenge(node);
        auto const decoded = Wire::DecodeProveNodePayload(ProofOver(challenged, AdmittedNode, AdmittedNode));
        REQUIRE(decoded.has_value());
        auto relabelled = Unwrap(decoded);
        relabelled.nodeId = std::string { OtherNode };
        auto const verdict =
            node.prover.Verify(challenged.issued.handshake, Testing::RequestPayloadOf(Wire::EncodeProveNode(relabelled)));
        CHECK(ErrorOf(verdict.reply) == Wire::ErrorCode::NodeProofRejected);
    }

    SECTION("a proof over another connection's handshake")
    {
        // What a replay of a recorded proof onto a second connection presents: well-formed,
        // genuinely signed, over a transcript this server never ran.
        auto const first = Challenge(node);
        auto const recorded = ProofOver(first, AdmittedNode, AdmittedNode);
        Testing::ScriptedSecureRandom fresh { Testing::ScriptedSecureRandom::Ascending(2 * NonceBytes, 0x40) };
        NodeProofResponder second { "scheduler", node.identity, node.membership, fresh, node.metrics, node.logger };
        Testing::ScriptedSecureRandom callerRandom { Testing::CallerHandshakeScript() };
        auto const opening = Testing::OpenHandshake(callerRandom);
        auto const issued = second.Challenge(Testing::RequestPayloadOf(Wire::EncodeNodeChallenge(opening.request)));
        REQUIRE(issued.has_value());
        auto const verdict = second.Verify(Unwrap(issued).handshake, recorded);
        CHECK(ErrorOf(verdict.reply) == Wire::ErrorCode::NodeProofRejected);
    }

    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRejected) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 0);

    // And a payload that is not a proof at all, on top.
    auto const challenged = Challenge(node);
    auto const malformed =
        node.prover.Verify(challenged.issued.handshake, Testing::RequestPayloadOf(Wire::EncodeFetch("x")));
    CHECK(ErrorOf(malformed.reply) == Wire::ErrorCode::MalformedFrame);
    CHECK_FALSE(malformed.keys.has_value());
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsMalformed) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRejected) == 1);

    // And the right proof, so the refusals above are about the signature rather than about a
    // verifier that refuses everything -- which is what each would look like on its own.
    auto const right = node.prover.Verify(challenged.issued.handshake, ProofOver(challenged, AdmittedNode, AdmittedNode));
    CHECK(StatusOf(right.reply) == Wire::Status::Ok);
}

TEST_CASE("A key the cluster does not hold is refused as unknown, never as a forgery", "[node][proof]")
{
    // The caller signed: it IS the machine holding that key, and the cluster simply has not admitted
    // it. An operator reading `NodeProofRejected` would go looking for an attacker; this is a
    // machine waiting to be enrolled.
    ProvingNode node;

    SECTION("a key nobody admitted")
    {
        auto const challenged = Challenge(node);
        auto const verdict =
            node.prover.Verify(challenged.issued.handshake, ProofOver(challenged, UnknownNode, UnknownNode));
        CHECK(ErrorOf(verdict.reply) == Wire::ErrorCode::NodeKeyUnknown);
        CHECK_FALSE(verdict.identity.has_value());
        CHECK(verdict.keys.has_value());
    }

    SECTION("an admitted machine's key, under another admitted machine's id")
    {
        // Signed genuinely by node-9, naming node-7: the roster holds node-7's OWN key for that id,
        // so node-9 cannot borrow it.
        auto const challenged = Challenge(node);
        auto const verdict = node.prover.Verify(challenged.issued.handshake, ProofOver(challenged, OtherNode, AdmittedNode));
        CHECK(ErrorOf(verdict.reply) == Wire::ErrorCode::NodeKeyUnknown);
        CHECK_FALSE(verdict.identity.has_value());
    }

    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRefusedUnknownKey) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRejected) == 0);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRefusedRevokedKey) == 0);
}

TEST_CASE("A revoked key is refused by name, and the connection keeps the identity it proved", "[node][proof][revoke]")
{
    // The forgotten machine itself, still holding its key and still dialling. Refused -- and the
    // identity is KEPT on the connection rather than dropped, so every later request on it is
    // refused as the forgotten machine's; a connection that forgot what it proved would be judged
    // by its address again, which `--fleet-member` may still admit.
    ProvingNode node;
    auto const challenged = Challenge(node);
    auto const verdict =
        node.prover.Verify(challenged.issued.handshake, ProofOver(challenged, ForgottenNode, ForgottenNode));

    CHECK(ErrorOf(verdict.reply) == Wire::ErrorCode::NodeKeyRevoked);
    CHECK(verdict.identity == std::optional { IdentityOf(ForgottenNode) });
    CHECK(verdict.keys.has_value());
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRefusedRevokedKey) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsRefusedUnknownKey) == 0);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 0);
}

TEST_CASE("A challenge this node cannot draw is refused as NoCluster, uncounted, and said", "[node][proof]")
{
    // Never a nonce or an ephemeral key from anywhere weaker (#1527). `NoCluster`: the caller did
    // nothing wrong. Uncounted -- every node-proof counter describes a CALLER -- and the Error is
    // where the seam's own failure is named.
    ProvingNode node;
    node.random.Deny(Testing::ScriptedSecureRandom::DeniedFailure());

    Testing::ScriptedSecureRandom callerRandom { Testing::CallerHandshakeScript() };
    auto const opening = Testing::OpenHandshake(callerRandom);
    auto const refused = node.prover.Challenge(Testing::RequestPayloadOf(Wire::EncodeNodeChallenge(opening.request)));

    REQUIRE_FALSE(refused.has_value());
    CHECK(node.random.FillCount() == 1);
    CHECK(ErrorOf(refused.error()) == Wire::ErrorCode::NoCluster);
    for (auto const counter: { IMetricsSink::Counter::NodeProofsAccepted,
                               IMetricsSink::Counter::NodeProofsRejected,
                               IMetricsSink::Counter::NodeProofsMalformed,
                               IMetricsSink::Counter::NodeProofsUnchallenged })
        CHECK(node.metrics.Read(counter) == 0);

    auto const lines = node.logger.Snapshot();
    CHECK(std::ranges::any_of(lines, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error
               && record.message.contains(Testing::ScriptedSecureRandom::DeniedFailure().primitive);
    }));
}

TEST_CASE("A challenge that will not decode is refused and counted as malformed", "[node][proof]")
{
    // A version-13 peer's challenge had no fields at all, and an old client is the ordinary reason
    // for this refusal: counted where a mismatch is, never where a wrong key is.
    ProvingNode node;
    auto const refused = node.prover.Challenge({});
    REQUIRE_FALSE(refused.has_value());
    CHECK(ErrorOf(refused.error()) == Wire::ErrorCode::MalformedFrame);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeProofsMalformed) == 1);
    CHECK(node.random.FillCount() == 0);
}

TEST_CASE("The node-proof surface refuses nobody at its door, and requires the credential when one is set", "[node][proof]")
{
    // The one hole this surface opens, and the gate beside it. The machine asking is on no list
    // -- being on none is the problem being solved -- so a membership test at the door would
    // refuse the population the verbs exist for.
    ProvingNode node;
    for (auto const op: { Wire::Op::NodeChallenge, Wire::Op::ProveNode })
    {
        CHECK_FALSE(
            node.prover.RefusePeer(PeerIdentity { .host = std::string { StrangerAddress } }, static_cast<std::uint8_t>(op))
                .has_value());
        CHECK_FALSE(Wire::FindOp(static_cast<std::uint8_t>(op))->preAuth.Allowed());
    }

    CHECK_FALSE(node.prover.AuthRequired(static_cast<std::uint8_t>(Wire::Op::ProveNode)));

    auto const policy = std::make_shared<AuthPolicy const>(std::string {}, std::string { "token" });
    NodeProofResponder gated { "scheduler", node.identity, node.membership, node.random, node.metrics, node.logger, policy };
    CHECK(gated.AuthRequired(static_cast<std::uint8_t>(Wire::Op::ProveNode)));
    CHECK(gated.AuthRequired(static_cast<std::uint8_t>(Wire::Op::NodeChallenge)));
}

TEST_CASE("A node running no consensus refuses the whole node-proof family, and says so", "[node][proof]")
{
    // `NoCluster` rather than `UnimplementedVerb`, for the enrollment family's reason: a caller told
    // the latter reads it as *this node's build is too old* and upgrades a machine already current.
    ProvingNode node;

    // Every component but the prover, which is what a node running no consensus is.
    MergedResponder merged { SurfaceComponents { .scheduler = &node.scheduler, .nodeProof = nullptr } };
    for (auto const op: { Wire::Op::NodeChallenge, Wire::Op::ProveNode })
    {
        auto const refused =
            merged.RefusePeer(PeerIdentity { .host = std::string { StrangerAddress } }, static_cast<std::uint8_t>(op));
        REQUIRE(refused.has_value());
        CHECK(ErrorOf(Unwrap(refused)) == Wire::ErrorCode::NoCluster);
        CHECK(ErrorOf(Unwrap(refused)) != Wire::UnimplementedVerb);
    }
    CHECK(merged.NodeProver() == nullptr);

    // The control: with a prover, the family is served and the door admits it.
    MergedResponder served { SurfaceComponents { .scheduler = &node.scheduler, .nodeProof = &node.prover } };
    CHECK(served.NodeProver() == &node.prover);
    CHECK_FALSE(served
                    .RefusePeer(PeerIdentity { .host = std::string { StrangerAddress } },
                                static_cast<std::uint8_t>(Wire::Op::ProveNode))
                    .has_value());
}
