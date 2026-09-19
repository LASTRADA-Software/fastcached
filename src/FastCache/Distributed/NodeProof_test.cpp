// SPDX-License-Identifier: Apache-2.0
//
// The node identity handshake's pure half (#178): who signs what, over which transcript, and the
// keys both ends derive from it. Where it runs on a connection is `NodeProofResponder_test` and
// `FrameEndpoint_test`; where the keys seal frames is `SealedFrameSocket_test`.
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
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

TEST_CASE("The handshake's two signatures are signed under distinct labels, and neither is a retired one",
          "[distributed][nodeproof]")
{
    // A label is what keeps a signature made for one purpose from verifying as another, so two
    // rows sharing one would make a server's signature a caller's proof. A retired label is the
    // pre-shared key's MAC input, and reusing it would let a MAC and a signed message be the same
    // bytes.
    for (auto const& row: NodeProofSignatureLabels)
    {
        CHECK(std::ranges::count(NodeProofSignatureLabels, row.label, &NodeProofSignatureLabel::label) == 1);
        CHECK_FALSE(std::ranges::contains(RetiredNodeProofLabels, row.label));
    }
}

TEST_CASE("Every construction one identity key signs under names a label no other construction uses",
          "[distributed][nodeproof]")
{
    // A scheduler signs all of these with ONE key -- its node identity key -- so a label is the only
    // thing keeping its lease from verifying as its Raft verdict or its challenge reply as its
    // roster endorsement. Each construction checks its OWN labels apart; nothing else asked the
    // question across them once #178 deleted the pre-shared key's `SigningDomainTable`, whose
    // `static_assert` asked it of every MAC under that key.
    std::vector<std::string_view> labels { Cluster::DiscoveryWire::ProofSignatureLabel,
                                           Cluster::RosterEndorsementLabel,
                                           Distributed::LeaseSignatureLabel };
    for (auto const& row: Consensus::RaftPeerSignatureLabels)
        labels.push_back(row.label);
    for (auto const& row: NodeProofSignatureLabels)
        labels.push_back(row.label);

    for (auto const label: labels)
    {
        INFO(label);
        CHECK_FALSE(label.empty());
        CHECK(std::ranges::count(labels, label) == 1);
        CHECK_FALSE(std::ranges::contains(RetiredNodeProofLabels, label));
    }
}
