// SPDX-License-Identifier: Apache-2.0
//
// The node identity handshake's pure half (#178): who signs what, over which transcript, and the
// keys both ends derive from it. Where it runs on a connection is `NodeProofResponder_test` and
// `FrameEndpoint_test`; where the keys seal frames is `SealedFrameSocket_test`.
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Distributed/NodeProof.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/NodeProofFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The id the proving machine claims, and the machine whose test key it signs with.
constexpr std::string_view ProvingNode = "node-7";

/// One handshake, run by both ends to the point a proof has been minted.
struct Handshake
{
    Testing::CallerHandshake caller; ///< The caller's opening and its secret.
    NodeEphemeral server;            ///< The server's ephemeral pair.
    Wire::NodeChallengeReply reply;  ///< The server's signed answer.
    Wire::ProveNodeRequest proof;    ///< The caller's signed proof.
};

/// Run a handshake between the scheduler and @p ProvingNode, each drawing from its own script.
/// @param serverFirstByte Where the server's script starts, so two handshakes can differ.
/// @return Both halves.
[[nodiscard]] Handshake RunHandshake(std::uint8_t serverFirstByte = 0x00)
{
    Testing::ScriptedSecureRandom callerRandom { Testing::CallerHandshakeScript() };
    Testing::ScriptedSecureRandom serverRandom { Testing::ScriptedSecureRandom::Ascending(2 * NonceBytes, serverFirstByte) };

    auto caller = Testing::OpenHandshake(callerRandom);
    auto const nonce = DrawNonce(serverRandom);
    auto server = DrawNodeEphemeral(serverRandom);
    REQUIRE(nonce.has_value());
    REQUIRE(server.has_value());

    auto reply =
        AnswerNodeChallenge(TestKeyPair("scheduler"),
                            caller.request,
                            ServerHello { .serverId = "scheduler", .nonce = *nonce, .ephemeral = server->publicKey });
    auto proof = MintNodeProof(TestKeyPair(std::string { ProvingNode }), ProvingNode, caller.request, reply);
    return Handshake {
        .caller = std::move(caller), .server = *std::move(server), .reply = std::move(reply), .proof = std::move(proof)
    };
}

/// Whether two keys are the same key.
/// @param a One.
/// @param b The other.
/// @return True when their bytes are equal.
[[nodiscard]] bool SameKey(SessionKey const& a, SessionKey const& b)
{
    return std::ranges::equal(a.Bytes(), b.Bytes());
}

} // namespace

TEST_CASE("A signed handshake verifies at both ends", "[distributed][nodeproof]")
{
    // The control every refusal below is measured against: unaltered, both signatures verify.
    auto const handshake = RunHandshake();
    CHECK(VerifyNodeChallengeReply(handshake.caller.request, handshake.reply));
    CHECK(VerifyNodeProof(handshake.caller.request, handshake.reply, handshake.proof));

    // The reply names the key it was signed under, and the proof the key it presents: a verifier
    // asks its roster about THESE, never about a key it assumed.
    CHECK(std::ranges::equal(handshake.reply.serverKey, TestKeyPair("scheduler").PublicKey()));
    CHECK(std::ranges::equal(handshake.proof.publicKey, TestKeyPair(std::string { ProvingNode }).PublicKey()));
    CHECK(handshake.proof.nodeId == ProvingNode);
}

TEST_CASE("A server's reply verifies over its own handshake and nothing else", "[distributed][nodeproof]")
{
    // Every field is signed, so every field is one a relay cannot change: the id a caller checks
    // its roster for, the key it checks, the nonce and ephemeral key the session is derived from,
    // and the caller's own opening.
    auto const handshake = RunHandshake();

    auto renamed = handshake.reply;
    renamed.serverId = "someone-else";
    CHECK_FALSE(VerifyNodeChallengeReply(handshake.caller.request, renamed));

    auto rekeyed = handshake.reply;
    std::ranges::copy(TestKeyPair("impostor").PublicKey(), rekeyed.serverKey.begin());
    CHECK_FALSE(VerifyNodeChallengeReply(handshake.caller.request, rekeyed));

    auto renonced = handshake.reply;
    renonced.nonce[0] ^= std::byte { 0x01 };
    CHECK_FALSE(VerifyNodeChallengeReply(handshake.caller.request, renonced));

    auto swapped = handshake.reply;
    swapped.ephemeral[0] ^= std::byte { 0x01 };
    CHECK_FALSE(VerifyNodeChallengeReply(handshake.caller.request, swapped));

    // A reply recorded for ANOTHER caller's opening: what a relay replaying a captured reply has.
    auto otherOpening = handshake.caller.request;
    otherOpening.nonce[0] ^= std::byte { 0x01 };
    CHECK_FALSE(VerifyNodeChallengeReply(otherOpening, handshake.reply));
}

