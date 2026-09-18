// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
/// A nonce whose every byte differs, so a truncation or a mis-offset shows.
[[nodiscard]] std::array<std::byte, 32> SampleNonce()
{
    std::array<std::byte, 32> nonce {};
    for (auto const index: std::views::iota(std::size_t { 0 }, nonce.size()))
        nonce[index] = static_cast<std::byte>(0xA0 + index);
    return nonce;
}

/// The key pair a seed of @p fill determines.
/// @param fill Every byte of the seed.
/// @return The pair.
[[nodiscard]] Ed25519KeyPair Pair(std::uint8_t fill)
{
    auto seed = std::array<std::byte, Ed25519SeedBytes> {};
    seed.fill(static_cast<std::byte>(fill));
    auto pair = Ed25519KeyPair::FromSeed(seed);
    REQUIRE(pair.has_value());
    return *std::move(pair);
}

/// An honest proof by @p pair for @p challenge.
/// @param pair Who signs.
/// @param challenge What was asked.
/// @param nodeId Who they say they are.
/// @param endpoint Where they say they answer.
/// @return The proof.
[[nodiscard]] DiscoveryWire::Proof Signed(Ed25519KeyPair const& pair,
                                          DiscoveryWire::Challenge const& challenge,
                                          std::string_view nodeId,
                                          std::string_view endpoint)
{
    return DiscoveryWire::Proof {
        .nodeId = std::string { nodeId },
        .raftEndpoint = std::string { endpoint },
        .publicKey = pair.PublicKey(),
        .signature = pair.Sign(DiscoveryWire::ProofMessage(challenge, nodeId, endpoint, pair.PublicKey())),
    };
}
} // namespace

TEST_CASE("DiscoveryWire round-trips every datagram kind", "[cluster][discovery][wire]")
{
    // Every field a DIFFERENT value, which is the discipline RaftWire records
    // having learned the hard way: its encoder arms are near-copies of one
    // another, and the mistake copying invites is a transposed field index --
    // which two fields sharing a value would let straight through.
    SECTION("beacon")
    {
        DiscoveryWire::Beacon const beacon { .clusterId = "prod-eu", .nodeId = "worker-a", .raftEndpoint = "10.0.0.5:7000" };
        auto const encoded = DiscoveryWire::EncodeBeacon(beacon);

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Beacon);
        auto const decoded = DiscoveryWire::DecodeBeacon(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).clusterId == "prod-eu");
        CHECK(Unwrap(decoded).nodeId == "worker-a");
        CHECK(Unwrap(decoded).raftEndpoint == "10.0.0.5:7000");
    }

    SECTION("challenge")
    {
        DiscoveryWire::Challenge const challenge { .clusterId = "prod-eu", .nonce = SampleNonce() };
        auto const encoded = DiscoveryWire::EncodeChallenge(challenge);

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Challenge);
        auto const decoded = DiscoveryWire::DecodeChallenge(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).clusterId == "prod-eu");
        CHECK(Unwrap(decoded).nonce == SampleNonce());
    }

    SECTION("proof")
    {
        auto const proof =
            Signed(Pair(0x31), { .clusterId = "prod-eu", .nonce = SampleNonce() }, "worker-a", "10.0.0.5:7000");
        auto const encoded = DiscoveryWire::EncodeProof(proof);

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Proof);
        auto const decoded = DiscoveryWire::DecodeProof(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).nodeId == "worker-a");
        CHECK(Unwrap(decoded).raftEndpoint == "10.0.0.5:7000");
        CHECK(Unwrap(decoded).publicKey == proof.publicKey);
        CHECK(Unwrap(decoded).signature == proof.signature);
    }
}

TEST_CASE("DiscoveryWire refuses what is not its datagram", "[cluster][discovery][wire]")
{
    // These land on a broadcast or multicast address that anything on the
    // segment may also use, so "not for me" has to be cheap and total.
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram({}).has_value());

    auto good = DiscoveryWire::EncodeBeacon({ .clusterId = "p", .nodeId = "n", .raftEndpoint = "e:1" });

    auto wrongMagic = good;
    wrongMagic[0] = std::byte { 0xFC }; // the compile cache's
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(wrongMagic).has_value());

    auto futureVersion = good;
    futureVersion[1] = std::byte { DiscoveryWire::CurrentVersion + 1 };
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(futureVersion).has_value());

    auto unknownKind = good;
    unknownKind[2] = std::byte { 0x7F };
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(unknownKind).has_value());

    // A datagram is all-or-nothing at the kernel, so a declared length that does
    // not match what arrived is a malformed sender rather than a short read --
    // and parsing on would read whatever followed in the buffer.
    auto truncated = good;
    truncated.pop_back();
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(truncated).has_value());

    auto padded = good;
    padded.push_back(std::byte { 0 });
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(padded).has_value());
}

