// SPDX-License-Identifier: Apache-2.0
#include "NodeRoster.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::TestKeyPair;

namespace
{

/// The instant every case is decided at.
constexpr auto Noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// A worker with no consensus: its fleet, and nothing reaching it but this machine.
[[nodiscard]] NodeConfig Worker()
{
    NodeConfig cfg;
    cfg.schedulers = { "127.0.0.1:6674" };
    cfg.clusterId = "fleet";
    return cfg;
}

/// The same worker, reachable from other machines: a wildcard bind that admits everybody.
[[nodiscard]] NodeConfig NetworkFacingWorker()
{
    auto cfg = Worker();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.fleetOpen = true;
    return cfg;
}

/// A roster of three keyed voters.
[[nodiscard]] Cluster::Roster ThreeVoters()
{
    Cluster::Roster roster;
    for (auto const* const id: { "n1", "n2", "n3" })
        roster.members.push_back(Cluster::RosterMember { .id = id,
                                                         .raftEndpoint = std::string { id } + ":6680",
                                                         .seat = Cluster::MemberSeat::Voter,
                                                         .publicKey = TestKeyPair(id).PublicKey() });
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

/// The anchors a worker trusts before it holds a roster: every voter's key.
[[nodiscard]] std::vector<Ed25519PublicKey> Anchors()
{
    return { TestKeyPair("n1").PublicKey(), TestKeyPair("n2").PublicKey(), TestKeyPair("n3").PublicKey() };
}

} // namespace

TEST_CASE("A consensus member verifies grants against the state it applies, which never lapses", "[node][roster]")
{
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto const roster = NodeRoster::Build(cfg, clock, metrics, logger);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);
    REQUIRE(node.Lease() != nullptr);

    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    state.rosterVersion = 4;
    node.Applied(state);

    CHECK(node.Lease()->KeysOf("n1").live == TestKeyPair("n1").PublicKey());
    auto const summary = node.Summary();
    REQUIRE(summary.has_value());
    CHECK(Testing::Unwrap(summary).version == 4);
    CHECK(Testing::Unwrap(summary).voters == 1);
    // No certificate on a consensus member: absent, never a lapse far in the future.
    CHECK_FALSE(Testing::Unwrap(summary).certifiedUntil.has_value());
    CHECK_FALSE(node.ExpiresInSeconds().has_value());
    CHECK_FALSE(node.Wanting());
}

TEST_CASE("A consensus member places a server only once its applied state names a voter's key", "[node][roster][proof]")
{
    // #178 PR 6: a node proves itself only to a server its roster does not call a stranger. A
    // consensus member's roster is the state it applied, and until the first commit records a
    // voter's key -- its OWN, when it schedules for itself -- there is no voter a server could have
    // been. Calling it `NotVoter` then made a scheduler that also runs a worker refuse to prove
    // itself to itself until a heartbeat round after that commit, with a warning at every start.
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto const roster = NodeRoster::Build(cfg, clock, metrics, logger);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);

    CHECK(node.StandingOf("n1", TestKeyPair("n1").PublicKey()) == ServerStanding::Unchecked);

    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    state.revokedKeys = { Cluster::RevokedKey { .id = Consensus::NodeId { "n0" },
                                                .publicKey = TestKeyPair("n0").PublicKey() } };
    node.Applied(state);

    // Once a voter's key is known, the three answers are the roster's.
    CHECK(node.StandingOf("n1", TestKeyPair("n1").PublicKey()) == ServerStanding::Voter);
    CHECK(node.StandingOf("n1", TestKeyPair("x").PublicKey()) == ServerStanding::NotVoter);
    CHECK(node.StandingOf("n0", TestKeyPair("n0").PublicKey()) == ServerStanding::Revoked);
}

TEST_CASE("A consensus member names a revoked server as revoked before it knows any voter's key", "[node][roster][proof]")
{
    // The control on the rule above: what a member has NOT learned yet excuses a stranger, never a
    // machine the state it applied says was forgotten.
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto const roster = NodeRoster::Build(cfg, clock, metrics, logger);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);

    Cluster::ClusterState state;
    state.revokedKeys = { Cluster::RevokedKey { .id = Consensus::NodeId { "n0" },
                                                .publicKey = TestKeyPair("n0").PublicKey() } };
    node.Applied(state);

    CHECK(node.StandingOf("n0", TestKeyPair("n0").PublicKey()) == ServerStanding::Revoked);
}

TEST_CASE("A worker no other machine can reach holds no roster and checks no grant", "[node][roster]")
{
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto const roster = NodeRoster::Build(Worker(), clock, metrics, logger);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);
    CHECK(node.Lease() == nullptr);
    CHECK_FALSE(node.Summary().has_value());
    CHECK_FALSE(node.Wanting());
}

