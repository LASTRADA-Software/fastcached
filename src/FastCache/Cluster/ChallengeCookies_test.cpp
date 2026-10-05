// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ChallengeCookies.hpp>
#include <FastCache/Cluster/DiscoveryBounds.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using FastCache::Testing::ScriptedSecureRandom;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

// A copy would carry a second record of what is spent, and a proof spent in one would verify
// again in the other; a move would leave a source a caller could still ask.
static_assert(!std::is_copy_constructible_v<ChallengeCookies>);
static_assert(!std::is_move_constructible_v<ChallengeCookies>);
static_assert(!std::is_copy_assignable_v<ChallengeCookies>);

// Nothing but a cookie jar makes a proven discovery summary: no public factory takes a proof.
static_assert(!std::is_default_constructible_v<ProvenFleetSummary>);
static_assert(!std::is_constructible_v<ProvenFleetSummary, FleetSummary>);
static_assert(!std::is_constructible_v<ProvenFleetSummary, FleetSummary, Ed25519PublicKey>);
static_assert(!std::is_convertible_v<FleetSummary, ProvenFleetSummary>);

namespace
{
/// The lifetime these cases run under: the default one.
constexpr auto Lifetime = std::chrono::seconds { 30 };

/// A solitary fleet's summary, every field set, so a field the signature missed would show.
/// @return The summary.
[[nodiscard]] FleetSummary Desk()
{
    return FleetSummary { .clusterId = "c-desk",
                          .state = FleetState::Solitary,
                          .createdAtUnixSeconds = 1'790'000'000,
                          .leaderId = "n-desk",
                          .leaderNodeEndpoint = "desk:6674",
                          .nodeId = "n-desk",
                          .raftEndpoint = "desk:6680" };
}

/// An honest proof of @p summary by @p pair, answering @p challenge and echoing its nonce.
/// @param pair Who signs.
/// @param challenge What was asked.
/// @param summary What they say.
/// @return The proof.
[[nodiscard]] DiscoveryWire::Proof Answer(Ed25519KeyPair const& pair,
                                          DiscoveryWire::Challenge const& challenge,
                                          FleetSummary const& summary)
{
    return DiscoveryWire::Proof {
        .summary = summary,
        .answers = challenge.nonce,
        .publicKey = pair.PublicKey(),
        .signature = SignLabelled(pair, DiscoveryWire::ProofMessage(challenge, summary, pair.PublicKey())),
    };
}

/// The host the desk's beacon came from, so the host its challenge goes to and its proof comes from.
constexpr std::string_view DeskHost = "10.0.0.2";

/// A check budget that never says no, for the cases whose subject is the cookie.
/// @return True.
[[nodiscard]] bool AlwaysCheck()
{
    return true;
}

/// A laptop that challenges from a scripted generator, on a clock the case moves.
struct Laptop
{
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(8 * ChallengeCookies::CookieKeyBytes) };
    core::platform::ManualClock clock;
    ChallengeCookies cookies { random, Lifetime };

    /// Challenge the desk, as its beacon described it, at the host the beacon came from.
    /// @param target What the beacon said.
    /// @return The challenge sent.
    [[nodiscard]] DiscoveryWire::Challenge Ask(FleetSummary const& target = Desk())
    {
        auto challenge = cookies.Issue(clock.now(), "c-laptop", target, DeskHost);
        REQUIRE(challenge.has_value());
        return *std::move(challenge);
    }

    /// Judge @p proof as this laptop's discovery layer does, in its own cluster.
    /// @param proof What arrived.
    /// @param from The host it arrived from; the desk's unless a case says otherwise.
    /// @return The verdict.
    [[nodiscard]] std::expected<ProvenFleetSummary, CookieRefusal> Judge(DiscoveryWire::Proof const& proof,
                                                                         std::string_view from = DeskHost)
    {
        return cookies.Verify(clock.now(), "c-laptop", proof, from, AlwaysCheck);
    }
};