TEST_CASE("A proof verifies for the id it names, under the key it presents, over its own handshake",
          "[distributed][nodeproof]")
{
    auto const handshake = RunHandshake();

    // Relabelled: the signature binds the id, so a captured proof cannot be re-spent as another's.
    auto relabelled = handshake.proof;
    relabelled.nodeId = "node-9";
    CHECK_FALSE(VerifyNodeProof(handshake.caller.request, handshake.reply, relabelled));

    // Rekeyed: a machine presenting somebody else's key with its own signature.
    auto rekeyed = handshake.proof;
    std::ranges::copy(TestKeyPair("node-9").PublicKey(), rekeyed.publicKey.begin());
    CHECK_FALSE(VerifyNodeProof(handshake.caller.request, handshake.reply, rekeyed));

    // Replayed onto another connection: a different server nonce is a different transcript.
    auto const another = RunHandshake(0x40);
    CHECK_FALSE(VerifyNodeProof(another.caller.request, another.reply, handshake.proof));
    CHECK(VerifyNodeProof(another.caller.request, another.reply, another.proof));
}

TEST_CASE("Both ends of a handshake derive the same key for each direction, and the directions differ",
          "[distributed][nodeproof]")
{
    auto const handshake = RunHandshake();
    auto const caller = DeriveNodeSessionKeys(
        handshake.caller.secret, handshake.caller.request, handshake.reply, ProvingNode, /*callerSide=*/true);
    auto const server = DeriveNodeSessionKeys(
        handshake.server.secret, handshake.caller.request, handshake.reply, ProvingNode, /*callerSide=*/false);
    REQUIRE(caller.has_value());
    REQUIRE(server.has_value());

    CHECK(SameKey(Unwrap(caller).callerToServer, Unwrap(server).callerToServer));
    CHECK(SameKey(Unwrap(caller).serverToCaller, Unwrap(server).serverToCaller));
    // One key per direction: a sealer and an opener agree on ONE count, so a shared key would have
    // two senders claiming the same positions.
    CHECK_FALSE(SameKey(Unwrap(caller).callerToServer, Unwrap(caller).serverToCaller));

    // And the keys do the job: a frame the caller seals opens at the server.
    auto const header = std::array { std::byte { 0xFC }, std::byte { 0x0E }, std::byte { 0x01 } };
    auto const payload = std::array { std::byte { 0x2A } };
    FrameSealer sealer { Unwrap(caller).callerToServer };
    FrameOpener opener { Unwrap(server).callerToServer };
    CHECK(opener.Open(header, payload, sealer.Seal(header, payload)));
}

TEST_CASE("A session key is bound to the id the caller proved", "[distributed][nodeproof]")
{
    // The ids are in the derivation, so a server that recorded a different id from the one the
    // caller signed for agrees no key with it -- and the caller's first sealed frame fails.
    auto const handshake = RunHandshake();
    auto const caller = DeriveNodeSessionKeys(
        handshake.caller.secret, handshake.caller.request, handshake.reply, ProvingNode, /*callerSide=*/true);
    auto const server = DeriveNodeSessionKeys(
        handshake.server.secret, handshake.caller.request, handshake.reply, "node-9", /*callerSide=*/false);
    REQUIRE(caller.has_value());
    REQUIRE(server.has_value());
    CHECK_FALSE(SameKey(Unwrap(caller).callerToServer, Unwrap(server).callerToServer));
}

TEST_CASE("A handshake whose ephemeral key is low-order agrees no session", "[distributed][nodeproof]")
{
    // A low-order point fixes the "shared" secret to a value everybody knows, so a caller sending
    // one would be sealed under a key a relay can compute. Refused rather than derived.
    auto handshake = RunHandshake();
    auto lowOrder = handshake.caller.request;
    std::ranges::fill(lowOrder.ephemeral, std::byte { 0 });
    CHECK_FALSE(DeriveNodeSessionKeys(handshake.server.secret, lowOrder, handshake.reply, ProvingNode, false).has_value());
}

