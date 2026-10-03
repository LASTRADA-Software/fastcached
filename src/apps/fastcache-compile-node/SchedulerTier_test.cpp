// SPDX-License-Identifier: Apache-2.0
//
// What the scheduler surface answers BEFORE consensus has told it anything.
//
// The tier is built in `WorkerBody` well before `nodeIo.Start()`, and the consensus
// driver reports a role from a callback that cannot run until that reactor is
// turning -- and then only once an election completes. So "before consensus exists"
// understates the window: it lasts until a leader is elected, which on a restarting
// fleet is an election timeout rather than an instant.
#include "NodeFormation.hpp"
#include "NodeMembership.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;

/// `NodeMembership` reports an unreadable `fleet-open` row here; no case asserts on it.
namespace
{
FastCache::NullLogger membershipLog;
}
using namespace FastCache::Node;

namespace
{

/// Everything `SchedulerTier::Start` needs, kept alive for the tier's lifetime.
///
/// A struct rather than locals per case because the tier borrows every one of them:
/// a case that let one go out of scope first would be testing a dangling reference
/// rather than a scheduler.
struct TierFixture
{
    /// The node's one condition registry, which the tier answers its fleet-wide rows into.
    NodeConditions conditions;

    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;

    /// This node's identity key, as its start resolved it out of the state directory (#178).
    std::optional<Ed25519KeyPair> identity { Testing::TestKeyPair("n1") };
};

namespace Wire = CompileCacheWire;

/// An interval the condition watch never reaches inside a case, so every evaluation a case sees is
/// one it asked for -- and no watch thread reads the `ManualClock` while the case advances it.
constexpr auto NoWatch = std::chrono::hours { 24 };

/// A caller the fleet has admitted.
Distributed::CallerContext const Insider { .membership = Distributed::Membership::Member, .peerId = "peer-1" };

/// The row @p condition as @p conditions would send it.
/// @param conditions The registry.
/// @param condition The row.
/// @return Its fields.
[[nodiscard]] Wire::NodeConditionFields SentRow(NodeConditions const& conditions, NodeCondition condition)
{
    auto const rows = conditions.Snapshot();
    auto const found = std::ranges::find(rows, RowFor(condition).id, &Wire::NodeConditionFields::id);
    REQUIRE(found != rows.end());
    return *found;
}

/// A node that runs consensus: a Raft port, which is what turns it on (#1022).
///
/// The shape `StartConsensusOrExplain` accepts, so this is a node whose role WILL be
/// published by the driver -- which is exactly what makes the interval before that
/// publication a window rather than a permanent state.
/// @return The config.
[[nodiscard]] NodeConfig ClusteredNode()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.schedulers = { "127.0.0.1:6675" };
    cfg.nodeId = "n1";
    cfg.raftListen = "127.0.0.1:6680";
    return cfg;
}

} // namespace

TEST_CASE("A clustered scheduler does not claim leadership before consensus reports", "[node][scheduler]")
{
    // **The defect** ([#613](https://github.com/LASTRADA-Software/fastcached/issues/613)).
    // The constructor published `SetRole(Leader, {}, StandaloneSchedulerTerm)`
    // unconditionally, so a node configured for consensus answered `Lease` as leader,
    // at term 0, from the moment its listener began accepting until an election
    // finished. Grants minted in that window name term 0, and a worker that has
    // learned any term above it refuses every one of them.
    //
    // "Leader at term 0" is not a weaker answer than "leader at term N" -- it is a
    // different and wrong one, which is the same distinction the node already draws
    // between bound and surveyed (#365): a surface must not answer a question whose
    // input it does not yet have.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };

    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());

    // `Undecided` is the state this already has a name and a refusal for: `Gate()`
    // answers anything but `Leader` with `NotLeader`, and a client follows that --
    // an empty endpoint during an election means compile locally, which is the
    // designed behaviour and costs one local compile rather than a wrong grant.
    CHECK((*tier)->Service().Role() != Distributed::SchedulerRole::Leader);
}

