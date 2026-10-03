// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "LocalCache.hpp"
#include "NodeFormation.hpp"
#include "NodeIdentity.hpp"
#include "NodeMembership.hpp"
#include "NodeRoster.hpp"
#include "SchedulerTier.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/PreviousClusterState.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SharedTierFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;

namespace
{
/// `NodeMembership` reports an unreadable `fleet-open` row here; no case asserts on it.
FastCache::NullLogger membershipLog;

/// A loopback port nothing listens on at the moment of asking: bound, read and released, the way
/// the cases here state a member's endpoint. One helper rather than a lambda per case.
/// @return The port.
[[nodiscard]] std::uint16_t FreeLoopbackPort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();
    return port;
}

/// An upstream that records, at every apply it is told of, what the directory it reads said then --
/// so a case can tell "told after the directory" from "told at all".
class RecordingUpstream final: public Node::ICacheUpstream
{
  public:
    /// @param directory What the production upstream would read; must outlive this.
    explicit RecordingUpstream(Node::ISharedCacheTargetSource const& directory) noexcept:
        _directory { directory }
    {
    }

    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view /*key*/) override
    {
        co_return std::nullopt;
    }

    [[nodiscard]] core::async::Task<Node::UpstreamStore> Store(std::string_view /*key*/,
                                                               std::span<std::byte const> /*value*/) override
    {
        co_return Node::UpstreamStore::NotConfigured;
    }

    [[nodiscard]] bool Configured() const noexcept override
    {
        return false;
    }

    void StateApplied() override
    {
        std::scoped_lock const lock { _mutex };
        _seen.push_back(_directory.Current().source);
    }

    /// @return The directory's source at each apply, in order. Any thread.
    [[nodiscard]] std::vector<CompileCacheWire::WireSharedCacheSource> Seen() const
    {
        std::scoped_lock const lock { _mutex };
        return _seen;
    }

  private:
    Node::ISharedCacheTargetSource const& _directory;
    mutable std::mutex _mutex;
    std::vector<CompileCacheWire::WireSharedCacheSource> _seen;
};
} // namespace
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

TEST_CASE("A leader advertises the port a client speaks to, at an address it can reach", "[node][consensus]")
{
    // Neither half can supply the other, which is the whole reason this is a
    // function. A scheduling node's `--listen-node` binds the WILDCARD for a bare
    // port -- peers are
    // on other machines by definition -- so what the surface bound names no address a
    // client can dial. The consensus endpoint is dialable by construction, every peer
    // opening a socket to it, and names the wrong port.
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "0.0.0.0:7000") == "10.0.0.1:7000");

    // A scheduler bound to one interface keeps its port and nothing else: the host is
    // the one peers have proved they can reach.
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "127.0.0.1:7100") == "10.0.0.1:7100");
}

TEST_CASE("A log line says a learner has no consensus endpoint rather than leaving a blank", "[node][consensus][learner]")
{
    CHECK(DescribeConsensusEndpoint("10.0.0.1:6680") == "at 10.0.0.1:6680");
    CHECK(DescribeConsensusEndpoint("") == "with no consensus endpoint");
}

TEST_CASE("A node with no scheduler surface advertises nothing", "[node][consensus]")
{
    // A legitimate shape rather than a misconfiguration: a member that contributes
    // CPU and consensus without handing out anybody's work. Recording an endpoint for
    // it would redirect clients at a port nothing is listening on, which is worse
    // than redirecting them nowhere -- they would wait for a connect that cannot
    // succeed instead of compiling locally at once.
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "").empty());
    CHECK(AdvertisedSchedulerEndpoint("", "0.0.0.0:7000").empty());
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1", "0.0.0.0:7000").empty());
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "7000").empty());
}

TEST_CASE("An IPv6 advertisement is bracketed, so it splits back the way it went in", "[node][consensus]")
{
    // `SplitHostPort` hands back a v6 host WITHOUT its brackets, and every consumer
    // of this string splits it again -- so it has to go back the way it came or the
    // next split takes the wrong colon. That is the defect `Core/HostPort` exists to
    // hold in one place, and this is one of the places.
    auto const advertised = AdvertisedSchedulerEndpoint("[2001:db8::1]:6680", "[::]:7000");
    CHECK(advertised == "[2001:db8::1]:7000");

    auto const split = SplitHostPort(advertised);
    REQUIRE(split.has_value());
    CHECK(Unwrap(split).first == "2001:db8::1");
    CHECK(Unwrap(split).second == "7000");
}

TEST_CASE("A role line names the term it happened in", "[node][consensus]")
{
    // Issue #117: the dump of an intermittent election showed three nodes moving
    // between roles and gave no way to tell one re-election from five, because the
    // line carried no term at all.
    using FastCache::Distributed::SchedulerRole;

    CHECK(DescribeRole(SchedulerRole::Leader, FastCache::Consensus::Term { .value = 4 }, "")
          == "consensus: this node is now the leader in term 4");

    CHECK(DescribeRole(SchedulerRole::Follower, FastCache::Consensus::Term { .value = 4 }, "127.0.0.1:6674")
          == "consensus: this node is now a follower in term 4 of 127.0.0.1:6674");

    // A node that knows no leader has no endpoint to name, and the line must not
    // grow an empty " of " where one would go -- that reads as a redirect to
    // nowhere rather than as an election in progress.
    CHECK(DescribeRole(SchedulerRole::Undecided, FastCache::Consensus::Term { .value = 5 }, "")
          == "consensus: this node is now undecided in term 5");
}

TEST_CASE("A demotion names the term, the peer, and what this node was", "[node][consensus]")
{
    // The three facts the CI dump was missing. The role is the CONSENSUS one:
    // `pre-candidate` and `candidate` both read as `undecided` to the scheduler,
    // and a deposed LEADER is the case worth spotting at a glance.
    auto const cause = FastCache::Consensus::TermAdoption { .previousTerm = FastCache::Consensus::Term { .value = 1 },
                                                            .previousRole = FastCache::Consensus::Role::Leader,
                                                            .from = "n3" };

    CHECK(DescribeTermAdoption(FastCache::Consensus::Term { .value = 2 }, cause)
          == "consensus: term 2 arrived from n3; this node was leader in term 1");
}

TEST_CASE("A quorum proposal stops being in flight when the term moves", "[node][consensus][membership]")
{
    // #388. The reconciler waits rather than re-proposing while a configuration
    // change is in flight, which is right -- `RaftNode` refuses a second one, so
    // re-proposing every interval would log a refusal every interval.
    //
    // What the wait could not see is that leadership moved. A proposal made in a
    // term this node no longer holds was never committed, and an uncommitted entry
    // from a dead term is truncated by whoever leads next -- so the index it landed
    // at may hold something else, or nothing.
    using Consensus::LogIndex;
    using Consensus::Term;

    constexpr auto At = [](std::uint64_t v) {
        return LogIndex { .value = v };
    };
    constexpr auto In = [](std::uint64_t v) {
        return Term { .value = v };
    };

    SECTION("in flight while the term holds and the log has not caught up")
    {
        CHECK(QuorumProposalPending(At(5), In(1), /*commitIndex=*/At(4), /*currentTerm=*/In(1)));
    }

    SECTION("settled once the commit index reaches it")
    {
        CHECK_FALSE(QuorumProposalPending(At(5), In(1), At(5), In(1)));
        CHECK_FALSE(QuorumProposalPending(At(5), In(1), At(6), In(1)));
    }

    SECTION("and abandoned when the term moved, however far behind the log is")
    {
        // The case that deadlocked. A node proposes at index 5 in term 1, is
        // deposed, and is elected again; the entry at 5 is long gone and the commit
        // index is BELOW it. Judged on the index alone this reads as "still in
        // flight" forever, so the change is never re-proposed, the joiner it was
        // going to admit is never counted, and -- having no cluster -- that joiner
        // is excused from every deadline and votes in no election. The cluster then
        // cannot re-elect once one more member goes away.
        CHECK_FALSE(QuorumProposalPending(At(5), In(1), /*commitIndex=*/At(4), /*currentTerm=*/In(2)));

        // Including the case where the log went backwards further still, which is
        // what a truncation looks like.
        CHECK_FALSE(QuorumProposalPending(At(5), In(1), At(0), In(3)));
    }

    SECTION("a node that has proposed nothing is never waiting")
    {
        // The default-constructed pair. Term 0 is what a node starts in, so this
        // must not read as a live proposal made in the current term.
        CHECK_FALSE(QuorumProposalPending(LogIndex {}, Term {}, LogIndex {}, Term {}));
        CHECK_FALSE(QuorumProposalPending(LogIndex {}, Term {}, At(9), In(4)));
    }
}