namespace
{
/// One sample of every input any identity-key construction's builder takes.
struct ConstructionSamples
{
    Ed25519PublicKey key {};           ///< The signer's public key.
    Nonce nonce {};                    ///< A challenge's nonce.
    Wire::FleetSummary summary;        ///< A fleet summary.
    std::array<std::byte, 3> field {}; ///< A field, standing in for packed claims or a transcript.
};

/// @param samples The samples.
/// @return A one-field transcript over the sample field; a view into @p samples.
[[nodiscard]] std::array<std::span<std::byte const>, 1> TranscriptOf(ConstructionSamples const& samples)
{
    return { std::span<std::byte const> { samples.field } };
}

/// One construction, and the production builder that makes its message.
struct ConstructionBuilder
{
    IdentityKeyPurpose purpose;                           ///< The construction.
    LabelledMessage (*build)(ConstructionSamples const&); ///< Its production builder, over the samples.
};

/// Every construction one identity key signs, each through its production message builder: one row
/// per `IdentityKeyPurpose`, in enumerator order.
///
/// Keyed on the enum `IdentityKeyLabels` is keyed on, so a construction added there without a row
/// here fails the BUILD, and every question below is asked of it. The Raft and node-proof rows go
/// through their protocols' OWN enums, so a mapping row pointing at the wrong construction is red.
constexpr EnumTable<IdentityKeyPurpose, ConstructionBuilder> ConstructionBuilders { {
    { .purpose = IdentityKeyPurpose::DiscoveryProof,
      .build =
          [](ConstructionSamples const& samples) {
              return Cluster::DiscoveryWire::ProofMessage(
                  { .clusterId = "c-asker", .nonce = samples.nonce }, samples.summary, samples.key);
          } },
    { .purpose = IdentityKeyPurpose::FleetSummary,
      .build =
          [](ConstructionSamples const& samples) {
              return Cluster::FleetSummaryMessage(samples.nonce, samples.summary, samples.key);
          } },
    { .purpose = IdentityKeyPurpose::RosterEndorsement,
      .build =
          [](ConstructionSamples const& /*samples*/) {
              return Cluster::EndorsementMessage(
                  Cluster::RosterEndorsement { .clusterId = "c-sample", .endorser = "n-sample" });
          } },
    { .purpose = IdentityKeyPurpose::Lease,
      .build = [](ConstructionSamples const& samples) { return Distributed::Detail::SignedLeaseMessage(samples.field); } },
    { .purpose = IdentityKeyPurpose::RaftDiallerProof,
      .build =
          [](ConstructionSamples const& samples) {
              auto const transcript = TranscriptOf(samples);
              return Consensus::RaftPeerSignedMessage(Consensus::RaftPeerSignature::DiallerProof,
                                                      WireFields::FieldList { transcript });
          } },
    { .purpose = IdentityKeyPurpose::RaftAcceptorVerdict,
      .build =
          [](ConstructionSamples const& samples) {
              auto const transcript = TranscriptOf(samples);
              return Consensus::RaftPeerSignedMessage(Consensus::RaftPeerSignature::AcceptorVerdict,
                                                      WireFields::FieldList { transcript });
          } },
    { .purpose = IdentityKeyPurpose::NodeServerChallenge,
      .build =
          [](ConstructionSamples const& samples) {
              auto const transcript = TranscriptOf(samples);
              return NodeProofSignedMessage(NodeProofSignature::ServerChallenge, transcript);
          } },
    { .purpose = IdentityKeyPurpose::NodeProof,
      .build =
          [](ConstructionSamples const& samples) {
              auto const transcript = TranscriptOf(samples);
              return NodeProofSignedMessage(NodeProofSignature::NodeProof, transcript);
          } },
    { .purpose = IdentityKeyPurpose::MachineTicket,
      .build =
          [](ConstructionSamples const& samples) {
              return MachineTicketMessage(MachineTicketClaims { .machineId = samples.summary.nodeId,
                                                                .audience = "office.corp:6674",
                                                                .expiresAtUnixSeconds = 1,
                                                                .nonce = {} });
          } },
    { .purpose = IdentityKeyPurpose::EnrollAdmission,
      .build =
          [](ConstructionSamples const& samples) {
              return Cluster::EnrollAdmissionMessage(
                  Cluster::AdmissionClaim { .nonce = samples.nonce,
                                            .joinerId = "n-joiner",
                                            .joinerKey = samples.key,
                                            .clusterId = "c-sample",
                                            .outcome = CompileCacheWire::EnrollOutcome::Approved,
                                            .roster = samples.field });
          } },
} };

static_assert(RowsInEnumeratorOrder(ConstructionBuilders, &ConstructionBuilder::purpose),
              "ConstructionBuilders must hold one row per IdentityKeyPurpose, in enumerator order");
} // namespace

TEST_CASE("Every identity-key construction's builder signs as its OWN construction, its label the FIRST field",
          "[distributed][nodeproof]")
{
    // A label is only a separation if it is IN the message, and first: FLEET-SUMMARY signs over a
    // nonce a STRANGER chose, so without its label leading, what separates that signature from every
    // other construction under the same key is that nobody happened to shape a 32-byte field there.
    // `LabelledMessage` makes "first" true by construction, so what each builder can still get wrong
    // is WHICH construction it names -- asked of each builder's own output, decoded.
    //
    // That no two constructions share a label, and none is retired, is a build failure over the one
    // table (`IdentityKeyLabelsSeparate`); restated here so a reader of this case sees it.
    STATIC_REQUIRE(IdentityKeyLabelsSeparate());

    auto const samples =
        ConstructionSamples { .key = TestKeyPair("signer").PublicKey(),
                              .nonce = {},
                              .summary = Wire::FleetSummary { .clusterId = "c-sample", .nodeId = "n-sample" },
                              .field = { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } } };
    for (auto const& row: ConstructionBuilders)
    {
        INFO(LabelOf(row.purpose));
        auto const message = row.build(samples);
        CHECK(message.Purpose() == row.purpose);
        auto const fields = WireFields::SplitAll(message.Bytes());
        REQUIRE(fields.has_value());
        REQUIRE_FALSE(Unwrap(fields).empty());
        CHECK(std::ranges::equal(Unwrap(fields).front(), WireFields::AsBytes(LabelOf(row.purpose))));
    }
}