/// The refusal @p verdict carries, or nullopt when it verified.
/// @param verdict What `Verify` answered.
/// @return The refusal.
[[nodiscard]] std::optional<CookieRefusal> RefusalOf(std::expected<ProvenFleetSummary, CookieRefusal> const& verdict)
{
    return verdict.has_value() ? std::nullopt : std::optional { verdict.error() };
}
} // namespace

TEST_CASE("A proof of a challenge this node issued verifies, once", "[cluster][discovery][cookies]")
{
    Laptop laptop;
    auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());

    auto const proven = laptop.Judge(proof);
    REQUIRE(proven.has_value());
    CHECK(Unwrap(proven).Summary() == Desk());
    CHECK(Unwrap(proven).Key() == TestKeyPair("desk").PublicKey());
    CHECK(laptop.cookies.SpentHeld() == 1);

    // The same proof again is a replay: recorded, it verifies nothing a second time.
    CHECK(RefusalOf(laptop.Judge(proof)) == CookieRefusal::Replayed);
    CHECK(laptop.cookies.SpentHeld() == 1);

    // And two challenges to one node are two cookies: a node beaconing twice in an epoch is
    // answered twice, which a cookie over the target alone would have called a replay.
    auto const again = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
    CHECK(again.answers != proof.answers);
    CHECK(laptop.Judge(again).has_value());
}

TEST_CASE("A proof that verifies nothing is refused by name, and spends nothing", "[cluster][discovery][cookies]")
{
    // One row per way a proof can fail to be an answer to this challenge, and WHICH refusal each
    // is: the cookie binds the cluster, node and endpoint, so changing one of those is an answer
    // to no challenge, while the rest of the summary is covered by the signature alone.
    struct Alteration
    {
        std::string_view what;
        std::function<void(DiscoveryWire::Proof&)> alter;
        CookieRefusal refusal;
    };
    auto const alterations = std::vector<Alteration> {
        { .what = "another node",
          .alter = [](DiscoveryWire::Proof& proof) { proof.summary.nodeId = "n-other"; },
          .refusal = CookieRefusal::NotIssued },
        { .what = "another endpoint",
          .alter = [](DiscoveryWire::Proof& proof) { proof.summary.raftEndpoint = "attacker:6680"; },
          .refusal = CookieRefusal::NotIssued },
        { .what = "another cluster",
          .alter = [](DiscoveryWire::Proof& proof) { proof.summary.clusterId = "c-other"; },
          .refusal = CookieRefusal::NotIssued },
        { .what = "a cookie byte flipped",
          .alter = [](DiscoveryWire::Proof& proof) { proof.answers.back() ^= std::byte { 0x01 }; },
          .refusal = CookieRefusal::NotIssued },
        { .what = "a serial never issued",
          .alter = [](DiscoveryWire::Proof& proof) { proof.answers.front() = std::byte { 0x7F }; },
          .refusal = CookieRefusal::NotIssued },
        { .what = "an older fleet claimed after signing",
          .alter = [](DiscoveryWire::Proof& proof) { proof.summary.createdAtUnixSeconds = 0; },
          .refusal = CookieRefusal::Forged },
        { .what = "an established fleet claimed after signing",
          .alter = [](DiscoveryWire::Proof& proof) { proof.summary.state = FleetState::Established; },
          .refusal = CookieRefusal::Forged },
        { .what = "a key other than the one that signed",
          .alter = [](DiscoveryWire::Proof& proof) { proof.publicKey = TestKeyPair("laptop").PublicKey(); },
          .refusal = CookieRefusal::Forged },
    };

    Laptop laptop;
    // One challenge per row, so no row meets another's failed checks (`MaxForgeriesPerChallenge`).
    for (auto const& alteration: alterations)
    {
        INFO(alteration.what);
        auto const honest = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
        auto altered = honest;
        alteration.alter(altered);
        CHECK(RefusalOf(laptop.Judge(altered)) == alteration.refusal);

        // It did not spend the challenge: the honest answer, arriving after it, verifies. Spending
        // on a refusal would let anybody who saw the challenge go out destroy its answer.
        CHECK(laptop.Judge(honest).has_value());
    }
    CHECK(laptop.cookies.SpentHeld() == alterations.size());
}