TEST_CASE("A consensus tier is built exactly when RunsConsensus says so", "[node][consensus]")
{
    // #613 was two tiers authoring one rule: `StartConsensusOrExplain` deciding
    // whether there is a cluster to start and `SchedulerTier` deciding whether a role
    // is COMING. While each spelled `cfg.nodeId.empty()` for itself the scheduler
    // published standalone leadership at term 0 and consensus published a real term
    // over the top of it, leaving a window in which the surface answered `Lease` as a
    // leader that had never been elected.
    //
    // #1022 MOVED that rule -- the switch is `--listen-raft` now -- and a moved rule is
    // exactly when a second author reappears. So this asserts the tier's own decision
    // at both inputs rather than trusting that it still calls the predicate.
    //
    // No I/O in any case. The `--node-id` case returns before anything is built;
    // the `--listen-raft` cases have no id, or no address, to be dialled under, so they reach
    // `ConsensusTier::Start` and are refused there, each by its own name -- which is the
    // observation, because a tier that had skipped the gate would have returned a null
    // tier instead.
    NullLogger logger;
    AtomicMetricsSink metrics;
    std::unique_ptr<SchedulerTier> const noScheduler;

    // Never reached by any case: one returns before consensus exists, the others are
    // refused before anything is wired. A roster for a node that holds none.
    auto const roster =
        NodeRoster::Build(Testing::FirstStart(NodeConfig {}), core::platform::defaultSystemWallClock(), metrics, logger);
    REQUIRE(roster.has_value());
    // Never asked to open: these cases' clusters name no shared cache.
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };

    SECTION("--node-id with no --listen-raft builds no tier")
    {
        auto cfg = Testing::FirstStart(NodeConfig {});
        cfg.raftListen.clear();
        cfg.nodeId = "n1";
        cfg.raftSelf = "10.0.0.1";
        NodeMembership membership { cfg, membershipLog };

        auto const tier = StartConsensusOrExplain(
            cfg,
            noScheduler,
            "127.0.0.1:6674",
            std::nullopt,
            membership,
            *Unwrap(roster),
            SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
            core::platform::defaultSystemWallClock(),
            metrics,
            logger);
        REQUIRE(tier.has_value());
        CHECK(*tier == nullptr);
    }

    // The identity key is resolved first, and its own refusal is the next case's: these two hold one,
    // so what refuses them is the self record they are about.
    std::optional<Ed25519KeyPair> const identity { Testing::TestKeyPair("n1") };

    SECTION("--listen-raft with no --node-id gets past the gate")
    {
        auto cfg = Testing::FirstStart(NodeConfig {});
        cfg.raftListen = "6680";
        NodeMembership membership { cfg, membershipLog };

        // Refused, and refused by NAME: a null tier here would mean the gate is still
        // reading the id, and any other refusal would mean it got somewhere this test
        // does not intend to reach.
        auto const tier = StartConsensusOrExplain(
            cfg,
            noScheduler,
            "127.0.0.1:6674",
            identity,
            membership,
            *Unwrap(roster),
            SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
            core::platform::defaultSystemWallClock(),
            metrics,
            logger);
        REQUIRE_FALSE(tier.has_value());
        CHECK(tier.error() == ConsensusNeedsNodeIdRefusal);
    }

    SECTION("an id and no address its peers dial is refused for the address")
    {
        // The other half of the self record, refused by ITS name: the id alone is not a member
        // anybody can reach. And the two refusals are told apart, so neither half of the check
        // can go missing behind the other.
        auto cfg = Testing::FirstStart(NodeConfig {});
        cfg.raftListen = "6680";
        cfg.nodeId = "n1";
        NodeMembership membership { cfg, membershipLog };

        auto const tier = StartConsensusOrExplain(
            cfg,
            noScheduler,
            "127.0.0.1:6674",
            identity,
            membership,
            *Unwrap(roster),
            SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
            core::platform::defaultSystemWallClock(),
            metrics,
            logger);
        REQUIRE_FALSE(tier.has_value());
        CHECK(tier.error() == ConsensusNamesNoDialAddressRefusal);
    }
}

TEST_CASE("A consensus tier refuses to start without an identity key", "[node][consensus][handshake]")
{
    // #178. Every connection between members proves each end's OWN key before a message is
    // read, so a node with no key is not a degraded member but one that can neither be heard
    // nor hear anybody -- and a tier that started one anyway would be the port open with every
    // refusal counter reading zero. Decided HERE, once, before anything is bound.
    //
    // No I/O: the refusal returns before a directory, a listener or a reactor exists. The
    // configuration otherwise gets past every earlier gate, so the refusal observed is the
    // identity key's and not the dial-address rule's.
    NullLogger logger;
    AtomicMetricsSink metrics;
    std::unique_ptr<SchedulerTier> const noScheduler;

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = "6680";
    cfg.raftSelf = "10.0.0.1";
    NodeMembership membership { cfg, membershipLog };
    auto const roster =
        NodeRoster::Build(Testing::FirstStart(NodeConfig {}), core::platform::defaultSystemWallClock(), metrics, logger);
    REQUIRE(roster.has_value());
    // Never asked to open: these cases' clusters name no shared cache.
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };

    auto const tier =
        StartConsensusOrExplain(cfg,
                                noScheduler,
                                "127.0.0.1:6674",
                                std::nullopt,
                                membership,
                                *Unwrap(roster),
                                SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
                                core::platform::defaultSystemWallClock(),
                                metrics,
                                logger);
    REQUIRE_FALSE(tier.has_value());
    CHECK(tier.error() == ConsensusNeedsIdentityKeyRefusal);
}

TEST_CASE("What a node reports about its own quorum is one read of the driver", "[node][consensus][observability]")
{
    // #435. `ConsensusTier::Status()` is `ConsensusStatusFrom` over a single
    // `RaftDriver::Progress`, and the mapping is split out for the reason
    // `QuorumProposalPending` is: acquiring a `Progress` needs a reactor, a peer
    // listener, a state directory and somebody to elect this node, while turning one
    // into what a scrape reports needs none of that. Folded together, the answer this
    // ticket is about would be reachable only from a running cluster.
    //
    // Every field is asserted, not a representative one. The failure this maps
    // against is a scrape that reports a healthy-looking cluster because one field
    // was dropped or crossed -- and a crossed `term`/`commitIndex` is exactly the
    // shape `Consensus::Term` and `Consensus::LogIndex` are distinct types to
    // prevent, so a case checking only "something came back" would pass under it.
    auto const progress =
        Consensus::RaftDriver::Progress { .configuration = { .voters = { "n1", "n3", "n2" }, .learners = { "n5", "n4" } },
                                          .commitIndex = Consensus::LogIndex { .value = 12 },
                                          .term = Consensus::Term { .value = 4 },
                                          .role = Consensus::Role::Leader,
                                          .knownLeader = Consensus::NodeId { "n1" },
                                          .matchIndex = {},
                                          .installRefusal = std::nullopt };

    auto const status = ConsensusStatusFrom(progress);

    // Verbatim, in the order consensus holds it: a caller comparing two nodes needs
    // to see the order a configuration was adopted in, and sorting here would take
    // that away with nothing saying so.
    CHECK(status.configuration.voters == std::vector<Consensus::NodeId> { "n1", "n3", "n2" });
    CHECK(status.configuration.learners == std::vector<Consensus::NodeId> { "n5", "n4" });
    CHECK(status.knownLeader == Consensus::NodeId { "n1" });
    CHECK(status.term.value == 4);
    CHECK(status.commitIndex.value == 12);
    CHECK(status.role == Consensus::Role::Leader);
}

