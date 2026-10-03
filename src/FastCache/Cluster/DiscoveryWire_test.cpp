// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
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

/// An established fleet's summary, every field a different value, so a transposed field index shows.
/// @return The summary.
[[nodiscard]] CompileCacheWire::FleetSummary Established()
{
    return CompileCacheWire::FleetSummary { .clusterId = "c-office",
                                            .state = CompileCacheWire::FleetState::Established,
                                            .createdAtUnixSeconds = 1'790'000'000,
                                            .leaderId = "n-a",
                                            .leaderNodeEndpoint = "office-a:6674",
                                            .nodeId = "n-b",
                                            .raftEndpoint = "",
                                            .members = { "n-a", "n-b" },
                                            .memberTotal = 3,
                                            .nodeEndpoint = "office-b:6674" };
}

/// A solitary node's summary naming @p nodeId at @p endpoint, as a node that knows no leader says it.
/// @param nodeId Who is speaking.
/// @param endpoint Where they answer Raft.
/// @return The summary.
[[nodiscard]] CompileCacheWire::FleetSummary Speaking(std::string_view nodeId, std::string_view endpoint)
{
    return CompileCacheWire::FleetSummary { .clusterId = "prod",
                                            .state = CompileCacheWire::FleetState::Solitary,
                                            .createdAtUnixSeconds = 0,
                                            .leaderId = "",
                                            .leaderNodeEndpoint = "",
                                            .nodeId = std::string { nodeId },
                                            .raftEndpoint = std::string { endpoint } };
}

/// An honest proof by @p pair for @p challenge.
/// @param pair Who signs.
/// @param challenge What was asked.
/// @param summary What they say about their fleet and themselves.
/// @return The proof.
[[nodiscard]] DiscoveryWire::Proof Signed(Ed25519KeyPair const& pair,
                                          DiscoveryWire::Challenge const& challenge,
                                          CompileCacheWire::FleetSummary const& summary)
{
    return DiscoveryWire::Proof {
        .summary = summary,
        .answers = challenge.nonce,
        .publicKey = pair.PublicKey(),
        .signature = SignLabelled(pair, DiscoveryWire::ProofMessage(challenge, summary, pair.PublicKey())),
    };
}

/// Frame @p fields as a proof datagram, whatever they hold.
/// @param fields The payload's fields.
/// @return The datagram.
[[nodiscard]] std::vector<std::byte> ProofOf(std::vector<std::span<std::byte const>> const& fields)
{
    return DiscoveryWire::Frame(DiscoveryWire::Kind::Proof, WireFields::Encode(WireFields::FieldList { fields }));
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
        auto const encoded = DiscoveryWire::EncodeBeacon({ .summary = Established() });

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Beacon);
        auto const decoded = DiscoveryWire::DecodeBeacon(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).summary == Established());
    }

    SECTION("challenge")
    {
        DiscoveryWire::Challenge const challenge { .clusterId = "prod-eu", .nonce = SampleNonce() };
        auto const encoded = Unwrap(
            DiscoveryWire::EncodeChallenge(challenge, DiscoveryWire::EncodeBeacon({ .summary = Established() }).size()));

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Challenge);
        auto const decoded = DiscoveryWire::DecodeChallenge(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).clusterId == "prod-eu");
        CHECK(Unwrap(decoded).nonce == SampleNonce());
    }

    SECTION("proof")
    {
        auto const proof = Signed(Pair(0x31), { .clusterId = "prod-eu", .nonce = SampleNonce() }, Established());
        auto const encoded = DiscoveryWire::EncodeProof(proof);

        REQUIRE(DiscoveryWire::ClassifyDatagram(encoded) == DiscoveryWire::Kind::Proof);
        auto const decoded = DiscoveryWire::DecodeProof(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).summary == Established());
        CHECK(Unwrap(decoded).answers == SampleNonce());
        CHECK(Unwrap(decoded).publicKey == proof.publicKey);
        CHECK(Unwrap(decoded).signature == proof.signature);
    }
}