TEST_CASE("The check budget is asked only for a live cookie, and its refusal spends nothing",
          "[cluster][discovery][cookies]")
{
    // Asked before the cookie, garbage naming no challenge would spend what honest proofs need;
    // asked after nothing, one live cookie buys a check per forgery. So it is asked for exactly the
    // proofs whose cookie holds, unexpired and unspent -- and a "no" leaves the cookie answerable.
    Laptop laptop;
    auto asked = 0;
    auto grant = false;
    auto const budget = std::function<bool()> { [&asked, &grant] {
        ++asked;
        return grant;
    } };
    auto const judge = [&laptop, &budget](DiscoveryWire::Proof const& proof) {
        return laptop.cookies.Verify(laptop.clock.now(), "c-laptop", proof, DeskHost, budget);
    };

    auto const honest = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
    auto unissued = honest;
    unissued.answers.back() ^= std::byte { 0x01 };
    CHECK(RefusalOf(judge(unissued)) == CookieRefusal::NotIssued);
    CHECK(asked == 0);

    CHECK(RefusalOf(judge(honest)) == CookieRefusal::Unchecked);
    CHECK(asked == 1);
    CHECK(laptop.cookies.SpentHeld() == 0);

    // The same proof once the budget says yes: nothing was spent, so it verifies.
    grant = true;
    CHECK(judge(honest).has_value());
    CHECK(asked == 2);

    // A replay and an expired cookie are refused before the budget is asked.
    CHECK(RefusalOf(judge(honest)) == CookieRefusal::Replayed);
    auto const late = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
    laptop.clock.advance(Lifetime);
    CHECK(RefusalOf(judge(late)) == CookieRefusal::Expired);
    CHECK(asked == 2);
}

TEST_CASE("A proof with an empty cluster id verifies nothing and never reaches the encoder", "[cluster][discovery][cookies]")
{
    Laptop laptop;
    auto summary = Desk();
    summary.clusterId = "";

    // Built directly rather than through `Answer`, which itself calls `EncodeFleetSummaryFields` to
    // compose the message to sign -- and that call's precondition assert would abort on this very
    // summary. The wire never hands one over (`DecodeFleetSummaryFields` refuses an empty id), which
    // is exactly why the judge must refuse it before it reaches that encoder rather than rely on it.
    auto const challenge = laptop.Ask(summary);
    auto const proof = DiscoveryWire::Proof {
        .summary = summary, .answers = challenge.nonce, .publicKey = TestKeyPair("desk").PublicKey(), .signature = {}
    };
    CHECK(RefusalOf(laptop.Judge(proof)) == CookieRefusal::Forged);
}

TEST_CASE("A cookie answers only while its epoch is held and its window open", "[cluster][discovery][cookies]")
{
    Laptop laptop;

    SECTION("a lifetime on, the window has closed")
    {
        auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
        laptop.clock.advance(Lifetime);
        CHECK(RefusalOf(laptop.Judge(proof)) == CookieRefusal::Expired);
    }
    SECTION("the previous epoch still answers inside its window")
    {
        auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
        laptop.clock.advance(Lifetime / 2);
        (void) laptop.Ask(); // a new epoch
        CHECK(laptop.random.FillCount() == 2);
        CHECK(laptop.Judge(proof).has_value());
    }
    SECTION("two epochs on, the key is gone")
    {
        auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
        laptop.clock.advance(Lifetime / 2);
        (void) laptop.Ask();
        laptop.clock.advance(Lifetime / 2);
        (void) laptop.Ask();
        CHECK(laptop.random.FillCount() == 3);
        CHECK(RefusalOf(laptop.Judge(proof)) == CookieRefusal::Expired);
    }
    SECTION("the control: inside one epoch, the same proof verifies")
    {
        auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
        laptop.clock.advance((Lifetime / 2) - 1s);
        (void) laptop.Ask();
        CHECK(laptop.random.FillCount() == 1);
        CHECK(laptop.Judge(proof).has_value());
    }
}