TEST_CASE("A node that has adopted no configuration reports an empty set, not a leader-less nothing",
          "[node][consensus][observability]")
{
    // The #388 shape, carried through the mapping. A joiner that received the
    // ClusterState record and never adopted the CONFIGURATION entry counts nobody --
    // and it can still name a leader, because a node with no cluster accepts entries
    // from any leader (`RaftNode::HasCluster`).
    //
    // So `knownLeader` is NOT constrained to `members`, and the pairing is the
    // diagnosis: a node naming a leader while counting nobody is admitted to the
    // fleet and absent from the quorum, which is invisible while that leader lives.
    auto const status = ConsensusStatusFrom(Consensus::RaftDriver::Progress { .configuration = {},
                                                                              .commitIndex = Consensus::LogIndex {},
                                                                              .term = Consensus::Term {},
                                                                              .role = Consensus::Role::Follower,
                                                                              .knownLeader = Consensus::NodeId { "n1" },
                                                                              .matchIndex = {},
                                                                              .installRefusal = std::nullopt });

    CHECK(status.configuration.voters.empty());
    CHECK(status.configuration.learners.empty());
    CHECK(status.knownLeader == Consensus::NodeId { "n1" });
    CHECK(status.role == Consensus::Role::Follower);
    CHECK(status.term.value == 0);
}

TEST_CASE("A node that runs no consensus hands a scrape nothing to call", "[node][consensus][observability]")
{
    // The branch `main.cpp` cannot be asked about, which is why it is a function
    // rather than a ternary in `WorkerBody`: that translation unit is in no test
    // target, so spelled there this decision was unreachable AND it took the
    // enclosing function past clang-tidy's cognitive-complexity ceiling.
    //
    // What is asserted is that the source is DISENGAGED rather than a callable that
    // answers a default `ConsensusStatus`. The two are not interchangeable and the
    // difference is the whole of #435's absence rule: a disengaged source leaves
    // `MetricsSnapshot::consensus` empty and the renderer emits no consensus series
    // at all, while a callable returning `ConsensusStatus {}` would report a node
    // that runs consensus and counts nobody -- which is the #388 fault state being
    // claimed about every daemon and every node started without `--listen-raft`.
    CHECK_FALSE(static_cast<bool>(ConsensusScrapeSource(nullptr)));
}

TEST_CASE("A running one-voter tier refuses to forget its only voter, and nothing reaches the log",
          "[node][consensus][forget]")
{
    // #1539's refusal, at the door an operator's `--cluster-forget` reaches: a real tier's
    // `ProposeToCluster`, over a real listener, a real state directory and a driver that
    // elected itself -- because a refusal decided by a function nothing calls is the bug it
    // was written to fix. The policy cases pin `PrepareForget`; this pins that `Propose`
    // asks it before anything is appended.
    NullLogger logger;
    AtomicMetricsSink metrics;

    // Port 0 is refused by the member grammar, so bind an ephemeral one the ordinary way.
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    Testing::ScratchDirectory const scratchDirectory { "consensus-forget-only-voter" };
    auto const& scratch = scratchDirectory.Path();
    {
        auto key = std::ofstream { scratch / "cluster.key", std::ios::binary };
        key << std::string(32, 'k');
    }

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    // Asked of ONE state value: `ClusterState()` answers by value, so two calls are two
    // vectors whose iterators cannot be compared.
    auto const records = [](Cluster::ClusterState const& state, Consensus::NodeId const& id) {
        return std::ranges::find(state.members, id, &Cluster::ClusterMember::id) != state.members.end();
    };
    auto const self = Consensus::NodeId { "n1" };

    auto started = ConsensusTier::Start(
        cfg,
        {},
        Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        core::platform::defaultSystemWallClock(),
        {},
        metrics,
        logger);
    REQUIRE(started.has_value());
    auto const& tier = *started;

    // Leading, and settled: its own record committed, after which its reconciler proposes
    // nothing -- so the commit index moves only for what this case proposes.
    REQUIRE(Testing::WaitUntil(
        "the one-voter tier to lead and record itself",
        [&tier, &records, &self] {
            return tier->Status().role == Consensus::Role::Leader && records(tier->ClusterState(), self);
        },
        [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
    auto const before = tier->Status().commitIndex;

    // CHECK rather than REQUIRE, and the error read only when there is one: the log
    // assertion below has to be REACHED when the refusal is missing, or a tier that
    // appended the forget would stop this case before the half that measures it.
    auto const refused = tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::Forget,
                                                                   .key = "n1",
                                                                   .value = {},
                                                                   .schedulerEndpoint = {},
                                                                   .publicKey = std::nullopt,
                                                                   .role = std::nullopt });
    CHECK_FALSE(refused.has_value());
    if (!refused.has_value())
    {
        CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);
        CHECK(refused.error().context.starts_with("cannot forget n1: it is the cluster's only voter"));
    }

    // Nothing reached the log, measured rather than assumed: the next entry proposed lands
    // exactly one index later. Had the forget been appended, a one-voter cluster would
    // have committed it at once and this entry would sit two along.
    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                                      .key = "lease-lifetime",
                                                      .value = "20min",
                                                      .schedulerEndpoint = {},
                                                      .publicKey = std::nullopt,
                                                      .role = std::nullopt })
                .has_value());
    REQUIRE(Testing::WaitUntil(
        "the sentinel setting to commit",
        [&tier] { return tier->ClusterState().SettingOf("lease-lifetime") == "20min"; },
        [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
    CHECK(tier->Status().commitIndex.value == before.value + 1);
    CHECK(records(tier->ClusterState(), self));
}

TEST_CASE("Every state a consensus tier applies reaches the shared-cache directory, host and upstream, in order",
          "[node][consensus][shared-cache]")
{
    // The apply half of the shared cache's production wiring. `StartConsensusOrExplain` is what
    // `main` calls, and its observer is the only thing that tells the three what the cluster agreed:
    // a host nobody told would leave the machine the fleet named answering not-shared-cache forever;
    // a directory nobody told would send every other node's reads nowhere; and an upstream told
    // BEFORE the directory would re-judge the previous state. So a real one-voter tier, over a real
    // listener and state directory, is asked to name itself.
    NullLogger logger;
    AtomicMetricsSink metrics;
    std::unique_ptr<SchedulerTier> const noScheduler;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    Testing::ScratchDirectory const scratch { "consensus-shared-cache-apply" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch.Path() / "state";

    NodeMembership membership { cfg, membershipLog };
    auto const roster =
        NodeRoster::Build(Testing::FirstStart(NodeConfig {}), core::platform::defaultSystemWallClock(), metrics, logger);
    REQUIRE(roster.has_value());
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };
    RecordingUpstream upstream { directory };

    auto started =
        StartConsensusOrExplain(cfg,
                                noScheduler,
                                {},
                                Testing::TestKeyPair("n1"),
                                membership,
                                *Unwrap(roster),
                                SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = &upstream },
                                core::platform::defaultSystemWallClock(),
                                metrics,
                                logger);
    REQUIRE(started.has_value());
    auto const& tier = *started;
    REQUIRE(tier != nullptr);

    // The setting is validated against a live key, so the tier must first have recorded itself.
    REQUIRE(Testing::WaitUntil(
        "the one-voter tier to lead and record itself",
        [&tier] {
            auto const state = tier->ClusterState();
            return tier->Status().role == Consensus::Role::Leader
                   && std::ranges::find(state.members, Consensus::NodeId { "n1" }, &Cluster::ClusterMember::id)
                          != state.members.end();
        },
        [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
    // Nothing named yet: the host has opened nothing.
    sharedCache.Reconcile();
    CHECK(sharedCache.Current() == nullptr);

    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                                      .key = std::string { Cluster::SharedCacheSetting },
                                                      .value = "n1",
                                                      .schedulerEndpoint = {},
                                                      .publicKey = std::nullopt,
                                                      .role = std::nullopt })
                .has_value());
    // Reconciled on this thread, as `ReconcileOn::Caller` means: what is waited for is the APPLY
    // reaching the host, and the host acts only when asked.
    CHECK(Testing::WaitUntil(
        "the applied setting to reach the host",
        [&sharedCache] {
            sharedCache.Reconcile();
            return sharedCache.Current() != nullptr;
        },
        [&sharedCache] { return std::format("named {}", sharedCache.Status().named); }));
    CHECK(opener.opens == 1);

    // The directory saw it too, and the upstream was told AFTER the directory: the last source it
    // read at an apply is the one the setting named.
    CHECK(directory.Current().source == CompileCacheWire::WireSharedCacheSource::ThisMachine);
    auto const seen = upstream.Seen();
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == CompileCacheWire::WireSharedCacheSource::ThisMachine);
}