TEST_CASE("Consensus reporting leadership is what makes a clustered scheduler lead", "[node][scheduler]")
{
    // The other half, and it is not decoration: a fix that simply never led would
    // satisfy the case above and break every cluster. The role arrives through the
    // one seam consensus drives, and the term arrives with it.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };

    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());

    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    CHECK((*tier)->Service().Role() == Distributed::SchedulerRole::Leader);
}

TEST_CASE("A node whose consensus is closed serves no scheduler, whatever its mode says", "[node][scheduler]")
{
    // #178, owner decision 3. A scheduler signs every grant with its identity key and hands its
    // workers the cluster's state, so it holds replicated state -- which is consensus, even on
    // one machine. The standalone leadership a node with no consensus used to take at term 0 is
    // gone. Since the mode decides the scheduler duty, a mode that serves one on a node whose
    // consensus is CLOSED (an empty `--listen-raft=`) serves none: there is no flag left to
    // refuse, and nothing starts that nothing could elect.
    //
    // WHAT DISTINGUISHES: the same node with consensus open serves one, so the fold is about
    // consensus and not about the mode.
    auto lone = Testing::FirstStart(NodeConfig {});
    lone.schedulers = { "127.0.0.1:6675" };
    lone.raftListen.clear();
    lone.raftListenExplicit = true;
    REQUIRE_FALSE(RunsConsensus(lone));
    CHECK_FALSE(ServesScheduler(lone));

    auto clustered = lone;
    clustered.raftListen = "127.0.0.1:6680";
    REQUIRE(RunsConsensus(clustered));
    CHECK(ServesScheduler(clustered));
}

TEST_CASE("A scheduler holding no identity key is refused, never run unsigned", "[node][scheduler][lease]")
{
    // #178. Every grant is signed by the issuing voter's own key; there is no unsigned grant
    // left to fall back to, so a tier handed no key refuses to exist rather than minting
    // grants no worker could verify. Unreachable from a configuration -- a scheduler runs
    // consensus and a consensus node always holds a key -- which is why this is the answer
    // to a CALLER, and the control beside it is the ordinary start.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };

    auto const refused = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, std::nullopt, fix.conditions, NoWatch);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == SchedulerNeedsIdentityKeyRefusal);

    CHECK(SchedulerTier::Start(cfg,
                               membership.Oracle(),
                               fix.clock,
                               fix.wallClock,
                               fix.metrics,
                               fix.logger,
                               fix.identity,
                               fix.conditions,
                               NoWatch)
              .has_value());
}

TEST_CASE("A scheduler answers its fleet-wide rows as it starts, and only a leader decides them",
          "[node][scheduler][conditions]")
{
    // A fleet-wide row reads the LEADER's registry -- what clients asked it for, what every machine
    // announced to it -- and a follower's copy holds none of that. So anything but the leader says
    // `not-evaluated`, never `clear`: clear from a node that cannot see the fleet is a confident
    // wrong signal. WHAT DISTINGUISHES: the same rows read `clear` once this node leads, and go back
    // to `not-evaluated`, naming the new leader, when it is demoted.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };
    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());

    auto const fleetRows = [] {
        auto rows = std::vector<NodeCondition> {};
        for (auto const& row: NodeConditionTable)
            if (row.scope == ConditionScope::Scheduler)
                rows.push_back(row.condition);
        return rows;
    }();
    // Absence of the negative is not the positive: the loops below say nothing over an empty set.
    REQUIRE_FALSE(fleetRows.empty());

    for (auto const condition: fleetRows)
    {
        INFO("condition " << RowFor(condition).id);
        auto const row = SentRow(fix.conditions, condition);
        CHECK(row.state == "not-evaluated");
        CHECK(row.detail.contains("not leading"));
    }

    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    for (auto const condition: fleetRows)
    {
        INFO("condition " << RowFor(condition).id);
        CHECK(fix.conditions.StateOf(condition) == Wire::ConditionState::Clear);
    }

    (*tier)->SetRole(Distributed::SchedulerRole::Follower, "10.0.0.5:6674", 8);
    for (auto const condition: fleetRows)
    {
        INFO("condition " << RowFor(condition).id);
        auto const row = SentRow(fix.conditions, condition);
        CHECK(row.state == "not-evaluated");
        CHECK(row.detail.contains("10.0.0.5:6674"));
    }
}