TEST_CASE("No discovery reply is larger than the datagram that provoked it", "[cluster][discovery][wire][security]")
{
    // A reply goes to whatever address its request came FROM, which its sender typed. So a beacon
    // is padded to the proof its sender answers with, a challenge to exactly the beacon it answers,
    // and the proof then fits the challenge: at no step is a spoofed datagram an amplifier.
    auto const challenge = DiscoveryWire::Challenge { .clusterId = "prod-eu", .nonce = SampleNonce() };
    for (auto const& summary: { Established(), Speaking("n", "e:1") })
    {
        auto const beacon = DiscoveryWire::EncodeBeacon({ .summary = summary });
        auto const proof = DiscoveryWire::EncodeProof(Signed(Pair(0x31), challenge, summary));
        CHECK(beacon.size() == DiscoveryWire::ProofDatagramSize(summary));
        CHECK(proof.size() == DiscoveryWire::ProofDatagramSize(summary));

        auto const asked = DiscoveryWire::EncodeChallenge(challenge, beacon.size());
        REQUIRE(asked.has_value());
        CHECK(Unwrap(asked).size() == beacon.size());
        CHECK(DiscoveryWire::AnswerFits(Unwrap(asked).size(), beacon.size()));
        CHECK(DiscoveryWire::AnswerFits(proof.size(), Unwrap(asked).size()));
    }
}

TEST_CASE("A challenge that cannot fit the beacon it answers is not encoded", "[cluster][discovery][wire][security]")
{
    // Padding only ever grows a challenge, so one whose own fields are larger than the beacon it
    // answers cannot be made to fit, and is not sent at all rather than sent larger.
    auto const challenge = DiscoveryWire::Challenge { .clusterId = "prod-eu", .nonce = SampleNonce() };
    auto const unpadded = WireFrame::HeaderSize
                          + WireFields::Encode({ WireFields::AsBytes(challenge.clusterId),
                                                 std::span<std::byte const> { challenge.nonce },
                                                 std::span<std::byte const> {} })
                                .size();
    CHECK_FALSE(DiscoveryWire::EncodeChallenge(challenge, unpadded - 1).has_value());
    auto const exact = DiscoveryWire::EncodeChallenge(challenge, unpadded);
    REQUIRE(exact.has_value());
    CHECK(Unwrap(exact).size() == unpadded);
    CHECK(Unwrap(DiscoveryWire::DecodeChallenge(Unwrap(exact))).nonce == SampleNonce());
}

TEST_CASE("Padding is never read, and a datagram without it is refused", "[cluster][discovery][wire]")
{
    // The trailing field is only there to be counted: what it holds decodes to the same datagram.
    // And it is part of the grammar, so the shape without it -- the one before padding -- is
    // refused by arity rather than read.
    auto const summary = CompileCacheWire::EncodeFleetSummaryFields(Established());
    auto const nested = std::span<std::byte const> { summary };
    auto const junk = std::vector<std::byte>(40, std::byte { 0xA5 });
    auto const beacon = DiscoveryWire::Frame(DiscoveryWire::Kind::Beacon,
                                             WireFields::Encode({ nested, std::span<std::byte const> { junk } }));
    CHECK(Unwrap(DiscoveryWire::DecodeBeacon(beacon)).summary == Established());
    CHECK_FALSE(
        DiscoveryWire::DecodeBeacon(DiscoveryWire::Frame(DiscoveryWire::Kind::Beacon, WireFields::Encode({ nested })))
            .has_value());

    auto const nonce = SampleNonce();
    auto const cluster = WireFields::AsBytes(std::string_view { "prod-eu" });
    auto const challenge = DiscoveryWire::Frame(
        DiscoveryWire::Kind::Challenge,
        WireFields::Encode({ cluster, std::span<std::byte const> { nonce }, std::span<std::byte const> { junk } }));
    CHECK(Unwrap(DiscoveryWire::DecodeChallenge(challenge)).nonce == nonce);
    CHECK_FALSE(DiscoveryWire::DecodeChallenge(
                    DiscoveryWire::Frame(DiscoveryWire::Kind::Challenge,
                                         WireFields::Encode({ cluster, std::span<std::byte const> { nonce } })))
                    .has_value());
}

