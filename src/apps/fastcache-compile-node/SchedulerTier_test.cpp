// SPDX-License-Identifier: Apache-2.0
//
// What the scheduler surface answers BEFORE consensus has told it anything.
//
// The tier is built in `WorkerBody` well before `nodeIo.Start()`, and the consensus
// driver reports a role from a callback that cannot run until that reactor is
// turning -- and then only once an election completes. So "before consensus exists"
// understates the window: it lasts until a leader is elected, which on a restarting
// fleet is an election timeout rather than an instant.
#include "NodeMembership.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>


using namespace FastCache;
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
    ManualClock clock;
    ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
};

/// A node that runs consensus: `--node-id`, with a Raft port to serve it on.
///
/// The shape `StartConsensusOrExplain` accepts, so this is a node whose role WILL be
/// published by the driver -- which is exactly what makes the interval before that
/// publication a window rather than a permanent state.
/// @return The config.
[[nodiscard]] NodeConfig ClusteredNode()
{
    NodeConfig cfg;
    cfg.scheduler = "127.0.0.1:6675";
    cfg.serveScheduler = true;
    cfg.nodeId = "n1";
    cfg.raftListen = "127.0.0.1:6680";
    return cfg;
}

/// A node that runs no consensus at all: no `--node-id`.
/// @return The config.
[[nodiscard]] NodeConfig LoneNode()
{
    NodeConfig cfg;
    cfg.scheduler = "127.0.0.1:6675";
    cfg.serveScheduler = true;
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
    NodeMembership membership { cfg };

    auto tier = SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger);
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
    NodeMembership membership { cfg };

    auto tier = SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger);
    REQUIRE(tier.has_value());

    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    CHECK((*tier)->Service().Role() == Distributed::SchedulerRole::Leader);
}

TEST_CASE("A node leading alone still leads from the moment it starts", "[node][scheduler]")
{
    // **The control, and the reason the fix is conditional rather than a deletion.**
    // A node with no `--node-id` runs no consensus, so nothing will ever call
    // `SetRole` -- leaving it `Undecided` would refuse every verb forever on the
    // deployment most people run. The constructor's own comment says a node that
    // refused until an election completed would be worse than what it replaces "at
    // exactly the moment somebody is watching it start"; that argument is right for
    // this node and does not transfer to a clustered one, where an election really is
    // coming.
    TierFixture fix;
    auto const cfg = LoneNode();
    NodeMembership membership { cfg };

    auto tier = SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger);
    REQUIRE(tier.has_value());

    CHECK((*tier)->Service().Role() == Distributed::SchedulerRole::Leader);
}