TEST_CASE("A proof holds only from the host its challenge was sent to, and is refused before the budget",
          "[cluster][discovery][cookies][security]")
{
    // The cookie binds the DESTINATION host, so a real cookie collected at one address cannot be
    // answered from another: a forger has to receive a challenge at every address it answers from,
    // and the per-source check budget bounds addresses it OWNS rather than ones it types (the
    // reviewer's V-1). Refused as a cookie this node never made, before the budget is asked -- and
    // the same proof from the right host is the control, and verifies.
    Laptop laptop;
    auto asked = 0;
    auto const budget = std::function<bool()> { [&asked] {
        ++asked;
        return true;
    } };
    auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());

    for (auto const elsewhere: std::array<std::string_view, 4> { "10.0.0.66", "10.0.0.20", "10.0.0.2.evil", "" })
    {
        INFO("from: " << elsewhere);
        CHECK(RefusalOf(laptop.cookies.Verify(laptop.clock.now(), "c-laptop", proof, elsewhere, budget))
              == CookieRefusal::NotIssued);
    }
    CHECK(asked == 0);
    CHECK(laptop.cookies.Verify(laptop.clock.now(), "c-laptop", proof, DeskHost, budget).has_value());
    CHECK(asked == 1);
}

TEST_CASE("A proof answers only the cluster this node was in when it asked", "[cluster][discovery][cookies]")
{
    // A node that dissolved or joined a fleet refuses the answers to challenges it sent as the old
    // one: the cookie binds the challenger's cluster, and the judge is asked in the cluster it is now.
    Laptop laptop;
    auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
    CHECK(RefusalOf(laptop.cookies.Verify(laptop.clock.now(), "c-joined", proof, DeskHost, AlwaysCheck))
          == CookieRefusal::NotIssued);
    CHECK(laptop.Judge(proof).has_value());
}

TEST_CASE("A cookie key that cannot be drawn issues no challenge and keeps what is held", "[cluster][discovery][cookies]")
{
    // #1527: withheld rather than issued under a weak key. The epoch already held is kept, so an
    // answer to a challenge issued before the generator failed still verifies.
    Laptop laptop;
    auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
    laptop.random.DenyAfter(0, ScriptedSecureRandom::DeniedFailure());

    laptop.clock.advance(Lifetime / 2);
    auto const refused = laptop.cookies.Issue(laptop.clock.now(), "c-laptop", Desk(), DeskHost);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);
    CHECK(laptop.random.FillCount() == 2);
    CHECK(laptop.Judge(proof).has_value());

    // And one that could never draw issued nothing, so nothing answers it.
    auto denied = ScriptedSecureRandom { ScriptedSecureRandom::DeniedFailure() };
    auto never = ChallengeCookies { denied, Lifetime };
    CHECK_FALSE(never.Issue(laptop.clock.now(), "c-laptop", Desk(), DeskHost).has_value());
    CHECK(RefusalOf(never.Verify(laptop.clock.now(), "c-laptop", proof, DeskHost, AlwaysCheck)) == CookieRefusal::NotIssued);
}

TEST_CASE("An epoch issues at most its cap, and every cookie it issued keeps its place", "[cluster][discovery][cookies]")
{
    // A record with a capacity below what was issued is a race: a flood of VERIFIED proofs, under a
    // key of the attacker's own, filled it before an honest proof arrived (the reviewer's P8). So an
    // epoch issues at most `MaxEpochChallenges` and keeps a state for each; the first and the last
    // it issued both verify, and the next challenge draws a new key although no time has passed.
    Laptop laptop;
    auto const pair = TestKeyPair("desk");
    auto const first = laptop.Ask();
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 2 }, MaxEpochChallenges))
        (void) laptop.Ask();
    auto const last = laptop.Ask();
    CHECK(laptop.random.FillCount() == 1);

    auto const fresh = laptop.Ask();
    CHECK(laptop.random.FillCount() == 2);

    CHECK(laptop.Judge(Answer(pair, first, Desk())).has_value());
    CHECK(laptop.Judge(Answer(pair, last, Desk())).has_value());
    CHECK(laptop.Judge(Answer(pair, fresh, Desk())).has_value());
    CHECK(laptop.cookies.SpentHeld() == 3);
}