TEST_CASE("A leader counts its sends to a learner with no endpoint as a learner with no session",
          "[node][consensus][learner][formation]")
{
    // The link column at the node's door: `LearnMembers` reads each member's seat's link out of
    // the state and tells the transport which peers dial in, so a message for a learner nobody
    // dials lands on the no-session row -- never on the row for a peer nothing can reach, which
    // would tell an operator a laptop that is merely shut is an id this node was never given.
    // A real tier, because a placement made by a function nothing calls is no placement.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    auto const scratch = Testing::UniqueScratchPath("consensus-learner-no-endpoint");
    std::filesystem::create_directories(scratch);

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    auto started = ConsensusTier::Start(
        cfg,
        {},
        Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        core::platform::defaultSystemWallClock(),
        {},
        metrics,
        logger);
    REQUIRE(started.has_value());
    auto const& tier = *started;
    REQUIRE(Testing::WaitUntil(
        "the one-voter tier to lead",
        [&tier] { return tier->Status().role == Consensus::Role::Leader; },
        [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));

    // A learner recorded with no endpoint at all: it dials in, so it needs none.
    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                                      .key = "laptop",
                                                      .value = {},
                                                      .schedulerEndpoint = {},
                                                      .publicKey = Testing::TestKeyPair("laptop").PublicKey(),
                                                      .role = std::nullopt })
                .has_value());

    // The reconciler adds it to the configuration as a learner, and the leader then has
    // something to send it that nothing can carry.
    auto const drops = [&metrics] {
        return metrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession)
               + metrics.Read(IMetricsSink::Counter::RaftSendsDroppedUnknownPeer);
    };
    REQUIRE(Testing::WaitUntil(
        "the leader to replicate to the learner it cannot reach",
        [&drops] { return drops() > 0; },
        [&tier] {
            return std::format("{} learner(s) in the configuration", tier->Status().configuration.learners.size());
        }));
    CHECK(tier->Status().configuration.learners == std::vector<Consensus::NodeId> { "laptop" });
    CHECK(metrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession) > 0);
    CHECK(metrics.Read(IMetricsSink::Counter::RaftSendsDroppedUnknownPeer) == 0);
}

TEST_CASE("The peers that dial in are read from the record and from the configuration consensus holds",
          "[node][consensus][learner][formation]")
{
    // Both, through the one column. The RECORD alone misses a learner whose admission sits in
    // the log tail a restarted node has not applied yet, while the configuration it recovered
    // already counts it -- and a leader sends to that learner the moment it leads.
    auto state = Cluster::ClusterState {};
    Apply(state,
          Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                             .key = "tablet",
                             .value = {},
                             .schedulerEndpoint = {},
                             .publicKey = Testing::TestKeyPair("tablet").PublicKey(),
                             .role = std::nullopt });
    Apply(state,
          Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                             .key = "n1",
                             .value = "10.0.0.1:6680",
                             .schedulerEndpoint = {},
                             .publicKey = Testing::TestKeyPair("n1").PublicKey(),
                             .role = std::nullopt });
    auto const configuration = Consensus::Configuration { .voters = { "n1", "n2" }, .learners = { "laptop" } };

    // `laptop` from the configuration, `tablet` from the record; neither voter.
    CHECK(DialInPeers(state, configuration) == std::vector<Consensus::NodeId> { "laptop", "tablet" });

    // And a member both place is named once.
    auto const both = Consensus::Configuration { .voters = { "n1" }, .learners = { "tablet" } };
    CHECK(DialInPeers(state, both) == std::vector<Consensus::NodeId> { "tablet" });

    // The two changes in flight, where the record and the configuration disagree. A recorded
    // VOTER consensus still counts as a learner while it catches up (#1537) is placed by the
    // configuration alone; a recorded LEARNER consensus still counts as a voter while its
    // demotion is uncommitted is placed by the record alone. One with an address is dialled
    // first anyway, so the placement only decides which row a drop lands on.
    auto const catchingUp = Consensus::Configuration { .voters = { "n2" }, .learners = { "n1" } };
    CHECK(DialInPeers(state, catchingUp) == std::vector<Consensus::NodeId> { "n1", "tablet" });
    auto const demoting = Consensus::Configuration { .voters = { "n1", "tablet" }, .learners = {} };
    CHECK(DialInPeers(state, demoting) == std::vector<Consensus::NodeId> { "tablet" });
}

TEST_CASE("A restarted leader counts its sends to a learner it has not re-applied as a learner with no session",
          "[node][consensus][learner][formation]")
{
    // The restart order: a node recovers the configuration from its log at once, and the
    // commands only once it has led and committed again. So a learner admitted in the log's tail
    // is counted -- and sent to -- before the applied state records it, and until the next
    // reconcile pass after that commit, placement read from the record alone put every such
    // drop on the row whose help text says no wait fixes it.
    NullLogger logger;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    auto const scratch = Testing::UniqueScratchPath("consensus-learner-restart");
    std::filesystem::create_directories(scratch);

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    auto const start = [&cfg, &logger](IMetricsSink& metrics) {
        return ConsensusTier::Start(
            cfg,
            {},
            Testing::TestKeyPair("n1"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            core::platform::defaultSystemWallClock(),
            {},
            metrics,
            logger);
    };

    // First life: admit the learner and let the reconciler put it in the configuration.
    {
        AtomicMetricsSink metrics;
        auto started = start(metrics);
        REQUIRE(started.has_value());
        auto const& tier = *started;
        REQUIRE(Testing::WaitUntil(
            "the one-voter tier to lead",
            [&tier] { return tier->Status().role == Consensus::Role::Leader; },
            [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
        REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                                          .key = "laptop",
                                                          .value = {},
                                                          .schedulerEndpoint = {},
                                                          .publicKey = Testing::TestKeyPair("laptop").PublicKey(),
                                                          .role = std::nullopt })
                    .has_value());
        REQUIRE(Testing::WaitUntil(
            "the learner to join the configuration",
            [&tier] { return tier->Status().configuration.learners == std::vector<Consensus::NodeId> { "laptop" }; },
            [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
    }

    // Second life, over the same state directory: the configuration is back at once, the record
    // only after this node leads, commits and applies again.
    AtomicMetricsSink metrics;
    auto started = start(metrics);
    REQUIRE(started.has_value());
    auto const& tier = *started;
    auto const records = [&tier] {
        auto const state = tier->ClusterState();
        return std::ranges::contains(state.members, Consensus::NodeId { "laptop" }, &Cluster::ClusterMember::id);
    };
    REQUIRE(Testing::WaitUntil(
        "the restarted leader to apply the learner's record and send to it",
        [&records, &metrics] { return records() && metrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession) > 0; },
        [&tier, &metrics] {
            return std::format("role {}, {} no-session drop(s), {} unknown-peer drop(s)",
                               Consensus::TraitsOf(tier->Status().role).name,
                               metrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession),
                               metrics.Read(IMetricsSink::Counter::RaftSendsDroppedUnknownPeer));
        }));
    CHECK(metrics.Read(IMetricsSink::Counter::RaftSendsDroppedUnknownPeer) == 0);
}

TEST_CASE("A lone voter endorses the roster it applied, under its own key, and re-signs when it changes",
          "[node][consensus][roster]")
{
    // #178, owner decision 3: a lone scheduler runs a one-member consensus, and so it is the
    // one voter whose endorsement certifies the roster its workers adopt. A real tier -- real
    // listener, state directory and a driver that elected itself -- because an endorsement
    // produced by a function nothing calls is the bug it was written to fix.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    Testing::ScratchDirectory const scratchDirectory { "consensus-endorse" };
    auto const& scratch = scratchDirectory.Path();
    {
        auto key = std::ofstream { scratch / "cluster.key", std::ios::binary };
        key << std::string(32, 'k');
    }

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.clusterId = "fleet";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    // Collected off the reconciler thread, read on this one.
    struct Seen
    {
        std::mutex lock;
        std::vector<Cluster::RosterEndorsement> endorsements;
    };
    auto const seen = std::make_shared<Seen>();
    auto started = ConsensusTier::Start(
        cfg,
        {},
        Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        core::platform::defaultSystemWallClock(),
        [seen](Cluster::RosterEndorsement const& endorsement) {
            std::scoped_lock const guard { seen->lock };
            seen->endorsements.push_back(endorsement);
        },
        metrics,
        logger);
    REQUIRE(started.has_value());
    auto const& tier = *started;

    auto const latest = [&seen] {
        std::scoped_lock const guard { seen->lock };
        return seen->endorsements.empty() ? std::optional<Cluster::RosterEndorsement> {}
                                          : std::optional { seen->endorsements.back() };
    };
    auto const count = [&seen] {
        std::scoped_lock const guard { seen->lock };
        return seen->endorsements.size();
    };

    REQUIRE(Testing::WaitUntil(
        "the lone voter to endorse the roster recording itself",
        [&tier, &latest] {
            auto const endorsement = latest();
            return endorsement.has_value() && endorsement->version == tier->ClusterState().rosterVersion;
        },
        [&tier, &count] {
            return std::format("{} endorsement(s), roster version {}", count(), tier->ClusterState().rosterVersion);
        }));

    // What a worker checks, and what distinguishes an endorsement of THIS roster from one of
    // any: the fleet, the digest of the applied state's roster, a lapse one lifetime ahead,
    // and a signature under this node's own key -- not under another machine's.
    auto const first = Unwrap(latest());
    auto const state = tier->ClusterState();
    CHECK(first.clusterId == "fleet");
    CHECK(first.endorser == "n1");
    CHECK(first.rosterDigest == Cluster::DigestOfRoster(Cluster::ProjectRoster(state)));
    CHECK(Cluster::VerifyEndorsement(first, Testing::TestKeyPair("n1").PublicKey()));
    CHECK_FALSE(Cluster::VerifyEndorsement(first, Testing::TestKeyPair("n2").PublicKey()));
    auto const now = std::chrono::system_clock::now();
    CHECK(first.notAfter > now + Cluster::RosterEndorsementLifetime - std::chrono::minutes { 5 });
    CHECK(first.notAfter <= now + Cluster::RosterEndorsementLifetime);

    // An unchanged roster is NOT re-signed every pass: the next endorsement is due a refresh
    // from now. A changed one is re-signed at once -- admitting a principal moves the roster.
    auto const before = count();
    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::AdmitPrincipal,
                                                      .key = "w1",
                                                      .value = {},
                                                      .schedulerEndpoint = {},
                                                      .publicKey = Testing::TestKeyPair("w1").PublicKey(),
                                                      .role = Cluster::PrincipalRole::Worker })
                .has_value());
    REQUIRE(Testing::WaitUntil(
        "the lone voter to endorse the roster that admits w1",
        [&latest, &first] {
            auto const endorsement = latest();
            return endorsement.has_value() && endorsement->version > first.version;
        },
        [&count] { return std::format("{} endorsement(s)", count()); }));
    CHECK(count() == before + 1);
    CHECK(Unwrap(latest()).rosterDigest != first.rosterDigest);
}

