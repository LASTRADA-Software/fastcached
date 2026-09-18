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
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

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
    ManualClock clock;
    ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;

    /// This node's identity key, as its start resolved it out of the state directory (#178).
    std::optional<Ed25519KeyPair> identity { Testing::TestKeyPair("n1") };
};

/// A node that runs consensus: a Raft port, which is what turns it on (#1022).
///
/// The shape `StartConsensusOrExplain` accepts, so this is a node whose role WILL be
/// published by the driver -- which is exactly what makes the interval before that
/// publication a window rather than a permanent state.
/// @return The config.
[[nodiscard]] NodeConfig ClusteredNode()
{
    NodeConfig cfg;
    cfg.schedulers = { "127.0.0.1:6675" };
    cfg.serveScheduler = true;
    cfg.nodeId = "n1";
    cfg.raftListen = "127.0.0.1:6680";
    cfg.clusterKeyFile = "cluster.key";
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

    auto tier =
        SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity);
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

    auto tier =
        SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity);
    REQUIRE(tier.has_value());

    (*tier)->SetRole(Distributed::SchedulerRole::Leader, {}, 7);
    CHECK((*tier)->Service().Role() == Distributed::SchedulerRole::Leader);
}

TEST_CASE("A scheduler that runs no consensus is refused before it could lead alone", "[node][scheduler]")
{
    // #178, owner decision 3. A scheduler signs every grant with its identity key and hands its
    // workers a roster its cluster's voters certify, so it holds replicated state -- which is
    // consensus, even on one machine. The standalone leadership a node with no `--listen-raft`
    // used to take at term 0 is gone, and the configuration that asked for it is refused BY
    // NAME at startup rather than run as a scheduler nothing could ever elect.
    //
    // WHAT DISTINGUISHES: the same node given `--listen-raft` (and the key consensus needs) is
    // accepted, so the rule is about consensus and not about scheduling.
    NodeConfig lone;
    lone.schedulers = { "127.0.0.1:6675" };
    lone.serveScheduler = true;
    CHECK(Testing::Unwrap(StartupPolicyRejection(lone)) == SchedulerNeedsConsensusRefusal);

    auto clustered = lone;
    clustered.raftListen = "127.0.0.1:6680";
    clustered.clusterKeyFile = "cluster.key";
    CHECK(StartupPolicyRejection(clustered)
          != std::optional<std::string> { std::string { SchedulerNeedsConsensusRefusal } });

    // And the refusal says how to run one machine: a cluster of one.
    CHECK(SchedulerNeedsConsensusRefusal.contains("--listen-raft"));
    CHECK(SchedulerNeedsConsensusRefusal.contains("cluster of one"));
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

    auto const refused =
        SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, std::nullopt);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == SchedulerNeedsIdentityKeyRefusal);

    CHECK(SchedulerTier::Start(cfg, membership.Oracle(), fix.clock, fix.wallClock, fix.metrics, fix.logger, fix.identity)
              .has_value());
}