TEST_CASE("Verified proofs never fill an epoch's record for the cookies it issued", "[cluster][discovery][cookies]")
{
    // The old record held 1024 per epoch; one more verified proof than that, in one epoch, and the
    // honest proof after them still verifies.
    auto constexpr OldRecord = std::size_t { 1024 };
    static_assert(OldRecord + 2 <= MaxEpochChallenges, "the burst below stays inside one epoch");
    Laptop laptop;
    auto const pair = TestKeyPair("desk");
    auto proofs = std::vector<DiscoveryWire::Proof> {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, OldRecord + 1))
        proofs.push_back(Answer(pair, laptop.Ask(), Desk()));
    auto const honest = Answer(TestKeyPair("lab"), laptop.Ask(), Desk());
    CHECK(laptop.random.FillCount() == 1);

    for (auto const& proof: proofs)
        REQUIRE(laptop.Judge(proof).has_value());
    CHECK(laptop.Judge(honest).has_value());
    CHECK(laptop.cookies.SpentHeld() == OldRecord + 2);
}

TEST_CASE("A cookie buys a bounded number of failed checks, and is exhausted after them", "[cluster][discovery][cookies]")
{
    // A forgery does not spend the cookie it names, so without a cap one live cookie buys a
    // signature check per datagram. After `MaxForgeriesPerChallenge` failed checks it is exhausted:
    // refused without another check, and without asking the budget. What a forger racing the honest
    // answer buys with that is this one challenge; the next is answerable.
    Laptop laptop;
    auto asked = std::size_t { 0 };
    auto const budget = std::function<bool()> { [&asked] {
        ++asked;
        return true;
    } };
    auto const judge = [&laptop, &budget](DiscoveryWire::Proof const& proof) {
        return laptop.cookies.Verify(laptop.clock.now(), "c-laptop", proof, DeskHost, budget);
    };

    auto const challenge = laptop.Ask();
    auto forged = Answer(TestKeyPair("desk"), challenge, Desk());
    forged.publicKey = TestKeyPair("mallory").PublicKey();
    for ([[maybe_unused]] auto const attempt: std::views::iota(std::size_t { 0 }, MaxForgeriesPerChallenge))
        CHECK(RefusalOf(judge(forged)) == CookieRefusal::Forged);
    CHECK(asked == MaxForgeriesPerChallenge);

    CHECK(RefusalOf(judge(forged)) == CookieRefusal::Exhausted);
    CHECK(RefusalOf(judge(Answer(TestKeyPair("desk"), challenge, Desk()))) == CookieRefusal::Exhausted);
    CHECK(asked == MaxForgeriesPerChallenge);

    // One forgery fewer, and the honest answer is still checked and taken.
    auto const next = laptop.Ask();
    auto nextForged = Answer(TestKeyPair("desk"), next, Desk());
    nextForged.publicKey = TestKeyPair("mallory").PublicKey();
    for ([[maybe_unused]] auto const attempt: std::views::iota(std::size_t { 1 }, MaxForgeriesPerChallenge))
        CHECK(RefusalOf(judge(nextForged)) == CookieRefusal::Forged);
    CHECK(judge(Answer(TestKeyPair("desk"), next, Desk())).has_value());
}

TEST_CASE("A proof verified in one epoch is a replay in the next", "[cluster][discovery][cookies]")
{
    // The previous epoch is still accepted after a rotation, so its record must still say what was
    // spent there: a record kept for the current epoch alone, or cleared when a new key is drawn,
    // would let a recorded proof verify a second time across the boundary.
    Laptop laptop;
    auto const proof = Answer(TestKeyPair("desk"), laptop.Ask(), Desk());
    REQUIRE(laptop.Judge(proof).has_value());

    laptop.clock.advance(Lifetime / 2);
    (void) laptop.Ask();
    REQUIRE(laptop.random.FillCount() == 2);

    CHECK(RefusalOf(laptop.Judge(proof)) == CookieRefusal::Replayed);
}