TEST_CASE("A leading scheduler names the toolchain nobody serves, and clears it once a worker does",
          "[node][scheduler][conditions]")
{
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };
    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());
    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    auto& service = (*tier)->ServiceForSurfaces();

    auto const ask =
        Wire::LeaseRequest { .fingerprint = "fp-cl", .key = "k1", .acceptedCodecs = {}, .toolchainLabel = "cl 19.44.35207" };
    REQUIRE(service.Lease(Insider, ask).error == Wire::ErrorCode::NoWorker);
    (*tier)->EvaluateConditions();

    auto const raised = SentRow(fix.conditions, NodeCondition::UnservedToolchain);
    CHECK(raised.state == "raised");
    CHECK(raised.persistence == "live");
    // The driver AND the version, as the client reported them -- the whole point of the row.
    CHECK(raised.detail.contains("cl 19.44.35207"));
    CHECK(raised.detail.contains("1 lease(s) refused"));

    // A live row clears while the process runs: a worker for that fingerprint is the fix.
    REQUIRE(service
                .Register(Insider,
                          Distributed::WorkerRegistration {
                              .fingerprint = "fp-cl", .endpoint = "10.0.0.9:6674", .slots = 1, .codecs = {} })
                .status
            == Wire::Status::Ok);
    (*tier)->EvaluateConditions();
    CHECK(fix.conditions.StateOf(NodeCondition::UnservedToolchain) == Wire::ConditionState::Clear);
}

TEST_CASE("An unlabelled toolchain is still named, never dropped", "[node][scheduler][conditions]")
{
    // An operator's pinned `<fingerprint>=<compiler>` has no banner, so its client sends no label.
    // That is not a reason to name nothing: `ToolchainName` marks it `(unlabelled)` rather than
    // leaving the row silent about which toolchain it means.
    //
    // A label spelled in another encoding is not tested here: `SchedulerService::Lease` refuses the
    // WHOLE request `MalformedFrame` when its label is not valid UTF-8 (`LeaseFieldRefusals`,
    // `RefuseUnkept` in SchedulerService.cpp) -- text a peer sends is validated where it enters, so
    // an undecodable label never reaches `UnservedToolchainsNow` for this row to name. `IsValidUtf8`
    // on the composed detail below is still asked, since `ListDetail` joins text this scheduler kept.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };
    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());
    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    auto& service = (*tier)->ServiceForSurfaces();

    REQUIRE(
        service.Lease(Insider, Wire::LeaseRequest { .fingerprint = "fp-pinned", .key = "k1", .acceptedCodecs = {} }).error
        == Wire::ErrorCode::NoWorker);
    (*tier)->EvaluateConditions();

    auto const raised = SentRow(fix.conditions, NodeCondition::UnservedToolchain);
    CHECK(raised.state == "raised");
    CHECK(raised.detail.contains("fp-pinned"));
    CHECK(raised.detail.contains("unlabelled"));
    CHECK(IsValidUtf8(raised.detail));
}

TEST_CASE("The condition watch re-asks the fleet-wide rows without being told", "[node][scheduler][conditions]")
{
    // Both fleet-wide rows clear by TIME as well as by event, so an evaluation driven only by verbs
    // would leave a leader nobody talks to naming a machine that left. The watch is what re-asks;
    // this case proves it runs, by never asking itself.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };
    auto tier = SchedulerTier::Start(cfg,
                                     membership.Oracle(),
                                     fix.clock,
                                     fix.wallClock,
                                     fix.metrics,
                                     fix.logger,
                                     fix.identity,
                                     fix.conditions,
                                     std::chrono::milliseconds { 10 });
    REQUIRE(tier.has_value());
    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    REQUIRE(fix.conditions.StateOf(NodeCondition::UnservedToolchain) == Wire::ConditionState::Clear);

    REQUIRE((*tier)
                ->ServiceForSurfaces()
                .Lease(Insider, Wire::LeaseRequest { .fingerprint = "fp-cl", .key = "k1", .acceptedCodecs = {} })
                .error
            == Wire::ErrorCode::NoWorker);

    CHECK(Testing::WaitUntil(
        "the condition watch to raise unserved-toolchain with nobody calling EvaluateConditions",
        [&fix] { return fix.conditions.StateOf(NodeCondition::UnservedToolchain) == Wire::ConditionState::Raised; },
        [&fix] {
            return std::format("unserved-toolchain is {}",
                               Wire::ConditionName(fix.conditions.StateOf(NodeCondition::UnservedToolchain)));
        }));
}