namespace
{
/// A worker principal admitted under its own test key: a roster fact that is not a member.
/// @param id The principal's id.
/// @return The command.
[[nodiscard]] Cluster::Command PrincipalCommand(std::string id)
{
    auto const key = Testing::TestKeyPair(id).PublicKey();
    return Cluster::Command { .kind = Cluster::CommandKind::AdmitPrincipal,
                              .key = std::move(id),
                              .value = {},
                              .schedulerEndpoint = {},
                              .publicKey = key,
                              .role = Cluster::PrincipalRole::Worker };
}

/// Whether @p state records a principal under @p id.
/// @param state The state.
/// @param id The id.
/// @return True when a principal carries it.
[[nodiscard]] bool HasPrincipal(Cluster::ClusterState const& state, std::string_view id)
{
    return std::ranges::contains(state.principals, id, &Cluster::ClusterPrincipal::id);
}

/// Write a node's own consensus state into @p directory, as a node that ran would have left it.
///
/// A one-voter cluster of `n1` that compacted through index 3 into a snapshot holding
/// @p snapshotState, and still holds @p retained at index 4 -- written through the
/// store the tier opens, in the order a driver writes it: the log, then the snapshot,
/// which trims what it covers.
/// @param directory The node's state directory.
/// @param snapshotState The application's bytes as of index 3.
/// @param retained The command at index 4, above the snapshot.
void PlantConsensusState(std::filesystem::path const& directory,
                         std::vector<std::byte> snapshotState,
                         std::vector<std::byte> retained)
{
    auto store = Consensus::FileRaftStorage::Open(directory);
    REQUIRE(store.has_value());
    auto const term = Consensus::Term { .value = 1 };
    REQUIRE(store->SaveState(Consensus::PersistentState { .currentTerm = term, .votedFor = std::nullopt }).has_value());

    auto const covered = Cluster::Encode(PrincipalCommand("w0"));
    auto entries = std::vector<Consensus::LogEntry> {};
    for ([[maybe_unused]] auto const index: std::views::iota(1, 4))
        entries.push_back(Consensus::LogEntry { .term = term, .kind = Consensus::EntryKind::Command, .payload = covered });
    entries.push_back(
        Consensus::LogEntry { .term = term, .kind = Consensus::EntryKind::Command, .payload = std::move(retained) });
    // Each written outside the assertion: a `REQUIRE` expands its expression more than
    // once, so a move inside one reads to the analyser as a use after the move.
    auto const logged = store->SaveLog(
        Consensus::LogAppend { .fromIndex = Consensus::LogIndex { .value = 1 }, .entries = std::move(entries) });
    REQUIRE(logged.has_value());

    auto const snapshotted = store->SaveSnapshot(
        Consensus::RaftSnapshot { .lastIncludedIndex = Consensus::LogIndex { .value = 3 },
                                  .lastIncludedTerm = term,
                                  .configuration = Consensus::Configuration { .voters = { "n1" }, .learners = {} },
                                  .state = std::move(snapshotState) });
    REQUIRE(snapshotted.has_value());
}
} // namespace