TEST_CASE("DiscoveryWire will not decode one kind as another", "[cluster][discovery][wire]")
{
    // The kind byte is what keeps three near-identical payload shapes apart. A
    // decoder that trusted its caller would happily read a challenge's nonce as
    // a beacon's node id.
    auto const beacon = DiscoveryWire::EncodeBeacon({ .clusterId = "p", .nodeId = "n", .raftEndpoint = "e:1" });
    auto const challenge = DiscoveryWire::EncodeChallenge({ .clusterId = "p", .nonce = SampleNonce() });

    CHECK_FALSE(DiscoveryWire::DecodeChallenge(beacon).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeProof(beacon).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeBeacon(challenge).has_value());
}

TEST_CASE("Version 2 is a grammar change, so a version-1 datagram is refused rather than misread",
          "[cluster][discovery][wire]")
{
    // **Moved, and #402's did not** (#178). #402 changed only what the MAC covered and
    // rightly left the version alone; this changes the proof's ARITY and its field widths
    // -- a key and a 64-byte signature where a 32-byte MAC was -- so a version-1 reader
    // would refuse the proof as malformed and report a peer failing to prove a key it
    // holds. Pinned as values, since the question is always which of the two changed.
    CHECK(DiscoveryWire::CurrentVersion == 2);
    CHECK(DiscoveryWire::MinimumVersion == 2);

    auto good = DiscoveryWire::EncodeBeacon({ .clusterId = "p", .nodeId = "n", .raftEndpoint = "e:1" });
    REQUIRE(DiscoveryWire::ClassifyDatagram(good).has_value());
    auto older = good;
    older[1] = std::byte { 1 };
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(older).has_value());
}

TEST_CASE("A proof carries exactly one key and one signature", "[cluster][discovery][wire]")
{
    // A prefix of a key is a different key, and a truncated signature verifies as nothing,
    // so both are refused at the decoder and the verifier is never asked about them.
    auto const proof = Signed(Pair(0x31), { .clusterId = "p", .nonce = SampleNonce() }, "worker-a", "10.0.0.5:7000");
    auto const frame = [](std::vector<std::span<std::byte const>> const& fields) {
        auto const payload = WireFields::Encode(WireFields::FieldList { fields });
        return DiscoveryWire::Frame(DiscoveryWire::Kind::Proof, payload);
    };
    auto const id = WireFields::AsBytes(std::string_view { "worker-a" });
    auto const endpoint = WireFields::AsBytes(std::string_view { "10.0.0.5:7000" });
    auto const key = std::span<std::byte const> { proof.publicKey };
    auto const signature = std::span<std::byte const> { proof.signature };

    CHECK(DiscoveryWire::DecodeProof(frame({ id, endpoint, key, signature })).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeProof(frame({ id, endpoint, key.first(31), signature })).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeProof(frame({ id, endpoint, key, signature.first(63) })).has_value());

    // And the version-1 shape -- three fields, the last a 32-byte MAC -- is refused on its
    // arity, whatever its bytes say.
    CHECK_FALSE(DiscoveryWire::DecodeProof(frame({ id, endpoint, key })).has_value());
}