TEST_CASE("A beacon carries the whole fleet summary", "[cluster][discovery][wire][formation]")
{
    auto const beacon = DiscoveryWire::Beacon { .summary = Established() };
    auto const decoded = DiscoveryWire::DecodeBeacon(DiscoveryWire::EncodeBeacon(beacon));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).summary == Established());
}

TEST_CASE("A beacon or a proof whose summary the codec refuses is refused", "[cluster][discovery][wire][formation]")
{
    // The summary is one nested field, and its codec is the only reader of it: a state byte this
    // build has no name for is refused there, and must be refused here too rather than read as a
    // beacon whose state is whatever the default says.
    auto summary = CompileCacheWire::EncodeFleetSummaryFields(Established());
    auto const fields = WireFields::SplitExactly(summary, CompileCacheWire::FleetSummaryFieldCount);
    REQUIRE(fields.has_value());
    auto const stateOffset = static_cast<std::size_t>(Unwrap(fields)[1].data() - summary.data());
    summary[stateOffset] = std::byte { 0x7F };
    REQUIRE_FALSE(CompileCacheWire::DecodeFleetSummaryFields(summary, CompileCacheWire::MaxFleetSummaryMembers).has_value());

    // In the beacon's own shape -- the summary, then padding -- so the refusal is the state byte's
    // and not the arity's; the honest summary in the same shape is the control.
    auto const beaconOf = [](std::span<std::byte const> nested) {
        return DiscoveryWire::Frame(DiscoveryWire::Kind::Beacon,
                                    WireFields::Encode({ nested, std::span<std::byte const> {} }));
    };
    auto const honestBeacon = CompileCacheWire::EncodeFleetSummaryFields(Established());
    CHECK(DiscoveryWire::DecodeBeacon(beaconOf(honestBeacon)).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeBeacon(beaconOf(summary)).has_value());

    // The control: the same proof with its honest summary decodes, so the refusal is the state byte's.
    auto const proof = Signed(Pair(0x31), { .clusterId = "p", .nonce = SampleNonce() }, Established());
    auto const honest = CompileCacheWire::EncodeFleetSummaryFields(proof.summary);
    auto const answers = std::span<std::byte const> { proof.answers };
    auto const key = std::span<std::byte const> { proof.publicKey };
    auto const signature = std::span<std::byte const> { proof.signature };
    CHECK(
        DiscoveryWire::DecodeProof(ProofOf({ std::span<std::byte const> { honest }, answers, key, signature })).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeProof(ProofOf({ std::span<std::byte const> { summary }, answers, key, signature }))
                    .has_value());
}

