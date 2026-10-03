// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ChallengeIssuer.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using CompileCacheWire::FleetSummaryReply;
using FastCache::Testing::Unwrap;

namespace
{
/// A nonce of @p fill, as an asker sends it.
/// @param fill Every byte of it.
/// @return The nonce.
[[nodiscard]] std::array<std::byte, CompileCacheWire::NodeChallengeBytes> NonceOf(std::byte fill)
{
    auto nonce = std::array<std::byte, CompileCacheWire::NodeChallengeBytes> {};
    nonce.fill(fill);
    return nonce;
}

/// What @p machine answers to @p nonce about @p summary: signed by its own test key.
/// @param nonce The asker's nonce.
/// @param summary What it says.
/// @param machine Whose key signs and is carried.
/// @return The reply.
[[nodiscard]] FleetSummaryReply Answered(std::span<std::byte const> nonce,
                                         FleetSummary const& summary,
                                         std::string const& machine)
{
    auto const key = Testing::TestKeyPair(machine);
    auto reply = FleetSummaryReply { .summary = summary };
    reply.publicKey = key.PublicKey();
    reply.signature = SignLabelled(key, FleetSummaryMessage(nonce, summary, key.PublicKey()));
    return reply;
}

/// The office fleet's summary, as its leader says it.
/// @return The summary.
[[nodiscard]] FleetSummary Office()
{
    return FleetSummary { .clusterId = "c-office",
                          .state = FleetState::Established,
                          .createdAtUnixSeconds = 1'700'000'000,
                          .leaderId = "n-office",
                          .leaderNodeEndpoint = "office.example:6674",
                          .nodeId = "n-office",
                          .raftEndpoint = "office.example:6680" };
}
} // namespace

TEST_CASE("A fleet summary verifies only over the nonce it was asked with", "[cluster][formation][summary]")
{
    auto const nonce = NonceOf(std::byte { 1 });
    auto const summary = Office();
    auto const reply = Answered(nonce, summary, "n-office");

    auto const proven =
        ProvenFleet::FromSeedAnswer(Testing::IssuedNonceOf(std::byte { 1 }).held, reply, SeedSource::FleetSeedFlag);
    REQUIRE(proven.has_value());
    CHECK(Unwrap(proven).Summary() == summary);
    CHECK(Unwrap(proven).Key() == Testing::TestKeyPair("n-office").PublicKey());
    CHECK(Unwrap(proven).Origin() == FleetOrigin::FleetSeedFlag);

    // A replayed answer: signed over one question's nonce, checked against another question's.
    CHECK_FALSE(ProvenFleet::FromSeedAnswer(Testing::IssuedNonceOf(std::byte { 2 }).held, reply, SeedSource::FleetSeedFlag)
                    .has_value());
}

TEST_CASE("A fleet summary verifies only as it was signed: every field and the key are covered",
          "[cluster][formation][summary]")
{
    auto const nonce = NonceOf(std::byte { 3 });
    auto const reply = Answered(nonce, Office(), "n-office");
    REQUIRE(VerifyFleetSummarySignature(nonce, reply));

    // A relay rewriting any field a yield is decided on -- the state, the age, the leader -- breaks it.
    auto younger = reply;
    younger.summary.createdAtUnixSeconds = 1;
    CHECK_FALSE(VerifyFleetSummarySignature(nonce, younger));
    auto solitary = reply;
    solitary.summary.state = FleetState::Solitary;
    CHECK_FALSE(VerifyFleetSummarySignature(nonce, solitary));
    auto redirected = reply;
    redirected.summary.leaderNodeEndpoint = "mallory.example:6674";
    CHECK_FALSE(VerifyFleetSummarySignature(nonce, redirected));

    // And the signature cannot be re-attributed to another key.
    auto reattributed = reply;
    reattributed.publicKey = Testing::TestKeyPair("mallory").PublicKey();
    CHECK_FALSE(VerifyFleetSummarySignature(nonce, reattributed));

    // A summary naming no cluster is refused before anything is encoded: no summary may carry one.
    auto unnamed = reply;
    unnamed.summary.clusterId.clear();
    CHECK_FALSE(VerifyFleetSummarySignature(nonce, unnamed));
}