TEST_CASE("A node whose own consensus state this build cannot read refuses to start, and nothing is applied",
          "[node][consensus][snapshot]")
{
    // The follow-up to #1542, at the door an operator meets it: `ConsensusTier::Start` over
    // a real state directory. A node that upgraded across a change of the cluster state's
    // or the command's encoding restarts over its OWN snapshot and log, written by the
    // build before. Recovery used to hand the snapshot to a machine that could not decode
    // it and ran on with an EMPTY state -- no members, no settings, no revoked keys --
    // and skipped the commands it could not read: removal failing open, loudly but open.
    // It now refuses to start, by name, before anything is applied.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    // Cleared first and removed after, which a bare `UniqueScratchPath` is not: its name is the
    // pid and a counter, Windows reuses pids freely, and a directory an earlier run left behind
    // under the same name holds a log `PlantConsensusState` cannot write over -- every section
    // then failed at its `SaveLog`, in two runs of four on a host holding 2,933 such directories.
    Testing::ScratchDirectory const scratchDirectory { "consensus-unreadable-state" };
    auto const& scratch = scratchDirectory.Path();
    {
        auto key = std::ofstream { scratch / "cluster.key", std::ios::binary };
        key << std::string(32, 'k');
    }

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";
    auto const directory = NodeStateDirectory(cfg);

    // A principal in the snapshot and an admission above it: what a node that ran on an
    // unread snapshot loses, and what one that skipped an unread command loses.
    auto current = Cluster::ClusterState {};
    Cluster::Apply(current, PrincipalCommand("w1"));
    auto const currentSnapshot = Cluster::Encode(current);
    auto const currentCommand = Cluster::Encode(PrincipalCommand("w2"));

    // Anything published at all is something applied: the observer is how the member set
    // reaches the fleet's oracle.
    auto published = std::make_shared<std::atomic<int>>(0);
    auto const start = [&] {
        return ConsensusTier::Start(
            cfg,
            {},
            Testing::TestKeyPair("n1"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [published](Cluster::ClusterState const&) { published->fetch_add(1); },
            core::platform::defaultSystemWallClock(),
            {},
            metrics,
            logger);
    };

    SECTION("control: state this build wrote starts, with the snapshot restored and the entry above it applied")
    {
        PlantConsensusState(directory, currentSnapshot, currentCommand);
        auto started = start();
        REQUIRE(started.has_value());
        auto const& tier = *started;

        // Restored when the driver was built, before either loop ran.
        CHECK(HasPrincipal(tier->ClusterState(), "w1"));
        CHECK(published->load() > 0);

        // And the entry above it, once the node leads again and commits it.
        CHECK(Testing::WaitUntil(
            "the retained admission to commit",
            [&tier] { return HasPrincipal(tier->ClusterState(), "w2"); },
            [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
    }

    SECTION("a snapshot the build before wrote refuses the start, naming the directory, both versions and the remedy")
    {
        PlantConsensusState(directory, Testing::EncodePreviousClusterState(), currentCommand);
        auto const started = start();
        REQUIRE_FALSE(started.has_value());
        auto const& refusal = started.error();
        CAPTURE(refusal);
        CHECK(refusal.contains(directory.string()));
        CHECK(refusal.contains("the snapshot as of log entry 3"));
        CHECK(refusal.contains(std::format("cluster state encoding version {}", Testing::PreviousClusterStateVersion)));
        CHECK(refusal.contains("reads 8"));
        CHECK(refusal.contains("it is intact, and there is no conversion"));
        CHECK(refusal.contains(Consensus::UnreadableStateRemedy));
        CHECK(published->load() == 0);
    }

    SECTION("a retained command the build before wrote refuses the start, before the snapshot is restored")
    {
        // The snapshot is THIS build's, so it would restore -- and must not have been: the
        // commands are asked about first, so a refusal hands the application nothing.
        PlantConsensusState(directory, currentSnapshot, Testing::EncodePreviousClusterCommand());
        auto const started = start();
        REQUIRE_FALSE(started.has_value());
        auto const& refusal = started.error();
        CAPTURE(refusal);
        CHECK(refusal.contains(directory.string()));
        CHECK(refusal.contains("log entry 4"));
        CHECK(refusal.contains(std::format("cluster command encoding version {}", Testing::PreviousClusterCommandVersion)));
        CHECK(refusal.contains("reads 4"));
        CHECK(refusal.contains(Consensus::UnreadableStateRemedy));
        CHECK(published->load() == 0);
    }

    SECTION("a retained command carrying a retired verb refuses the start, naming the retirement")
    {
        // Byte 3 was a client verb an earlier build committed: this build's command version
        // and layout, and a verb it retired. Applied as nothing it would leave this node
        // holding a state its log says otherwise about, so it refuses the start by name.
        auto const retired = Cluster::Encode(Cluster::Command { .kind = Cluster::CommandKind::RetiredForgetClient,
                                                                .key = "10.0.0.7",
                                                                .value = {},
                                                                .schedulerEndpoint = {},
                                                                .publicKey = std::nullopt,
                                                                .role = std::nullopt });
        PlantConsensusState(directory, currentSnapshot, retired);
        auto const started = start();
        REQUIRE_FALSE(started.has_value());
        auto const& refusal = started.error();
        CAPTURE(refusal);
        CHECK(refusal.contains(directory.string()));
        CHECK(refusal.contains("log entry 4"));
        CHECK(refusal.contains("verb 4 is retired"));
        CHECK(refusal.contains(Consensus::UnreadableStateRemedy));
        CHECK(published->load() == 0);
    }
}

TEST_CASE("The remedy for unreadable consensus state names only flags this node has", "[node][consensus][snapshot]")
{
    // The remedy is prose an operator follows at the worst moment, so a flag it names that
    // this binary does not have sends them to `--help` in the middle of a recovery. Asked
    // of the option table, never remembered. A spelling ending in `-` is a family
    // (`--cluster-*`), and at least one row must belong to it.
    auto const remedy = std::string_view { Consensus::UnreadableStateRemedy };
    auto const options = NodeOptions();
    auto named = std::vector<std::string_view> {};
    for (auto const word: std::views::split(remedy, ' '))
    {
        auto const text = std::string_view { word.begin(), word.end() };
        auto const at = text.find("--");
        if (at == std::string_view::npos)
            continue;
        auto const flag = text.substr(at);
        named.push_back(flag.substr(0, flag.find_first_not_of("abcdefghijklmnopqrstuvwxyz-", 2)));
    }

    // A positive control on the scan itself, so an empty list cannot pass as a clean one.
    REQUIRE(std::ranges::find(named, std::string_view { "--cluster-admit" }) != named.end());

    for (auto const flag: named)
    {
        CAPTURE(flag);
        if (flag.ends_with('-'))
            CHECK(std::ranges::any_of(options, [flag](auto const& row) { return row.primary.starts_with(flag); }));
        else
            CHECK(std::ranges::any_of(options, [flag](auto const& row) { return row.primary == flag; }));
    }
}

TEST_CASE("A node refusing its leader's snapshot says so as an Alert condition, and says when it stops",
          "[node][consensus][snapshot][conditions]")
{
    // #1552's surfaces. A node that cannot read what its leader sends stays behind, and from
    // the outside that is a slow node -- so it is a Live Alert row, `unreadable-leader-snapshot`,
    // whose detail says who offered what and why this build cannot read it, and an Error line.
    // Its end is reported too: watching it clear is watching the upgrade land.
    CapturingLogger logger;
    NodeConditions conditions;
    auto const refusal = Consensus::RaftDriver::InstallRefusal {
        .index = Consensus::LogIndex { .value = 812 },
        .leader = "n1",
        .reason = FastCache::UnsupportedFormatVersion("cluster state encoding version 6 (this build reads 5)")
    };

    ReportInstallRefusal(refusal, logger, &conditions);
    REQUIRE(conditions.StateOf(NodeCondition::UnreadableLeaderSnapshot) == CompileCacheWire::ConditionState::Raised);
    auto const rows = conditions.Snapshot();
    auto const row = std::ranges::find(
        rows, RowFor(NodeCondition::UnreadableLeaderSnapshot).id, &CompileCacheWire::NodeConditionFields::id);
    REQUIRE(row != rows.end());
    CHECK(row->detail == DescribeInstallRefusal(refusal));
    CHECK(row->detail.contains("leader n1"));
    CHECK(row->detail.contains("log entry 812"));
    CHECK(row->detail.contains("version 6 (this build reads 5)"));
    CHECK(row->severity == "alert");
    CHECK(row->persistence == "live");

    ReportInstallRefusal(std::nullopt, logger, &conditions);
    CHECK(conditions.StateOf(NodeCondition::UnreadableLeaderSnapshot) == CompileCacheWire::ConditionState::Clear);

    auto const lines = logger.Snapshot();
    CHECK(std::ranges::any_of(
        lines, [](auto const& line) { return line.level == LogLevel::Error && line.message.contains("cannot read"); }));
    CHECK(std::ranges::any_of(
        lines, [](auto const& line) { return line.level == LogLevel::Info && line.message.contains("caught up"); }));
}

namespace
{
/// One read from @p socket. A `core::net::BlockingSocket` blocks rather than suspends, so `core::async::syncRun`
/// completes this in one resume -- and the connector's `ioTimeout` bounds it.
/// @param socket Source; never null.
/// @param buffer Where to put what arrives.
/// @return How many bytes arrived; zero at the end or on an error.
[[nodiscard]] core::async::Task<std::size_t> ReadOnce(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    auto const read = co_await socket->read(buffer);
    co_return read.has_value() ? *read : std::size_t { 0 };
}

/// One write to @p socket, for `ReadOnce`'s reason.
/// @param socket Destination; never null.
/// @param bytes What to send.
/// @return How many bytes were accepted; zero on an error.
[[nodiscard]] core::async::Task<std::size_t> WriteOnce(core::net::ISocket* socket, std::span<std::byte const> bytes)
{
    auto const written = co_await socket->write(bytes);
    co_return written.has_value() ? *written : std::size_t { 0 };
}

/// One peer-wire frame as it arrived: its header and the payload it declared.
struct PeerFrame
{
    Consensus::RaftWire::FrameHeader header {}; ///< What the frame declared.
    std::vector<std::byte> payload;             ///< Exactly `header.payloadLength` bytes.
};

/// Read exactly one peer-wire frame from @p socket, and not a byte of the next.
/// @param socket Where it arrives.
/// @return The frame, or nullopt when the peer ended first or sent no frame.
[[nodiscard]] std::optional<PeerFrame> ReadPeerFrame(core::net::ISocket& socket)
{
    auto bytes = std::vector<std::byte> {};
    auto const fill = [&socket, &bytes](std::size_t want) {
        while (bytes.size() < want)
        {
            auto chunk = std::array<std::byte, 512> {};
            auto const room = std::min(chunk.size(), want - bytes.size());
            auto const got = core::async::syncRun(ReadOnce(&socket, std::span { chunk }.first(room)));
            if (got == 0)
                return false;
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
        }
        return true;
    };

    if (!fill(Consensus::RaftWire::HeaderSize))
        return std::nullopt;
    auto const header = Consensus::RaftWire::DecodeHeader(bytes);
    if (!header.has_value() || !fill(Consensus::RaftWire::HeaderSize + header->payloadLength))
        return std::nullopt;
    return PeerFrame { .header = *header,
                       .payload = std::vector<std::byte> {
                           bytes.begin() + static_cast<std::ptrdiff_t>(Consensus::RaftWire::HeaderSize), bytes.end() } };
}

/// Stand in for leader `n1` on @p port's peer wire and make it @p offer.
///
/// Every step is production's own, in production's order: the dialler's handshake answers
/// the acceptor's challenge with n1's own key, the SIGNED verdict is read back and required
/// to be an acceptance, and only then is the frame sealed, in the session that verdict
/// concluded -- so a case built on this asserts about a message the tier actually read,
/// never about one a failed handshake dropped on the floor. The frame cannot ride behind
/// the proof: the session key exists only once the verdict is concluded.
/// @param port The tier's peer port.
/// @param offer What the leader says.
/// @return The connection, open: the case holds it until it has seen what it waits for,
///         because a close with the tier's reply unread is a reset, which can reach the
///         tier before it has read the offer.
[[nodiscard]] std::unique_ptr<core::net::ISocket> OfferAsLeader(std::uint16_t port, Consensus::RaftMessage const& offer)
{
    Testing::TestPeerIdentity const identity { "n1", Testing::TestKeyPair("n1"), Testing::SharedRoster::Of({ "n1", "n2" }) };
    SystemSecureRandom random;
    auto handshake =
        Consensus::DiallerHandshake::Create(identity, "n2", Consensus::RaftWire::SessionDirection::OneWay, random);
    REQUIRE(handshake.has_value());

    core::net::BlockingConnector connector {
        core::net::defaultAddressResolver(), core::net::BlockingConnectorOptions { .ioTimeout = std::chrono::seconds { 10 } }
    };
    auto dialled = core::async::syncRun(connector.connect(
        "127.0.0.1",
        port,
        core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 }, .keepAlive = core::net::KeepAlive::No }));
    REQUIRE(dialled.has_value());
    auto socket = *std::move(dialled);

    auto const challengeFrame = ReadPeerFrame(*socket);
    REQUIRE(challengeFrame.has_value());
    auto const challenge =
        Consensus::RaftWire::DecodeChallenge(Unwrap(challengeFrame).header, Unwrap(challengeFrame).payload);
    REQUIRE(challenge.has_value());
    auto const proof = handshake->Answer(*challenge);
    REQUIRE(proof.has_value());

    auto const proofWire = Consensus::RaftWire::EncodeProof(*proof);
    REQUIRE(core::async::syncRun(WriteOnce(socket.get(), proofWire)) == proofWire.size());

    auto const verdictFrame = ReadPeerFrame(*socket);
    REQUIRE(verdictFrame.has_value());
    auto const verdict = Consensus::RaftWire::DecodeVerdict(Unwrap(verdictFrame).header, Unwrap(verdictFrame).payload);
    REQUIRE(verdict.has_value());
    auto const conclusion = handshake->Conclude(*verdict);
    REQUIRE(conclusion.outcome == Consensus::VerdictOutcome::Accepted);
    REQUIRE(conclusion.session.has_value());

    auto wire = Consensus::RaftWire::Encode(offer);
    auto sealer = FrameSealer { Unwrap(conclusion.session).diallerToAcceptor };
    auto const frame = std::span<std::byte const> { wire };
    auto const tag =
        sealer.Seal(frame.first(Consensus::RaftWire::HeaderSize), frame.subspan(Consensus::RaftWire::HeaderSize));
    wire.insert(wire.end(), tag.begin(), tag.end());
    REQUIRE(core::async::syncRun(WriteOnce(socket.get(), wire)) == wire.size());
    return socket;
}
} // namespace

TEST_CASE("A running tier offered a snapshot it cannot read raises unreadable-leader-snapshot, and takes nothing on",
          "[node][consensus][snapshot][conditions]")
{
    // #1552's wiring, end to end, at the door a leader reaches: the real peer wire, the
    // tier's own driver and its own `ClusterStateMachine`, and the registry `main` hands
    // it. No second build is needed, because this case IS the leader: it proves `n1`'s own
    // identity key, which the fleet roster the tier joined names, and offers the previous build's
    // cluster state, from the shared builder.
    // Anything between the wire and the condition that stopped carrying the refusal --
    // the driver never asking, the node taking it on, the observer not installed, the
    // report not raising -- leaves the row clear, and this case red.
    NullLogger logger;
    AtomicMetricsSink metrics;
    NodeConditions conditions;

    auto const self = FreeLoopbackPort();
    // Nobody answers here. The tier dials its leader and fails, which is ordinary for a
    // follower that has not reached its leader yet; the leader reaches IT, below.
    auto const leaderPort = FreeLoopbackPort();

    Testing::ScratchDirectory const scratchDirectory { "consensus-unreadable-install" };
    auto const& scratch = scratchDirectory.Path();
    {
        auto key = std::ofstream { scratch / "cluster.key", std::ios::binary };
        key << std::string(32, 'k');
    }

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n2";
    cfg.raftListen = std::format("127.0.0.1:{}", self);
    cfg.raftSelf = "127.0.0.1";
    // A voter of a fleet `n1` founded: the roster the approval carried names `n1` and its key,
    // and never this node.
    cfg.formation = NodeFormationView {
        .mode = Cluster::NodeMode::Voter,
        .clusterId = cfg.clusterId,
        .createdAtUnixSeconds = 0,
        .foundedHere = false,
        .fleetMembers = { Cluster::ClusterMember { .id = "n1",
                                                   .raftEndpoint = std::format("127.0.0.1:{}", leaderPort),
                                                   .schedulerEndpoint = {},
                                                   .schedulerEndpointHistory =
                                                       Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                                   .seat = Cluster::MemberSeat::Voter,
                                                   .publicKey = Testing::TestKeyPair("n1").PublicKey() } },
        .fleetSchedulers = {},
    };
    cfg.clusterDir = scratch / "state";

    auto started = ConsensusTier::Start(
        cfg,
        {},
        Testing::TestKeyPair("n2"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        core::platform::defaultSystemWallClock(),
        {},
        metrics,
        logger,
        &conditions);
    REQUIRE(started.has_value());
    auto const& tier = *started;
    REQUIRE(conditions.StateOf(NodeCondition::UnreadableLeaderSnapshot) == CompileCacheWire::ConditionState::Clear);

    auto const offer = [](std::vector<std::byte> state) {
        return Consensus::RaftMessage { Consensus::InstallSnapshotRequest {
            .term = Consensus::Term { .value = 1 },
            .leaderId = "n1",
            .lastIncludedIndex = Consensus::LogIndex { .value = 3 },
            .lastIncludedTerm = Consensus::Term { .value = 1 },
            .configuration = Consensus::Configuration { .voters = { "n1", "n2" }, .learners = {} },
            .state = std::move(state) } };
    };
    auto const commit = [&tier] {
        return std::format("commit index {}", tier->Status().commitIndex.value);
    };

    SECTION("control: a snapshot this build reads is taken on, and nothing is raised")
    {
        auto current = Cluster::ClusterState {};
        Cluster::Apply(current, PrincipalCommand("w7"));
        auto const connection = OfferAsLeader(self, offer(Cluster::Encode(current)));

        REQUIRE(Testing::WaitUntil(
            "the offered snapshot to be taken on", [&tier] { return HasPrincipal(tier->ClusterState(), "w7"); }, commit));
        CHECK(conditions.StateOf(NodeCondition::UnreadableLeaderSnapshot) == CompileCacheWire::ConditionState::Clear);
        connection->close();
    }

    SECTION("the previous build's state is refused, raised by name, and nothing is taken on")
    {
        auto const connection = OfferAsLeader(self, offer(Testing::EncodePreviousClusterState()));

        REQUIRE(Testing::WaitUntil(
            "the refusal to be raised",
            [&conditions] {
                return conditions.StateOf(NodeCondition::UnreadableLeaderSnapshot)
                       == CompileCacheWire::ConditionState::Raised;
            },
            commit));
        auto const rows = conditions.Snapshot();
        auto const row = std::ranges::find(
            rows, RowFor(NodeCondition::UnreadableLeaderSnapshot).id, &CompileCacheWire::NodeConditionFields::id);
        REQUIRE(row != rows.end());
        CHECK(row->detail.contains("leader n1"));
        CHECK(row->detail.contains("log entry 3"));
        CHECK(row->detail.contains(std::format("version {}", Testing::PreviousClusterStateVersion)));
        CHECK(row->detail.contains("reads 8"));

        // Nothing taken on: not the previous build's member, not a moved commit index.
        CHECK_FALSE(tier->ClusterState().RaftEndpointOf("n1").has_value());
        CHECK(tier->Status().commitIndex == Consensus::LogIndex::BeforeFirst());
        connection->close();
    }
}

TEST_CASE("Only a node that founded its cluster bootstraps it; one that joined a fleet starts empty",
          "[node][consensus][formation]")
{
    // The bootstrap set is the formation record's, never a flag's: a node that founded its
    // cluster starts consensus with itself as the one voter, and a node that joined another's
    // starts with an EMPTY configuration and learns it from the leader. A joiner that
    // bootstrapped the roster it was handed would elect itself among members that never heard
    // of it, and refuse every leader its own configuration does not name.
    //
    // WHAT DISTINGUISHES: the two starts differ in the record alone -- same id, same port
    // shape, same key -- so a tier that bootstraps whatever `BootstrapMembersOf` returns
    // passes the founder and fails the joiner.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto const start = [&logger, &metrics](NodeConfig const& cfg) {
        return ConsensusTier::Start(
            cfg,
            {},
            Testing::TestKeyPair("n2"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            core::platform::defaultSystemWallClock(),
            {},
            metrics,
            logger);
    };

    auto founder = Testing::FirstStart(NodeConfig {});
    founder.nodeId = "n2";
    founder.raftListen = std::format("127.0.0.1:{}", FreeLoopbackPort());
    founder.raftSelf = "127.0.0.1";
    founder.clusterDir = Testing::UniqueScratchPath("consensus-bootstrap-founder") / "state";
    REQUIRE(founder.formation.has_value());

    auto joiner = founder;
    joiner.raftListen = std::format("127.0.0.1:{}", FreeLoopbackPort());
    joiner.clusterDir = Testing::UniqueScratchPath("consensus-bootstrap-joiner") / "state";
    // A voter of a fleet `n1` founded; nobody answers at `n1`'s address, which is ordinary for
    // a node that has not reached its leader yet.
    joiner.formation = NodeFormationView {
        .mode = Cluster::NodeMode::Voter,
        .clusterId = joiner.clusterId,
        .createdAtUnixSeconds = 0,
        .foundedHere = false,
        .fleetMembers = { Cluster::ClusterMember { .id = "n1",
                                                   .raftEndpoint = std::format("127.0.0.1:{}", FreeLoopbackPort()),
                                                   .schedulerEndpoint = {},
                                                   .schedulerEndpointHistory =
                                                       Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                                   .seat = Cluster::MemberSeat::Voter,
                                                   .publicKey = Testing::TestKeyPair("n1").PublicKey() } },
        .fleetSchedulers = {},
    };

    SECTION("the founder bootstraps itself, alone")
    {
        auto started = start(founder);
        REQUIRE(started.has_value());
        auto const status = (*started)->Status();
        CHECK(status.configuration.voters == std::vector<Consensus::NodeId> { "n2" });
        CHECK(status.configuration.learners.empty());
    }

    SECTION("the joiner bootstraps nothing, and waits to be admitted")
    {
        auto started = start(joiner);
        REQUIRE(started.has_value());
        auto const status = (*started)->Status();
        CHECK(status.configuration.voters.empty());
        CHECK(status.configuration.learners.empty());
        CHECK(status.role != Consensus::Role::Leader);
    }
}

TEST_CASE("A node announces the seat its mode holds: a learner is never announced as a voter",
          "[node][consensus][formation][learner]")
{
    // The record a tier announces about itself is the self member `ConsensusSelfMemberOf` read off
    // the mode, with its scheduler endpoint and key filled in. A record built afresh with a voter's
    // seat -- what a bootstrap set of voters once implied -- announced every learner as a voter.
    //
    // WHAT DISTINGUISHES: the two starts differ in the mode alone, so a tier that announces a fixed
    // seat passes one of the two sections and fails the other.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto const joinedAs = [](Cluster::NodeMode mode, std::string_view scratch) {
        auto cfg = Testing::FirstStart(NodeConfig {});
        cfg.nodeId = "n2";
        cfg.raftListen = std::format("127.0.0.1:{}", FreeLoopbackPort());
        cfg.raftSelf = "127.0.0.1";
        cfg.clusterDir = Testing::UniqueScratchPath(scratch) / "state";
        cfg.formation = NodeFormationView {
            .mode = mode,
            .clusterId = cfg.clusterId,
            .createdAtUnixSeconds = 0,
            .foundedHere = false,
            .fleetMembers = { Cluster::ClusterMember { .id = "n1",
                                                       .raftEndpoint = std::format("127.0.0.1:{}", FreeLoopbackPort()),
                                                       .schedulerEndpoint = {},
                                                       .schedulerEndpointHistory =
                                                           Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                                       .seat = Cluster::MemberSeat::Voter,
                                                       .publicKey = Testing::TestKeyPair("n1").PublicKey() } },
            .fleetSchedulers = {},
        };
        return cfg;
    };

    auto const start = [&logger, &metrics](NodeConfig const& cfg) {
        return ConsensusTier::Start(
            cfg,
            {},
            Testing::TestKeyPair("n2"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            core::platform::defaultSystemWallClock(),
            {},
            metrics,
            logger);
    };

    SECTION("a learner's own record carries a learner's seat")
    {
        // Asked of `ConsensusSelfMemberOf`, the record `Start` announces verbatim, rather than of a
        // started tier: a learner binds no consensus port, and `Start` for a mode whose listener is
        // closed is lane 2a's Task 19, which has not landed -- today it refuses before it builds a
        // record. The voter section below is the real tier; the two together pin that the seat
        // follows the mode, and Task 19 owes the learner section its real-tier form.
        auto const cfg = joinedAs(Cluster::NodeMode::Learner, "consensus-seat-learner");
        auto const members = BootstrapMembersOf(cfg);
        auto const self = ConsensusSelfMemberOf(cfg, members, Testing::TestKeyPair("n2").PublicKey());
        INFO("self: " << (self.has_value() ? std::string {} : self.error()));
        REQUIRE(self.has_value());
        CHECK(self->seat == Cluster::MemberSeat::Learner);
        CHECK(self->publicKey == Testing::TestKeyPair("n2").PublicKey());
    }

    SECTION("a voter announces a voter's seat")
    {
        auto started = start(joinedAs(Cluster::NodeMode::Voter, "consensus-seat-voter"));
        INFO("start: " << (started.has_value() ? std::string {} : started.error()));
        REQUIRE(started.has_value());
        REQUIRE(*started != nullptr);
        CHECK((*started)->Self().seat == Cluster::MemberSeat::Voter);
    }
}

TEST_CASE("A shared-cache apply tells the directory before the upstream re-judges from it",
          "[node][consensus][shared-cache]")
{
    // The order the consensus tier's apply callback relies on, asked of the record that holds it: an
    // upstream told first would re-judge the state BEFORE the one just applied.
    NullLogger logger;
    Testing::MemoryOpener opener;
    SharedCacheHost host { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };
    RecordingUpstream upstream { directory };
    SharedCacheListeners const listeners { .directory = directory, .host = host, .upstream = &upstream };

    listeners.Applied(Testing::NamingSharedCache("n1"));
    CHECK(upstream.Seen() == std::vector { CompileCacheWire::WireSharedCacheSource::ThisMachine });
    host.Reconcile();
    CHECK(host.Current() != nullptr); // and the host was told the same state

    // A node with no private tier has no upstream to tell, and the other two are still told.
    SharedCacheListeners const noTier { .directory = directory, .host = host, .upstream = nullptr };
    noTier.Applied(Cluster::ClusterState {});
    CHECK(directory.Current().source == CompileCacheWire::WireSharedCacheSource::None);
}