TEST_CASE("A cluster id past its bound is refused where it enters, and an honest challenge always fits",
          "[cluster][discovery][wire][formation]")
{
    // The bound is what lets a challenge always fit the beacon it answers, so the sizes the
    // assertion reasons about are checked against the encoders that produce them.
    auto minimal = Speaking("", "");
    minimal.clusterId = "c";
    minimal.leaderId = {};
    minimal.leaderNodeEndpoint = {};
    CHECK(DiscoveryWire::ProofDatagramSize(minimal) == DiscoveryWire::SmallestProofDatagram());
    auto const longest = std::string(CompileCacheWire::MaxIdBytes, 'c');
    auto const nonce = SampleNonce();
    auto const unpadded = DiscoveryWire::Frame(
        DiscoveryWire::Kind::Challenge,
        WireFields::Encode(
            { WireFields::AsBytes(longest), std::span<std::byte const> { nonce }, std::span<std::byte const> {} }));
    CHECK(unpadded.size() == DiscoveryWire::LargestUnpaddedChallenge());
    CHECK(DiscoveryWire::EncodeChallenge({ .clusterId = longest, .nonce = nonce }, DiscoveryWire::SmallestProofDatagram())
              .has_value());

    // At the bound a beacon and a challenge decode; one byte past it, neither does.
    auto const beaconNaming = [](std::size_t bytes) {
        auto summary = Established();
        summary.clusterId = std::string(bytes, 'c');
        return DiscoveryWire::EncodeBeacon({ .summary = summary });
    };
    CHECK(DiscoveryWire::DecodeBeacon(beaconNaming(CompileCacheWire::MaxIdBytes)).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeBeacon(beaconNaming(CompileCacheWire::MaxIdBytes + 1)).has_value());

    auto const challengeNaming = [&nonce](std::size_t bytes) {
        auto const cluster = std::string(bytes, 'c');
        return DiscoveryWire::Frame(
            DiscoveryWire::Kind::Challenge,
            WireFields::Encode(
                { WireFields::AsBytes(cluster), std::span<std::byte const> { nonce }, std::span<std::byte const> {} }));
    };
    CHECK(DiscoveryWire::DecodeChallenge(challengeNaming(CompileCacheWire::MaxIdBytes)).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeChallenge(challengeNaming(CompileCacheWire::MaxIdBytes + 1)).has_value());
}

TEST_CASE("A proof naming as many members as a datagram carries fits the tightest unfragmented UDP payload",
          "[cluster][discovery][wire][formation]")
{
    // A PINNED MEASUREMENT, and its conditions live here rather than beside the constant:
    //   - every id at `MaxIdBytes` (64 bytes): the cluster, the leader, the speaker and each member;
    //   - each of the three dialled endpoints at the longest IPv4 spelling, 21 bytes;
    //   - the leader's identity key present, 32 bytes -- the field that took the bound from 11 to 10;
    //   - the budget is 1232 bytes of UDP payload: IPv6's 1280-byte minimum link MTU, less its
    //     40-byte header and UDP's 8. Discovery runs over IPv4 directed broadcast, whose budgets --
    //     1472 on Ethernet, 1392 behind a 1420-byte tunnel -- are all larger, so this is the
    //     tightest a datagram meets.
    // Endpoints are bounded only by `MaxFleetSummaryTextBytes`, so this is a measurement under
    // these conditions and not a ceiling; `AnswerFits` still refuses to amplify a longer one.
    constexpr std::size_t Ipv6MinimumMtuUdpPayload = 1280 - 40 - 8;
    auto const id = [](std::string_view prefix) {
        auto text = std::string { prefix };
        text.resize(CompileCacheWire::MaxIdBytes, 'x');
        return text;
    };
    auto const endpoint = std::string { "255.255.255.255:65535" };
    auto worst =
        CompileCacheWire::FleetSummary { .clusterId = id("c"),
                                         .state = CompileCacheWire::FleetState::Established,
                                         .createdAtUnixSeconds = 1'790'000'000,
                                         .leaderId = id("l"),
                                         .leaderNodeEndpoint = endpoint,
                                         .nodeId = id("n"),
                                         .raftEndpoint = endpoint,
                                         .members = {},
                                         .memberTotal = 1000,
                                         .nodeEndpoint = endpoint,
                                         .leaderKey = std::array<std::byte, CompileCacheWire::IdentityPublicKeyBytes> {},
                                         .pointsAt = {} };
    for (auto const index: std::views::iota(std::size_t { 0 }, CompileCacheWire::MaxFleetSummaryMembers))
        worst.members.push_back(id(std::format("m{}", index)));

    auto const size = DiscoveryWire::ProofDatagramSize(worst);
    CHECK(size == 1179); // the measurement, pinned
    CHECK(size <= Ipv6MinimumMtuUdpPayload);
    // And the bound is the largest that fits: one more member is one more prefixed id.
    CHECK(size + WireFields::FieldPrefixSize + CompileCacheWire::MaxIdBytes > Ipv6MinimumMtuUdpPayload);
}