TEST_CASE("A discovery proof never verifies as a fleet summary answer", "[cluster][formation][summary]")
{
    // One identity key signs both; only the label keeps them apart. A proof over the same nonce,
    // summary and key is a different message, so its signature proves no FleetSummary answer.
    auto const nonce = NonceOf(std::byte { 4 });
    auto const key = Testing::TestKeyPair("n-office");
    auto const challenge = DiscoveryWire::Challenge { .clusterId = "c-asker", .nonce = nonce };
    auto reply = FleetSummaryReply { .summary = Office() };
    reply.publicKey = key.PublicKey();
    reply.signature = SignLabelled(key, DiscoveryWire::ProofMessage(challenge, reply.summary, key.PublicKey()));
    CHECK_FALSE(VerifyFleetSummarySignature(nonce, reply));
}

TEST_CASE("A seed's answer carries the origin of the source that named the seed", "[cluster][formation][summary]")
{
    // The origin is a preference a yield reads, so it is the factory's and never the holder's: a
    // seed's answer takes its source's row of `SeedOriginTable`, and no source maps to `Beacon`,
    // which only a discovery proof carries.
    auto const nonce = NonceOf(std::byte { 5 });
    auto const reply = Answered(nonce, Office(), "n-office");
    auto const asked = [] {
        return Testing::IssuedNonceOf(std::byte { 5 }).held;
    };
    for (auto const source: Enumerators<SeedSource>())
    {
        auto const proven = ProvenFleet::FromSeedAnswer(asked(), reply, source);
        REQUIRE(proven.has_value());
        CHECK(Unwrap(proven).Origin() == SeedOriginTable[static_cast<std::size_t>(source)].origin);
        CHECK(Unwrap(proven).Origin() != FleetOrigin::Beacon);
    }
    CHECK(Unwrap(ProvenFleet::FromSeedAnswer(asked(), reply, SeedSource::DnsSrv)).Origin() == FleetOrigin::DnsSrv);
    CHECK(Unwrap(ProvenFleet::FromSeedAnswer(asked(), reply, SeedSource::Remembered)).Origin() == FleetOrigin::Remembered);
}

// What makes a recorded answer worthless is a TYPE, not a convention: `VerifyAnswer` takes an
// `IssuedNonce`, which only a draw makes, so bytes -- a recorded nonce above all -- cannot be passed.
static_assert(
    !std::is_invocable_v<decltype(&ProvenFleetSummary::VerifyAnswer), std::span<std::byte const>, FleetSummaryReply const&>,
    "a recorded nonce must not verify an answer");
static_assert(!std::is_invocable_v<decltype(&ProvenFleetSummary::VerifyAnswer), IssuedNonce&, FleetSummaryReply const&>,
              "verifying an answer must CONSUME its nonce, never borrow it");

TEST_CASE("A nonce verifies one answer, and a spent one verifies none", "[cluster][formation][summary]")
{
    // The replay a recorded answer would attempt, from the inside: the one nonce this process drew
    // is consumed by the first verification, so the same answer against the same question is
    // refused the second time -- and a moved-from nonce, which holds nothing, verifies nothing.
    auto fresh = Testing::IssuedNonceOf(std::byte { 6 });
    auto const reply = Answered(fresh.wire, Office(), "n-office");
    // Held in a table, as the discovery layer holds its challenges, so what a move leaves behind
    // is read through the table rather than through a name the move emptied.
    auto table = std::vector<IssuedNonce> {};
    table.push_back(std::move(fresh.held));
    CHECK(ProvenFleetSummary::VerifyAnswer(std::move(table.at(0)), reply).has_value());
    CHECK(table.at(0).Spent());
    CHECK(ProvenFleetSummary::VerifyAnswer(std::move(table.at(0)), reply) == std::nullopt);
}