TEST_CASE("A leading scheduler raises mixed-node-versions for two builds of one wire, and clears when one leaves",
          "[node][scheduler][conditions]")
{
    // Every machine here speaks this wire -- another wire is refused before it can announce -- so
    // nothing refuses the odd build and nothing else says so. The leader counts its OWN build too.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };
    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());
    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    // Alone, the leader is one build: checked and benign.
    REQUIRE(fix.conditions.StateOf(NodeCondition::MixedNodeVersions) == Wire::ConditionState::Clear);

    auto const odd = std::string { VersionString } + "-other-build";
    REQUIRE((*tier)
                ->ServiceForSurfaces()
                .AnnounceNode(Insider,
                              Distributed::NodePresence { .endpoint = "10.0.0.8:6674",
                                                          .version = odd,
                                                          .capacity = {},
                                                          .load = {},
                                                          .conditions = std::nullopt,
                                                          .endorsement = {} })
                .status
            == Wire::Status::Ok);
    (*tier)->EvaluateConditions();

    auto const raised = SentRow(fix.conditions, NodeCondition::MixedNodeVersions);
    CHECK(raised.state == "raised");
    CHECK(raised.persistence == "live");
    CHECK(raised.detail.contains(odd));
    CHECK(raised.detail.contains("10.0.0.8:6674"));
    CHECK(raised.detail.contains(std::format("{} on", VersionString)));

    // The odd machine stops announcing; once its presence expires the fleet is one build again,
    // with no verb arriving to say so -- the leader's own evaluation notices.
    fix.clock.advance(Distributed::WorkerRegistry::DefaultHeartbeatTimeout + std::chrono::seconds { 1 });
    (*tier)->EvaluateConditions();
    CHECK(fix.conditions.StateOf(NodeCondition::MixedNodeVersions) == Wire::ConditionState::Clear);
}

TEST_CASE("mixed-node-versions counts a machine that named no version apart, and never as a build",
          "[node][scheduler][conditions]")
{
    // A machine that did not say its version is neither a build of its own nor silently folded
    // into one that happens to match -- SpreadOfVersions counts it as `unstated`, and the row's
    // detail must say so once something else has already made the fleet mixed.
    TierFixture fix;
    auto const cfg = ClusteredNode();
    NodeMembership membership { cfg, membershipLog };
    auto tier = SchedulerTier::Start(
        cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity, fix.conditions, NoWatch);
    REQUIRE(tier.has_value());
    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);

    auto const odd = std::string { VersionString } + "-other-build";
    REQUIRE((*tier)
                ->ServiceForSurfaces()
                .AnnounceNode(Insider,
                              Distributed::NodePresence { .endpoint = "10.0.0.8:6674",
                                                          .version = odd,
                                                          .capacity = {},
                                                          .load = {},
                                                          .conditions = std::nullopt,
                                                          .endorsement = {} })
                .status
            == Wire::Status::Ok);
    REQUIRE((*tier)
                ->ServiceForSurfaces()
                .AnnounceNode(Insider,
                              Distributed::NodePresence { .endpoint = "10.0.0.9:6674",
                                                          .version = {},
                                                          .capacity = {},
                                                          .load = {},
                                                          .conditions = std::nullopt,
                                                          .endorsement = {} })
                .status
            == Wire::Status::Ok);
    (*tier)->EvaluateConditions();

    auto const raised = SentRow(fix.conditions, NodeCondition::MixedNodeVersions);
    CHECK(raised.state == "raised");
    CHECK(raised.detail.contains("(1 more machine(s) did not say)"));
}
