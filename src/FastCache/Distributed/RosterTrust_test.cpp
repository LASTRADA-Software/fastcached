// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Distributed/RosterTrust.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using namespace std::chrono_literals;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;

namespace
{

constexpr auto Noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// A roster of voters @p voters, keyed, with @p revoked's keys revoked as `Apply` revokes them.
[[nodiscard]] Cluster::Roster RosterOf(std::vector<std::string> const& voters, std::vector<std::string> const& revoked = {})
{
    Cluster::Roster roster;
    for (auto const& id: voters)
    {
        auto const isRevoked = std::ranges::find(revoked, id) != revoked.end();
        roster.members.push_back(
            Cluster::RosterMember { .id = id,
                                    .raftEndpoint = id + ":6680",
                                    .seat = Cluster::MemberSeat::Voter,
                                    .publicKey = isRevoked ? std::nullopt : std::optional { TestKeyPair(id).PublicKey() } });
    }
    for (auto const& id: revoked)
        roster.revoked.push_back(Cluster::RevokedKey { .id = id, .publicKey = TestKeyPair(id).PublicKey() });
    return roster;
}

/// @p roster at @p version, endorsed by @p endorsers until @p notAfter.
[[nodiscard]] Cluster::CertifiedRoster Certified(Cluster::Roster const& roster,
                                                 std::uint64_t version,
                                                 std::vector<std::string> const& endorsers,
                                                 std::chrono::system_clock::time_point notAfter = Noon + 1h)
{
    auto certified = Cluster::CertifiedRoster {
        .clusterId = "fleet", .version = version, .roster = Cluster::EncodeRoster(roster), .endorsements = {}
    };
    for (auto const& endorser: endorsers)
    {
        auto const key = TestKeyPair(endorser);
        certified.endorsements.push_back(
            Cluster::SignEndorsement(Cluster::RosterEndorsement { .clusterId = "fleet",
                                                                  .version = version,
                                                                  .rosterDigest = Cluster::DigestOfRoster(certified.roster),
                                                                  .notAfter = notAfter,
                                                                  .endorser = endorser,
                                                                  .signature = {} },
                                     [&key](std::span<std::byte const> message) { return key.Sign(message); }));
    }
    return certified;
}

/// Anchors: each named machine's test key.
[[nodiscard]] std::vector<Ed25519PublicKey> Anchors(std::vector<std::string> const& ids)
{
    std::vector<Ed25519PublicKey> keys;
    keys.reserve(ids.size());
    for (auto const& id: ids)
        keys.push_back(TestKeyPair(id).PublicKey());
    return keys;
}

/// A store that records what it was asked to keep.
class RecordingStore final: public IRosterStore
{
  public:
    [[nodiscard]] std::expected<void, std::string> Save(Cluster::PersistedRoster const& roster) override
    {
        saved.push_back(roster);
        return {};
    }

    std::vector<Cluster::PersistedRoster> saved;
};

} // namespace

TEST_CASE("A worker's first roster is the one a majority of its anchors certify", "[distributed][roster]")
{
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    RecordingStore store;
    RosterTrust trust { std::nullopt, Anchors({ "n1", "n2", "n3" }), std::nullopt, &store, metrics, logger };

    CHECK(trust.Read(Noon).standing == RosterStanding::Absent);
    CHECK_FALSE(trust.Summary().has_value());

    CHECK(trust.Offer(Certified(RosterOf({ "n1", "n2", "n3" }), 4, { "n1", "n2" }), Noon) == RosterOfferOutcome::Adopted);
    auto const reading = trust.Read(Noon);
    CHECK(reading.standing == RosterStanding::Current);
    CHECK(reading.certifiedUntil == std::optional { Noon + 1h });
    CHECK(trust.KeysOf("n2").live == TestKeyPair("n2").PublicKey());
    CHECK(Unwrap(trust.Summary()).version == 4);

    // Kept, lapse and all, and said once.
    REQUIRE(store.saved.size() == 1);
    CHECK(store.saved.front().certifiedUntil == Noon + 1h);
}

TEST_CASE("Once a roster is held, it certifies its successor and the anchors are never read again", "[distributed][roster]")
{
    // An anchor the cluster later drops must not get its say back by having been typed on a
    // command line: the replicated state wins over `--voter-key`, as it wins over `--raft-peer`.
    AtomicMetricsSink metrics;
    NullLogger logger;
    RosterTrust trust { std::nullopt, Anchors({ "n1", "n2", "n3" }), std::nullopt, nullptr, metrics, logger };
    REQUIRE(trust.Offer(Certified(RosterOf({ "n1", "n2", "n4" }), 1, { "n1", "n2" }), Noon) == RosterOfferOutcome::Adopted);

    // n3 is an anchor and no voter of the held roster; its endorsement counts for nothing.
    CHECK(trust.Offer(Certified(RosterOf({ "n1", "n3" }), 2, { "n1", "n3" }), Noon) == RosterOfferOutcome::Refused);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified) == 1);

    // The control: n4 is a voter of the held roster and no anchor, and counts.
    CHECK(trust.Offer(Certified(RosterOf({ "n1", "n4" }), 2, { "n1", "n4" }), Noon) == RosterOfferOutcome::Adopted);
}