TEST_CASE("A datagram naming more members than a datagram carries is refused, and a reply reads the same summary",
          "[cluster][discovery][wire][formation]")
{
    // Framed by hand, because the encoders refuse to write a list past the cap (`WithMembersAtMost`
    // is where a sender cuts it); what is tested is that a reader refuses one a peer sent anyway.
    auto const listing = [](std::size_t count) {
        auto summary = Established();
        summary.members.clear();
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            summary.members.push_back(std::format("n-{}", index));
        summary.memberTotal = count;
        return CompileCacheWire::EncodeFleetSummaryFields(summary);
    };
    auto const beaconOf = [](std::span<std::byte const> nested) {
        return DiscoveryWire::Frame(DiscoveryWire::Kind::Beacon,
                                    WireFields::Encode({ nested, std::span<std::byte const> {} }));
    };
    auto const atCap = listing(CompileCacheWire::MaxFleetSummaryMembers);
    auto const pastCap = listing(CompileCacheWire::MaxFleetSummaryMembers + 1);
    CHECK(DiscoveryWire::DecodeBeacon(beaconOf(atCap)).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeBeacon(beaconOf(pastCap)).has_value());
    CHECK(CompileCacheWire::DecodeFleetSummaryFields(pastCap, CompileCacheWire::MaxFleetSummaryReplyMembers).has_value());
}

TEST_CASE("DiscoveryWire refuses what is not its datagram", "[cluster][discovery][wire]")
{
    // These land on a broadcast or multicast address that anything on the
    // segment may also use, so "not for me" has to be cheap and total.
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram({}).has_value());

    auto good = DiscoveryWire::EncodeBeacon({ .summary = Speaking("n", "e:1") });

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
    // a beacon's summary.
    auto const beacon = DiscoveryWire::EncodeBeacon({ .summary = Speaking("n", "e:1") });
    auto const challenge =
        Unwrap(DiscoveryWire::EncodeChallenge({ .clusterId = "p", .nonce = SampleNonce() }, beacon.size()));

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

    auto good = DiscoveryWire::EncodeBeacon({ .summary = Speaking("n", "e:1") });
    REQUIRE(DiscoveryWire::ClassifyDatagram(good).has_value());
    auto older = good;
    older[1] = std::byte { 1 };
    CHECK_FALSE(DiscoveryWire::ClassifyDatagram(older).has_value());
}

TEST_CASE("A proof carries exactly one summary, one nonce, one key and one signature", "[cluster][discovery][wire]")
{
    // A truncated signature verifies as nothing, and a truncated nonce names no challenge, so each
    // is refused at the decoder and neither the cookie nor the verifier is ever asked about it.
    auto const proof = Signed(Pair(0x31), { .clusterId = "p", .nonce = SampleNonce() }, Established());
    auto const summary = CompileCacheWire::EncodeFleetSummaryFields(proof.summary);
    auto const blob = std::span<std::byte const> { summary };
    auto const answers = std::span<std::byte const> { proof.answers };
    auto const key = std::span<std::byte const> { proof.publicKey };
    auto const signature = std::span<std::byte const> { proof.signature };

    CHECK(DiscoveryWire::DecodeProof(ProofOf({ blob, answers, key, signature })).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeProof(ProofOf({ blob, answers, key, signature.first(63) })).has_value());
    CHECK_FALSE(DiscoveryWire::DecodeProof(ProofOf({ blob, answers.first(NonceBytes - 1), key, signature })).has_value());

    // The previous grammar -- no nonce, three fields -- is refused on its arity, whatever its
    // bytes say; and so is the one before it, an id and an endpoint where the summary is.
    CHECK_FALSE(DiscoveryWire::DecodeProof(ProofOf({ blob, key, signature })).has_value());
    auto const id = WireFields::AsBytes(std::string_view { "n-b" });
    auto const endpoint = WireFields::AsBytes(std::string_view { "10.0.0.5:7000" });
    CHECK_FALSE(DiscoveryWire::DecodeProof(ProofOf({ id, endpoint, key, signature })).has_value());
}

