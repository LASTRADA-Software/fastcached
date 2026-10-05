// SPDX-License-Identifier: Apache-2.0
/// What discovery spends per datagram it answers or judges: one Ed25519 signature per challenge
/// answered, one verification per proof whose cookie holds, and one HMAC per cookie issued or
/// checked.
///
/// The answer budget (`Cluster::DiscoveryBounds`) is sized from the first figure: at its refill
/// rate, signing is the share of a core a flood of challenges can take. So the figure a budget is
/// argued from is measured here, on the construction discovery actually signs -- a proof message
/// over a summary of realistic size -- rather than quoted from the primitive's reputation.
///
/// A figure here is a quantity under conditions. The BUILD is printed for the whole binary before
/// any case runs (`BuildBannerListener.cpp`); whoever quotes one states the host, its load and the
/// sample count, which no binary can read off itself.

#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace FastCache;

TEST_CASE("bench: the signature work discovery does per answer, per proof and per cookie", "[!benchmark][discovery]")
{
    auto seed = std::array<std::byte, Ed25519SeedBytes> {};
    seed.fill(std::byte { 0x5A });
    auto const pair = Ed25519KeyPair::FromSeed(seed);
    REQUIRE(pair.has_value());
    auto const publicKey = pair->PublicKey();

    // A summary of the size an office node announces: a minted cluster id, hostnames and ports.
    auto const summary = CompileCacheWire::FleetSummary { .clusterId = std::string(32, 'c'),
                                                          .state = CompileCacheWire::FleetState::Established,
                                                          .createdAtUnixSeconds = 1'790'000'000,
                                                          .leaderId = "n-office-build-01",
                                                          .leaderNodeEndpoint = "office-build-01.lan:6674",
                                                          .nodeId = "n-office-build-07",
                                                          .raftEndpoint = "office-build-07.lan:6680" };
    auto challenge = Cluster::DiscoveryWire::Challenge { .clusterId = std::string(32, 'a'), .nonce = {} };
    challenge.nonce.fill(std::byte { 0x33 });
    auto const message = Cluster::DiscoveryWire::ProofMessage(challenge, summary, publicKey);
    auto const signature = SignLabelled(*pair, message);

    // The positive control: a verification that failed would time a refusal, not the work.
    REQUIRE(VerifyLabelled(publicKey, message, signature));

    BENCHMARK("sign a discovery proof (one answered challenge)")
    {
        return SignLabelled(*pair, Cluster::DiscoveryWire::ProofMessage(challenge, summary, publicKey));
    };
    BENCHMARK("verify a discovery proof (one proof whose cookie holds)")
    {
        return VerifyLabelled(publicKey, Cluster::DiscoveryWire::ProofMessage(challenge, summary, publicKey), signature);
    };

    // The cookie is a truncated HMAC over the label, the serial and five short texts -- the last
    // the host the challenge went to: the same shape and size as this message.
    auto const cookieKey = std::array<std::byte, 32> {};
    auto const serial = WireFields::ToBigEndian(std::uint64_t { 7 });
    auto const cookieMessage =
        WireFields::Encode({ WireFields::AsBytes(std::string_view { "fastcache-discovery-cookie-v2" }),
                             std::span<std::byte const> { serial },
                             WireFields::AsBytes(challenge.clusterId),
                             WireFields::AsBytes(summary.clusterId),
                             WireFields::AsBytes(summary.nodeId),
                             WireFields::AsBytes(summary.raftEndpoint),
                             WireFields::AsBytes(std::string_view { "10.0.0.66" }) });
    BENCHMARK("MAC a challenge cookie (one issued or checked)")
    {
        return HmacSha256(cookieKey, cookieMessage);
    };
}