TEST_CASE("A proof signs the id, the endpoint, the nonce, the cluster and the key", "[cluster][discovery][wire]")
{
    // The security property of the handshake. Signing the nonce alone would let anyone
    // who observed one valid proof replay it with a DIFFERENT endpoint substituted --
    // admitting a known node id at an attacker's address, which is object injection into
    // every build the fleet serves.
    auto const pair = Pair(0x31);
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };
    auto const honest = Signed(pair, challenge, "worker-a", "10.0.0.5:7000");

    // The control first: an honest proof verifies, or every refusal below is vacuous.
    REQUIRE(DiscoveryWire::VerifyProofSignature(challenge, honest));

    auto moved = honest;
    moved.raftEndpoint = "10.0.0.9:7000";
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(challenge, moved));

    auto renamed = honest;
    renamed.nodeId = "worker-b";
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(challenge, renamed));

    // A different nonce is a different message, which is what makes a proof unreplayable
    // against a later challenge.
    DiscoveryWire::Challenge other = challenge;
    other.nonce[0] ^= std::byte { 0x01 };
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(other, honest));

    // And a different cluster, so one fleet's proof cannot answer another's challenge.
    DiscoveryWire::Challenge elsewhere = challenge;
    elsewhere.clusterId = "staging";
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(elsewhere, honest));

    // **The key is inside the message, not merely beside it.** Re-attributing a valid
    // signature to another key is refused -- here by the signature failing under that key,
    // and, because the key is also signed, even a key under which the bytes happened to
    // verify would be signing a message naming a DIFFERENT key.
    auto reattributed = honest;
    reattributed.publicKey = Pair(0x32).PublicKey();
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(challenge, reattributed));

    // A proof from another key, honestly made, verifies -- possession is all this answers.
    // Whether the cluster KNOWS that key is the roster's question, asked by the service.
    CHECK(DiscoveryWire::VerifyProofSignature(challenge, Signed(Pair(0x32), challenge, "worker-a", "10.0.0.5:7000")));
}

TEST_CASE("A proof is signed under its own label", "[cluster][discovery][wire]")
{
    // A label of its own, so a discovery signature can never verify as a Raft handshake
    // signature or the reverse. Asserted as the MESSAGE, which is what a signature covers:
    // the label, then the five fields.
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };
    auto const key = Pair(0x31).PublicKey();
    auto const message = DiscoveryWire::ProofMessage(challenge, "worker-a", "10.0.0.5:7000", key);

    auto const expected = WireFields::Encode({ WireFields::AsBytes(std::string_view { "fastcache-discovery-proof-v2" }),
                                               WireFields::AsBytes(challenge.clusterId),
                                               std::span<std::byte const> { challenge.nonce },
                                               WireFields::AsBytes(std::string_view { "worker-a" }),
                                               WireFields::AsBytes(std::string_view { "10.0.0.5:7000" }),
                                               std::span<std::byte const> { key } });
    CHECK(message == expected);

    // Spelled as a literal above rather than read from the constant, and the constant pinned
    // here: a label edited in one place moves every signature on the segment.
    CHECK(DiscoveryWire::ProofSignatureLabel == "fastcache-discovery-proof-v2");

    // And a signature over the same fields WITHOUT the label does not verify.
    auto const pair = Pair(0x31);
    auto const unlabelled = WireFields::Encode({ WireFields::AsBytes(challenge.clusterId),
                                                 std::span<std::byte const> { challenge.nonce },
                                                 WireFields::AsBytes(std::string_view { "worker-a" }),
                                                 WireFields::AsBytes(std::string_view { "10.0.0.5:7000" }),
                                                 std::span<std::byte const> { key } });
    auto forged = Signed(pair, challenge, "worker-a", "10.0.0.5:7000");
    forged.signature = pair.Sign(unlabelled);
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(challenge, forged));
}

TEST_CASE("A proof's fields are framed, not concatenated", "[cluster][discovery][wire]")
{
    // The lesson the object key already records: a separator that can occur inside a value
    // is not a framing. Without length prefixes these two would sign identically, so a node
    // could prove one identity and be believed as another.
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };
    auto const key = Pair(0x31).PublicKey();
    CHECK(DiscoveryWire::ProofMessage(challenge, "worker", "a:7000", key)
          != DiscoveryWire::ProofMessage(challenge, "workera", ":7000", key));
}

TEST_CASE("A beacon carries nothing an eavesdropper can use", "[cluster][discovery][wire]")
{
    // A beacon is an invitation to ASK, not a credential: the challenge that follows it
    // is what proves anything, and only to the node that chose its nonce.
    auto const encoded =
        DiscoveryWire::EncodeBeacon({ .clusterId = "prod", .nodeId = "worker-a", .raftEndpoint = "10.0.0.5:7000" });

    // Everything in it is exactly what was put there, and nothing else: three
    // fields, and a size that accounts for all of them.
    auto const decoded = DiscoveryWire::DecodeBeacon(encoded);
    REQUIRE(decoded.has_value());
    auto const reencoded = DiscoveryWire::EncodeBeacon(Unwrap(decoded));
    CHECK(std::ranges::equal(encoded, reencoded));
}