TEST_CASE("A proof whose key is one byte short is refused before anything verifies", "[cluster][discovery][wire][formation]")
{
    auto const key = Testing::TestKeyPair("n-b");
    auto const proof =
        DiscoveryWire::Proof { .summary = Established(), .answers = {}, .publicKey = key.PublicKey(), .signature = {} };
    auto const datagram = DiscoveryWire::EncodeProof(proof);
    REQUIRE(DiscoveryWire::DecodeProof(datagram).has_value());

    auto const payload = std::span<std::byte const> { datagram }.subspan(WireFrame::HeaderSize);
    auto const fields = WireFields::SplitExactly(payload, 4);
    REQUIRE(fields.has_value());
    auto const shortKey = Unwrap(fields)[2].first(Ed25519PublicKeyBytes - 1);
    auto const reframed =
        DiscoveryWire::Frame(DiscoveryWire::Kind::Proof,
                             WireFields::Encode({ Unwrap(fields)[0], Unwrap(fields)[1], shortKey, Unwrap(fields)[3] }));
    CHECK_FALSE(DiscoveryWire::DecodeProof(reframed).has_value());
}

TEST_CASE("A proof signs every summary field so changing any one is refused", "[cluster][discovery][wire][formation]")
{
    // The summary is what formation decides on -- whether to yield, to whom, where to enroll --
    // so a field outside the signature is one a relay could rewrite into a yield.
    auto const key = Testing::TestKeyPair("n-b");
    auto const challenge = DiscoveryWire::Challenge { .clusterId = "c-joiner", .nonce = {} };
    auto proof =
        DiscoveryWire::Proof { .summary = Established(), .answers = {}, .publicKey = key.PublicKey(), .signature = {} };
    proof.signature = SignLabelled(key, DiscoveryWire::ProofMessage(challenge, proof.summary, proof.publicKey));
    REQUIRE(DiscoveryWire::VerifyProofSignature(challenge, proof));

    // One mutation per field, as a table: dropping a field from what is signed turns its row red.
    using Summary = CompileCacheWire::FleetSummary;
    auto const mutations = std::array<std::pair<std::string_view, void (*)(Summary&)>, 10> { {
        { "clusterId", [](Summary& s) { s.clusterId += "x"; } },
        { "state", [](Summary& s) { s.state = CompileCacheWire::FleetState::Solitary; } },
        { "createdAt", [](Summary& s) { ++s.createdAtUnixSeconds; } },
        { "leaderId", [](Summary& s) { s.leaderId += "x"; } },
        { "leaderNodeEndpoint", [](Summary& s) { s.leaderNodeEndpoint += "0"; } },
        { "nodeId", [](Summary& s) { s.nodeId += "x"; } },
        { "raftEndpoint", [](Summary& s) { s.raftEndpoint = "attacker:6680"; } },
        { "members", [](Summary& s) { s.members.front() += "x"; } },
        { "memberTotal", [](Summary& s) { ++s.memberTotal; } },
        { "nodeEndpoint", [](Summary& s) { s.nodeEndpoint = "attacker:6674"; } },
    } };
    for (auto const& [name, mutate]: mutations)
    {
        INFO(name);
        auto changed = proof;
        mutate(changed.summary);
        CHECK_FALSE(DiscoveryWire::VerifyProofSignature(challenge, changed));
    }
}

TEST_CASE("A proof signs the nonce, the asking cluster and the key", "[cluster][discovery][wire]")
{
    // The summary's own fields are the case above. These are the rest of the message: what makes
    // a proof an answer to ONE question, and attributable to ONE key.
    auto const pair = Pair(0x31);
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };
    auto const honest = Signed(pair, challenge, Speaking("worker-a", "10.0.0.5:7000"));

    // The control first: an honest proof verifies, or every refusal below is vacuous.
    REQUIRE(DiscoveryWire::VerifyProofSignature(challenge, honest));

    // A different nonce is a different message, which is what makes a proof unreplayable
    // against a later challenge.
    DiscoveryWire::Challenge other = challenge;
    other.nonce[0] ^= std::byte { 0x01 };
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(other, honest));

    // And a different asking cluster, so a proof made for one asker cannot answer another's.
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
    CHECK(DiscoveryWire::VerifyProofSignature(challenge,
                                              Signed(Pair(0x32), challenge, Speaking("worker-a", "10.0.0.5:7000"))));
}