TEST_CASE("A worker adopts the roster its anchors certify, keeps it, and starts from it again", "[node][roster]")
{
    auto const scratch = Testing::ScratchDirectory { "node-roster-keep" };
    auto cfg = Worker();
    cfg.voterKeys = Anchors();
    cfg.clusterDir = scratch.Path();
    core::platform::ManualWallClock clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;

    {
        auto const roster = NodeRoster::Build(cfg, clock, metrics, logger);
        REQUIRE(roster.has_value());
        auto& node = *Testing::Unwrap(roster);
        REQUIRE(node.Lease() != nullptr);

        // Nothing yet: every grant would be refused, so the presence loop asks sooner.
        CHECK(node.Wanting());
        CHECK(node.Lease()->Read(Noon).standing == Distributed::RosterStanding::Absent);

        node.Offered(Cluster::EncodeCertifiedRoster(Certified(ThreeVoters(), 3, { "n1", "n2" })));
        CHECK_FALSE(node.Wanting());
        CHECK(node.Lease()->KeysOf("n2").live == TestKeyPair("n2").PublicKey());
        CHECK(node.ExpiresInSeconds() == std::optional<std::uint64_t> { 3600 });
        auto const summary = node.Summary();
        REQUIRE(summary.has_value());
        CHECK(Testing::Unwrap(summary).version == 3);
        CHECK(Testing::Unwrap(summary).certifiedUntil == std::optional { Noon + 1h });
    }

    // A restart reads what it kept -- lapse included, which a restart must not reset.
    clock.setNow(Noon + 30min);
    auto const again = NodeRoster::Build(cfg, clock, metrics, logger);
    REQUIRE(again.has_value());
    auto& restarted = *Testing::Unwrap(again);
    REQUIRE(restarted.Lease() != nullptr);
    CHECK(restarted.Lease()->Read(Noon + 30min).standing == Distributed::RosterStanding::Current);
    CHECK(restarted.ExpiresInSeconds() == std::optional<std::uint64_t> { 1800 });
}

TEST_CASE("A kept roster this machine cannot use refuses the start, never falls back to the anchors", "[node][roster]")
{
    auto const scratch = Testing::ScratchDirectory { "node-roster-bad" };
    auto cfg = Worker();
    cfg.voterKeys = Anchors();
    cfg.clusterDir = scratch.Path();
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;

    SECTION("bytes that are not a roster")
    {
        scratch.Write(Distributed::RosterFileName, "not a roster");
        auto const refused = NodeRoster::Build(cfg, clock, metrics, logger);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains(Distributed::RosterFileName));
    }

    SECTION("another fleet's roster, where --cluster-id asserts this one")
    {
        auto store = Distributed::FileRosterStore { scratch / Distributed::RosterFileName };
        auto other = Certified(ThreeVoters(), 1, { "n1", "n2" });
        other.clusterId = "elsewhere";
        REQUIRE(store.Save(Cluster::PersistedRoster { .certificate = other, .certifiedUntil = Noon + 1h }).has_value());
        cfg.clusterIdExplicit = true;
        auto const refused = NodeRoster::Build(cfg, clock, metrics, logger);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("elsewhere"));

        // The control: the same kept roster where nothing was asserted is adopted as held.
        cfg.clusterIdExplicit = false;
        CHECK(NodeRoster::Build(cfg, clock, metrics, logger).has_value());
    }
}

TEST_CASE("A worker other machines can reach refuses to start holding no roster and no anchor", "[node][roster]")
{
    // Where `RosterlessWorkerRefusal` is answered, since only the state directory can: the table
    // lets this configuration through because it names a `--cluster-dir`, which might have held
    // a roster. It holds none.
    auto const scratch = Testing::ScratchDirectory { "node-roster-none" };
    auto cfg = NetworkFacingWorker();
    cfg.clusterDir = scratch.Path();
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto const refused = NodeRoster::Build(cfg, clock, metrics, logger);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == RosterlessWorkerRefusal);

    // The control: an anchor is enough to start -- the worker then refuses grants until a
    // roster arrives, which is a state it can leave.
    cfg.voterKeys = Anchors();
    CHECK(NodeRoster::Build(cfg, clock, metrics, logger).has_value());
}

TEST_CASE("A node carries the endorsement it last signed, and counts a roster it cannot read", "[node][roster]")
{
    auto cfg = Worker();
    cfg.voterKeys = Anchors();
    core::platform::ManualWallClock const clock { Noon };
    AtomicMetricsSink metrics;
    NullLogger logger;
    auto const roster = NodeRoster::Build(cfg, clock, metrics, logger);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);

    CHECK(node.Endorsement().empty());
    auto const key = TestKeyPair("n1");
    auto const endorsement =
        Cluster::SignEndorsement(Cluster::RosterEndorsement { .clusterId = "fleet",
                                                              .version = 1,
                                                              .rosterDigest = {},
                                                              .notAfter = Noon + 1h,
                                                              .endorser = "n1",
                                                              .signature = {} },
                                 [&key](std::span<std::byte const> message) { return key.Sign(message); });
    node.Endorsed(endorsement);
    CHECK(node.Endorsement() == Cluster::EncodeEndorsement(endorsement));

    auto const garbage = std::vector<std::byte>(7, std::byte { 0x5A });
    node.Offered(garbage);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified) == 1);
    CHECK(node.Wanting());

    // An empty reply is a scheduler with no roster to hand out yet: not counted.
    node.Offered({});
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified) == 1);
}
