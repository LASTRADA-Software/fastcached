// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

/// The bytes of @p text.
[[nodiscard]] std::span<std::byte const> Bytes(std::string_view text)
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
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
        auto tag = Sha256::Hash(Bytes("anything"));
        DiscoveryWire::Proof const proof { .nodeId = "worker-a", .raftEndpoint = "10.0.0.5:7000", .tag = tag };
        auto const encoded = DiscoveryWire::EncodeProof(proof);

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Proof);
        auto const decoded = DiscoveryWire::DecodeProof(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).nodeId == "worker-a");
        CHECK(Unwrap(decoded).raftEndpoint == "10.0.0.5:7000");
        CHECK(Unwrap(decoded).tag == tag);
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

TEST_CASE("A proof authenticates the endpoint, not just the nonce", "[cluster][discovery][wire]")
{
    // The security property of the handshake. Authenticating the nonce alone
    // would let anyone who observed one valid proof replay its tag with a
    // DIFFERENT endpoint substituted -- admitting a legitimate node id at an
    // attacker's address, which is object injection into every build the fleet
    // serves.
    auto const key = Bytes("shared-secret");
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };

    auto const honest = DiscoveryWire::ExpectedProofTag(key, challenge, "worker-a", "10.0.0.5:7000");

    CHECK_FALSE(ConstantTimeEquals(honest, DiscoveryWire::ExpectedProofTag(key, challenge, "worker-a", "10.0.0.9:7000")));
    CHECK_FALSE(ConstantTimeEquals(honest, DiscoveryWire::ExpectedProofTag(key, challenge, "worker-b", "10.0.0.5:7000")));

    // A different nonce is a different tag, which is what makes a proof
    // unreplayable against a later challenge.
    DiscoveryWire::Challenge other = challenge;
    other.nonce[0] ^= std::byte { 0x01 };
    CHECK_FALSE(ConstantTimeEquals(honest, DiscoveryWire::ExpectedProofTag(key, other, "worker-a", "10.0.0.5:7000")));

    // And a different cluster, so one fleet's proof cannot admit a node to another.
    DiscoveryWire::Challenge elsewhere = challenge;
    elsewhere.clusterId = "staging";
    CHECK_FALSE(ConstantTimeEquals(honest, DiscoveryWire::ExpectedProofTag(key, elsewhere, "worker-a", "10.0.0.5:7000")));

    // The key is what it all rests on.
    CHECK_FALSE(ConstantTimeEquals(
        honest, DiscoveryWire::ExpectedProofTag(Bytes("other-secret"), challenge, "worker-a", "10.0.0.5:7000")));

    // Same inputs, same tag -- otherwise nobody could ever join.
    CHECK(ConstantTimeEquals(honest, DiscoveryWire::ExpectedProofTag(key, challenge, "worker-a", "10.0.0.5:7000")));
}

TEST_CASE("A proof is signed in its own domain, which the unlabelled message was not",
          "[cluster][discovery][wire]")
{
    // The wire change #402 makes, asserted rather than described. Until then a
    // proof's message was the four fields alone, with no domain label -- so what
    // kept it from colliding with a lease tag under the SAME pre-shared key was
    // that the two happened to have different field arities. That is a property of
    // the pair, not of either construction, and it survives exactly as long as
    // nobody adds a field to discovery.
    auto const key = Bytes("shared-secret");
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };

    auto const fields = std::array<std::span<std::byte const>, 4> { WireFields::AsBytes(challenge.clusterId),
                                                                   std::span<std::byte const> { challenge.nonce },
                                                                   WireFields::AsBytes(std::string_view { "worker-a" }),
                                                                   WireFields::AsBytes(
                                                                       std::string_view { "10.0.0.5:7000" }) };

    auto const tag = DiscoveryWire::ExpectedProofTag(key, challenge, "worker-a", "10.0.0.5:7000");

    // What it IS now: the label, then those four fields.
    CHECK(ConstantTimeEquals(
        tag,
        HmacSha256(key,
                   WireFields::Encode({ WireFields::AsBytes(DescribeSigningDomain(SigningDomain::DiscoveryProof).label),
                                        fields[0],
                                        fields[1],
                                        fields[2],
                                        fields[3] }))));

    // What it WAS: the four fields alone. A node on an older build therefore fails
    // to prove the key and is logged saying so, rather than being silently ignored
    // -- the datagram grammar and its version are untouched.
    CHECK_FALSE(ConstantTimeEquals(
        tag, HmacSha256(key, WireFields::Encode({ fields[0], fields[1], fields[2], fields[3] }))));

    // And the same four fields signed as a LEASE are a different tag, which is the
    // separation the label buys and which arity alone was standing in for.
    CHECK_FALSE(ConstantTimeEquals(tag, SignFields(key, SigningDomain::LeaseToken, fields)));
}

TEST_CASE("A proof's fields are framed, not concatenated", "[cluster][discovery][wire]")
{
    // The lesson the object key already records: a separator that can occur
    // inside a value is not a framing. Without length prefixes these two would
    // authenticate identically, so a node could prove one identity and be
    // admitted as another.
    auto const key = Bytes("shared-secret");
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };

    auto const split = DiscoveryWire::ExpectedProofTag(key, challenge, "worker", "a:7000");
    auto const shifted = DiscoveryWire::ExpectedProofTag(key, challenge, "workera", ":7000");
    CHECK_FALSE(ConstantTimeEquals(split, shifted));
}

TEST_CASE("A beacon carries nothing an eavesdropper can use", "[cluster][discovery][wire]")
{
    // A beacon is an invitation to ASK, not a credential. If it carried the key
    // or anything derived from it, discovery would hand every listener on the
    // segment what it needs to join -- which is the whole reason the pre-shared
    // key authenticates a handshake instead of travelling in broadcasts.
    auto const encoded =
        DiscoveryWire::EncodeBeacon({ .clusterId = "prod", .nodeId = "worker-a", .raftEndpoint = "10.0.0.5:7000" });

    std::string const asText { reinterpret_cast<char const*>(encoded.data()), encoded.size() };
    CHECK_FALSE(asText.contains("shared-secret"));

    // Everything in it is exactly what was put there, and nothing else: three
    // fields, and a size that accounts for all of them.
    auto const decoded = DiscoveryWire::DecodeBeacon(encoded);
    REQUIRE(decoded.has_value());
    auto const reencoded = DiscoveryWire::EncodeBeacon(Unwrap(decoded));
    CHECK(std::ranges::equal(encoded, reencoded));
}