TEST_CASE("A proof is signed under its own label", "[cluster][discovery][wire]")
{
    // A label of its own, so a discovery signature can never verify as a Raft handshake
    // signature or the reverse. Asserted as the MESSAGE, which is what a signature covers:
    // the label, the asking cluster, the nonce, the summary as ONE nested field, the key.
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };
    auto const key = Pair(0x31).PublicKey();
    auto const summary = CompileCacheWire::EncodeFleetSummaryFields(Established());
    auto const message = DiscoveryWire::ProofMessage(challenge, Established(), key);

    // Spelled as a literal rather than read from the constant: a label edited in one place moves
    // every signature on the segment.
    auto const expected = WireFields::Encode({ WireFields::AsBytes(std::string_view { "fastcache-discovery-proof-v3" }),
                                               WireFields::AsBytes(challenge.clusterId),
                                               std::span<std::byte const> { challenge.nonce },
                                               std::span<std::byte const> { summary },
                                               std::span<std::byte const> { key } });
    CHECK(std::ranges::equal(message.Bytes(), expected));

    // And a signature over the same fields WITHOUT the label does not verify.
    auto const pair = Pair(0x31);
    auto const unlabelled = WireFields::Encode({ WireFields::AsBytes(challenge.clusterId),
                                                 std::span<std::byte const> { challenge.nonce },
                                                 std::span<std::byte const> { summary },
                                                 std::span<std::byte const> { key } });
    auto forged = Signed(pair, challenge, Established());
    forged.signature = pair.Sign(unlabelled);
    CHECK_FALSE(DiscoveryWire::VerifyProofSignature(challenge, forged));
}

TEST_CASE("The discovery proof label moved and the one it replaced is retired", "[cluster][discovery][wire][formation]")
{
    // That no live label is a retired one is `IdentityKeyLabelsSeparate`'s, a build failure; what
    // is asserted here is WHICH label is live and that its predecessor is on the retired list.
    CHECK(LabelOf(IdentityKeyPurpose::DiscoveryProof) == "fastcache-discovery-proof-v3");
    CHECK(std::ranges::contains(RetiredIdentityKeyLabels, std::string_view { "fastcache-discovery-proof-v2" }));
}

TEST_CASE("A proof's fields are framed, not concatenated", "[cluster][discovery][wire]")
{
    // The lesson the object key already records: a separator that can occur inside a value
    // is not a framing. Without length prefixes these two would sign identically, so a node
    // could prove one identity and be believed as another.
    DiscoveryWire::Challenge const challenge { .clusterId = "prod", .nonce = SampleNonce() };
    auto const key = Pair(0x31).PublicKey();
    CHECK_FALSE(std::ranges::equal(DiscoveryWire::ProofMessage(challenge, Speaking("worker", "a:7000"), key).Bytes(),
                                   DiscoveryWire::ProofMessage(challenge, Speaking("workera", ":7000"), key).Bytes()));
}

TEST_CASE("A beacon carries nothing an eavesdropper can use", "[cluster][discovery][wire]")
{
    // A beacon is an invitation to ASK, not a credential: the challenge that follows it
    // is what proves anything, and only to the node that chose its nonce.
    auto const encoded = DiscoveryWire::EncodeBeacon({ .summary = Established() });

    // Everything in it is exactly what was put there, and nothing else: the summary, then
    // zeroes for padding, and a size that accounts for all of it.
    auto const decoded = DiscoveryWire::DecodeBeacon(encoded);
    REQUIRE(decoded.has_value());
    auto const reencoded = DiscoveryWire::EncodeBeacon(Unwrap(decoded));
    CHECK(std::ranges::equal(encoded, reencoded));
}