TEST_CASE("A re-endorsed roster lasts longer, and an older one changes nothing", "[distributed][roster]")
{
    AtomicMetricsSink metrics;
    NullLogger logger;
    RosterTrust trust { std::nullopt, Anchors({ "n1", "n2", "n3" }), std::nullopt, nullptr, metrics, logger };
    auto const roster = RosterOf({ "n1", "n2", "n3" });
    REQUIRE(trust.Offer(Certified(roster, 3, { "n1", "n2" }), Noon) == RosterOfferOutcome::Adopted);

    CHECK(trust.Offer(Certified(roster, 3, { "n1", "n2" }, Noon + 90min), Noon + 30min) == RosterOfferOutcome::Refreshed);
    CHECK(trust.Read(Noon + 30min).certifiedUntil == std::optional { Noon + 90min });

    // Stale: ordinary for a round or two after a change, and never counted.
    CHECK(trust.Offer(Certified(RosterOf({ "n1", "n2" }), 2, { "n1", "n2" }), Noon + 30min) == RosterOfferOutcome::Kept);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified) == 0);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedExpired) == 0);
}

TEST_CASE("A roster lapses past its certification and the slack, and an expired offer is counted apart",
          "[distributed][roster]")
{
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    RosterTrust trust { std::nullopt, Anchors({ "n1", "n2", "n3" }), std::nullopt, nullptr, metrics, logger, 30s };
    REQUIRE(trust.Offer(Certified(RosterOf({ "n1", "n2", "n3" }), 1, { "n1", "n2" }), Noon) == RosterOfferOutcome::Adopted);

    CHECK(trust.Read(Noon + 1h + 30s).standing == RosterStanding::Current);
    CHECK(trust.Read(Noon + 1h + 31s).standing == RosterStanding::Expired);

    // An offer whose majority has lapsed is `Expired`, not `Uncertified`: the voters DID
    // endorse it, the worker just heard too late.
    auto const late = Certified(RosterOf({ "n1", "n2", "n3" }), 2, { "n1", "n2" }, Noon + 1h);
    CHECK(trust.Offer(late, Noon + 2h) == RosterOfferOutcome::Refused);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedExpired) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified) == 0);

    // Said once per reason, counted every time.
    logger.Clear();
    CHECK(trust.Offer(late, Noon + 2h) == RosterOfferOutcome::Refused);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedExpired) == 2);
    CHECK(logger.Snapshot().empty());
}

TEST_CASE("A worker names a revoked signer from the roster it holds", "[distributed][roster]")
{
    AtomicMetricsSink metrics;
    NullLogger logger;
    RosterTrust trust { std::nullopt, Anchors({ "n1", "n2", "n3" }), std::nullopt, nullptr, metrics, logger };
    REQUIRE(trust.Offer(Certified(RosterOf({ "n1", "n2", "n3" }, { "n3" }), 2, { "n1", "n2" }), Noon)
            == RosterOfferOutcome::Adopted);

    auto const revoked = trust.KeysOf("n3");
    CHECK_FALSE(revoked.live.has_value());
    CHECK(std::ranges::contains(revoked.revoked, TestKeyPair("n3").PublicKey()));
    CHECK(trust.KeysOf("n1").live == TestKeyPair("n1").PublicKey());
}

TEST_CASE("A kept roster is where a restarted worker starts, lapse included", "[distributed][roster]")
{
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto const offered = Certified(RosterOf({ "n1", "n2", "n3" }), 5, { "n1", "n2" });
    auto const kept = Cluster::PersistedRoster { .certificate = offered, .certifiedUntil = Noon + 20min };
    RosterTrust trust { std::nullopt, {}, kept, nullptr, metrics, logger };

    CHECK(Unwrap(trust.Summary()).version == 5);
    CHECK(trust.Read(Noon).certifiedUntil == std::optional { Noon + 20min });
    CHECK(trust.KeysOf("n3").live == TestKeyPair("n3").PublicKey());
}

TEST_CASE("A fleet named on the command line refuses another's first roster; none named takes the anchors' word",
          "[distributed][roster]")
{
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto offered = Certified(RosterOf({ "n1", "n2", "n3" }), 1, { "n1", "n2" });

    RosterTrust asserted { std::string { "other" }, Anchors({ "n1", "n2", "n3" }), std::nullopt, nullptr, metrics, logger };
    CHECK(asserted.Offer(offered, Noon) == RosterOfferOutcome::Refused);

    RosterTrust open { std::nullopt, Anchors({ "n1", "n2", "n3" }), std::nullopt, nullptr, metrics, logger };
    CHECK(open.Offer(offered, Noon) == RosterOfferOutcome::Adopted);
}

TEST_CASE("A consensus member's roster is the state it applied: no certificate, never lapsing", "[distributed][roster]")
{
    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() },
                      Cluster::ClusterMember { .id = "n2",
                                               .raftEndpoint = "n2:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Learner,
                                               .publicKey = TestKeyPair("n2").PublicKey() } };
    state.revokedKeys = { Cluster::RevokedKey { .id = "n9", .publicKey = TestKeyPair("n9").PublicKey() } };
    state.rosterVersion = 11;

    StateLeaseRoster roster;
    roster.Adopt(state);
    CHECK(roster.KeysOf("n1").live == TestKeyPair("n1").PublicKey());
    // A learner cannot lead, so it signs no grant.
    CHECK_FALSE(roster.KeysOf("n2").live.has_value());
    CHECK(std::ranges::contains(roster.KeysOf("n1").revoked, TestKeyPair("n9").PublicKey()));
    CHECK(roster.Read(Noon + std::chrono::hours { 10'000 }).standing == RosterStanding::Current);
    CHECK_FALSE(roster.Read(Noon).certifiedUntil.has_value());
    CHECK(roster.Summary().version == 11);
    CHECK(roster.Summary().voters == 1);
}
