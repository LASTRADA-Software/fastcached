// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "EnrollClient.hpp"
#include "EnrollmentResponder.hpp"
#include "EnrollmentWindow.hpp"
#include "LocalCache.hpp"
#include "NodeAnnounce.hpp"
#include "NodeFormation.hpp"
#include "NodeIdentity.hpp"
#include "NodeMembership.hpp"
#include "NodeRoster.hpp"
#include "SchedulerTier.hpp"
#include "SchedulingRedirect.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/PreviousClusterState.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
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

namespace
{
/// Where a tier under test says its `0xFC` port answers: what its founding record states. One object
/// for the whole binary, so it outlives every tier a case starts. A name another machine could dial,
/// because a member record holds no other (`IsPeerDialableEndpoint`); nothing in this file dials it.
/// @return The source.
[[nodiscard]] FastCache::Cc::IAdvertisedEndpointSource const& TestAdvertised()
{
    static FastCache::Node::AnnouncedEndpoint const advertised { "office:6674" };
    return advertised;
}

/// The steady clock a roster under test measures leader silence on. One object for the whole
/// binary, so it outlives every roster a case builds; no case here waits out the bound.
/// @return The clock.
[[nodiscard]] ::core::platform::IClock const& RosterClock()
{
    static ::core::platform::SteadyClock const clock;
    return clock;
}

/// What a refusal said, for the line that asserts there was none.
///
/// A bare `REQUIRE(x.has_value())` prints `false` and drops the one fact a failure on another
/// machine needs: two of this file's starts failed intermittently with nothing recorded
/// but the line number, and the cause -- a state directory an earlier process of the same pid left
/// behind (`Testing::UniqueScratchPath`) -- was in the error text all along.
/// @param outcome A start's or a store's result.
/// @return The refusal's text, or `(none)` when it succeeded.
template <typename T, typename E>
[[nodiscard]] std::string RefusalOf(std::expected<T, E> const& outcome)
{
    if (outcome.has_value())
        return "(none)";
    if constexpr (requires { outcome.error().context; })
        return outcome.error().context;
    else if constexpr (requires { outcome.error().reason; })
        return outcome.error().reason;
    else
        return outcome.error();
}
} // namespace

TEST_CASE("A scratch path an earlier process of the same pid left behind is handed out empty", "[testing][scratch]")
{
    // The mechanism behind two of this file's intermittent failures, at the seam that owns it. A
    // pid is reused, so `<prefix>-<pid>-<counter>` is a path an earlier process may have left a
    // Raft log, a snapshot and a key in; the next process to draw that pid opened them as its own
    // and failed SaveLog, or refused its tier's start. Planted here at exactly the path this
    // process is handed next, as that earlier process would have left it.
    auto const first = Testing::UniqueScratchPath("scratch-reuse");
    auto const name = first.filename().string();
    auto const dash = name.rfind('-');
    REQUIRE(dash != std::string::npos);
    auto const next = first.parent_path() / std::format("{}-{}", name.substr(0, dash), std::stoi(name.substr(dash + 1)) + 1);

    std::filesystem::create_directories(next / "state");
    {
        auto log = std::ofstream { next / "state" / "raft-log", std::ios::binary };
        log << "an earlier process's log";
    }
    REQUIRE(std::filesystem::exists(next / "state" / "raft-log"));

    auto const handed = Testing::UniqueScratchPath("scratch-reuse");
    REQUIRE(handed == next); // the plant is where the leftover would be, or this asserts nothing
    CHECK_FALSE(std::filesystem::exists(handed));
}

TEST_CASE("Two threads drawing scratch paths at once never draw the same one", "[testing][scratch]")
{
    // The counter separates callers within one process, and a path is CLEARED as it is handed out --
    // so two threads drawing one number would not merely share a directory: the second would delete
    // the first's live one. Every draw from both threads, released together, is distinct.
    //
    // **This case discriminates only under ThreadSanitizer.** In an ordinary build a plain counter
    // passes it too: 400 draws need not collide, and a lost update is not observable on demand. Under
    // TSan (`tsan-gate.sh`, whose node row runs this binary) the unsynchronised increment from the two
    // latch-released threads is reported whether or not a value collides -- which is what proves the
    // counter atomic. Its green in any other build is not that evidence.
    static constexpr auto drawsPerThread = 200;
    auto drawn = std::array<std::vector<std::filesystem::path>, 2> {};
    auto refused = std::array<std::string, 2> {};
    {
        std::latch start { 2 };
        // A leftover the seam cannot clear THROWS, and a throw out of a thread is `std::terminate`
        // rather than a red: so it is caught here and asserted on the case's thread.
        auto const draw = [&start](std::vector<std::filesystem::path>& into, std::string& why) {
            start.arrive_and_wait();
            try
            {
                for ([[maybe_unused]] auto const index: std::views::iota(0, drawsPerThread))
                    into.push_back(Testing::UniqueScratchPath("scratch-threads"));
            }
            catch (std::exception const& error)
            {
                why = error.what();
            }
        };
        std::jthread const first { draw, std::ref(drawn[0]), std::ref(refused[0]) };
        std::jthread const second { draw, std::ref(drawn[1]), std::ref(refused[1]) };
    }
    CHECK(refused[0].empty());
    CHECK(refused[1].empty());
    auto all = drawn[0];
    all.insert(all.end(), drawn[1].begin(), drawn[1].end());
    REQUIRE(all.size() == 2 * drawsPerThread);
    std::ranges::sort(all);
    CHECK(std::ranges::adjacent_find(all) == all.end());
}

TEST_CASE("A log line says a learner has no consensus endpoint rather than leaving a blank", "[node][consensus][learner]")
{
    CHECK(DescribeConsensusEndpoint("10.0.0.1:6680") == "at 10.0.0.1:6680");
    CHECK(DescribeConsensusEndpoint("") == "with no consensus endpoint");
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
    auto const roster = NodeRoster::Build(Testing::FirstStart(NodeConfig {}), RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    // Never asked to open: these cases' clusters name no shared cache.
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };
    // Never told anything: no tier here applies a state.
    AppliedSchedulers schedulers { Testing::FirstStart(NodeConfig {}), AsConfigured };
    KnownSchedulingLeader knownLeader;
    SchedulingLeaderPublisher schedulingLeader { knownLeader };

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
            TestAdvertised(),
            std::nullopt,
            membership,
            *Unwrap(roster),
            schedulers,
            schedulingLeader,
            SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
            metrics,
            logger,
            nullptr,
            FormationHooks {});
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
            TestAdvertised(),
            identity,
            membership,
            *Unwrap(roster),
            schedulers,
            schedulingLeader,
            SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
            metrics,
            logger,
            nullptr,
            FormationHooks {});
        REQUIRE_FALSE(tier.has_value());
        CHECK(tier.error().reason == ConsensusNeedsNodeIdRefusal);
        CHECK(tier.error().cause == NodeRefusalCause::EarlierRule);
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
            TestAdvertised(),
            identity,
            membership,
            *Unwrap(roster),
            schedulers,
            schedulingLeader,
            SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
            metrics,
            logger,
            nullptr,
            FormationHooks {});
        REQUIRE_FALSE(tier.has_value());
        CHECK(tier.error().reason == ConsensusNamesNoDialAddressRefusal);
        CHECK(tier.error().cause == NodeRefusalCause::EarlierRule);
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
    auto const roster = NodeRoster::Build(Testing::FirstStart(NodeConfig {}), RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    // Never asked to open: these cases' clusters name no shared cache.
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };
    // Never told anything: no tier here applies a state.
    AppliedSchedulers schedulers { Testing::FirstStart(NodeConfig {}), AsConfigured };
    KnownSchedulingLeader knownLeader;
    SchedulingLeaderPublisher schedulingLeader { knownLeader };

    auto const tier =
        StartConsensusOrExplain(cfg,
                                noScheduler,
                                TestAdvertised(),
                                std::nullopt,
                                membership,
                                *Unwrap(roster),
                                schedulers,
                                schedulingLeader,
                                SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
                                metrics,
                                logger,
                                nullptr,
                                FormationHooks {});
    REQUIRE_FALSE(tier.has_value());
    CHECK(tier.error().reason == ConsensusNeedsIdentityKeyRefusal);
    CHECK(tier.error().cause == NodeRefusalCause::EarlierRule);
}

TEST_CASE("A listener handed to consensus is served only on the configured address, and closed when refused",
          "[node][consensus]")
{
    // The cases above choose their peer port by binding port 0 and hand the tier that socket,
    // so the port is never free between choosing and serving it. What the tier owes such a
    // caller is checked here: a socket on any ADDRESS but the configured one -- another port, or
    // the right port on another host -- is refused by name, since every peer dials the configured
    // address and serving another would be a node nobody can reach that reports itself up; and a
    // refused socket is closed, not leaked.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto held = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(held);
    REQUIRE(held->IsBound());
    auto const heldPort = held->boundPort();

    // The port the configuration names, which is NOT the held one. Nothing binds it here: the
    // refusal comes before the tier binds anything.
    auto const namedPort = [] {
        auto probe = BlockingListener::Bind("127.0.0.1", 0);
        REQUIRE(probe);
        REQUIRE(probe->IsBound());
        return probe->boundPort();
    }();
    REQUIRE(namedPort != heldPort);

    Testing::ScratchDirectory const scratch { "consensus-handed-listener" };
    // A first start: a cluster of one, founded here, whose formation record is the member list
    // the retired --raft-peer used to spell.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", namedPort);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    auto const start = [&](std::unique_ptr<BlockingListener> listener) {
        return ConsensusTier::Start(
            cfg,
            TestAdvertised(),
            Testing::TestKeyPair("n1"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            ConsensusTier::LeaderContactObserver {},
            metrics,
            logger,
            nullptr,
            FormationHooks {},
            std::move(listener));
    };

    SECTION("a socket on another port is refused, naming both addresses, and closed")
    {
        auto const started = start(std::move(held));
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().reason.contains(std::format("bound to 127.0.0.1:{}", heldPort)));
        CHECK(started.error().reason.contains(std::format("names 127.0.0.1:{}", namedPort)));
        // Closed: the port it held can be claimed again, which a still-open listener forbids.
        auto const rebound = BlockingListener::Bind("127.0.0.1", heldPort);
        REQUIRE(rebound);
        CHECK(rebound->IsBound());
    }

    SECTION("a listener that never bound is refused, with its own reason")
    {
        // Bound to the held port while it is held, which an exclusive claim refuses.
        auto unbound = BlockingListener::Bind("127.0.0.1", heldPort);
        REQUIRE(unbound);
        REQUIRE_FALSE(unbound->IsBound());
        auto const started = start(std::move(unbound));
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().reason.contains("the listener handed to consensus is not bound"));
    }

    SECTION("the right port on another host is refused, naming both addresses")
    {
        // The configuration names every interface; the socket answers loopback alone. Same port,
        // so only the HOST comparison can refuse it.
        cfg.raftListen = std::format("0.0.0.0:{}", heldPort);
        auto const started = start(std::move(held));
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().reason.contains(std::format("bound to 127.0.0.1:{}", heldPort)));
        CHECK(started.error().reason.contains(std::format("names 0.0.0.0:{}", heldPort)));
    }

    SECTION("a configured host NAME is refused rather than matched by its spelling")
    {
        cfg.raftListen = std::format("localhost:{}", heldPort);
        auto const started = start(std::move(held));
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().reason.contains("names its host by name"));
    }
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
                                          .lastLeaderContact = std::nullopt,
                                          .matchIndex = {},
                                          .installRefusal = std::nullopt,
                                          .appliedIndex = {},
                                          .lastLogIndex = {} };

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

TEST_CASE("A pass reads who leads and when it last spoke, and leaves whether it counts to the roster",
          "[node][consensus][isolation]")
{
    // RAW: leading is contact at the pass's instant (CheckQuorum deposes a leader whose quorum stops
    // answering); otherwise the leader the driver names and its last accepted contact -- whatever
    // the driver's ACTIVE configuration says of that leader, since the rule counts a leader against
    // the APPLIED configuration, which only the roster holds.
    auto const now = ::core::platform::SteadyTimePoint { std::chrono::hours { 3 } };
    auto const heard = now - std::chrono::minutes { 7 };
    auto const progress = [&heard](Consensus::Role role, std::optional<Consensus::NodeId> leader) {
        return Consensus::RaftDriver::Progress { .configuration = { .voters = { "n1", "n2" }, .learners = { "n5" } },
                                                 .commitIndex = Consensus::LogIndex { .value = 9 },
                                                 .term = Consensus::Term { .value = 3 },
                                                 .role = role,
                                                 .knownLeader = std::move(leader),
                                                 .lastLeaderContact = heard,
                                                 .matchIndex = {},
                                                 .installRefusal = std::nullopt,
                                                 .appliedIndex = {},
                                                 .lastLogIndex = {} };
    };

    auto const leading = LeaderReadingOf(progress(Consensus::Role::Leader, Consensus::NodeId { "n1" }), now);
    CHECK(leading.leads);
    CHECK(leading.silentFor == std::optional { ::core::platform::SteadyTimePoint::duration::zero() });
    auto const following = LeaderReadingOf(progress(Consensus::Role::Follower, Consensus::NodeId { "n2" }), now);
    CHECK_FALSE(following.leads);
    CHECK(following.leader == std::optional<std::string> { "n2" });
    CHECK(following.silentFor == std::optional<::core::platform::SteadyTimePoint::duration> { now - heard });
    // A leader the ACTIVE configuration seats as a learner, or does not hold at all, is passed on as
    // read: the roster, not the driver's configuration, decides whether it counts.
    CHECK(LeaderReadingOf(progress(Consensus::Role::Follower, Consensus::NodeId { "n5" }), now).leader
          == std::optional<std::string> { "n5" });
    CHECK(LeaderReadingOf(progress(Consensus::Role::Follower, Consensus::NodeId { "n9" }), now).leader
          == std::optional<std::string> { "n9" });
    // Nobody known to lead.
    CHECK_FALSE(LeaderReadingOf(progress(Consensus::Role::Follower, std::nullopt), now).leader.has_value());
}

TEST_CASE("Silence is measured on the roster's clock, whatever clock the driver stamps contact on",
          "[node][consensus][isolation]")
{
    // The driver stamps contact on its own reactor clock and the roster measures on the clock it was
    // given: the reading crosses as an AGE, so two clocks that disagree about the instant agree about
    // the silence. Here the driver's clock reads ten minutes and its leader spoke a minute ago, while
    // the roster's reads three hours: an instant handed across would be two hours fifty-one minutes
    // stale -- isolated -- where the leader in fact spoke a minute ago.
    auto const driverNow = ::core::platform::SteadyTimePoint { std::chrono::minutes { 10 } };
    auto const progress = Consensus::RaftDriver::Progress { .configuration = { .voters = { "n1" }, .learners = {} },
                                                            .commitIndex = Consensus::LogIndex { .value = 9 },
                                                            .term = Consensus::Term { .value = 3 },
                                                            .role = Consensus::Role::Follower,
                                                            .knownLeader = Consensus::NodeId { "n1" },
                                                            .lastLeaderContact = driverNow - std::chrono::minutes { 1 },
                                                            .matchIndex = {},
                                                            .installRefusal = std::nullopt,
                                                            .appliedIndex = {},
                                                            .lastLogIndex = {} };
    ::core::platform::ManualClock rosterClock;
    Distributed::StateLeaseRoster roster { rosterClock };
    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = FastCache::Testing::TestKeyPair("n1").PublicKey() } };
    roster.Adopt(state);
    rosterClock.advance(std::chrono::hours { 3 });
    REQUIRE(roster.Isolated());

    roster.NoteLeaderReading(LeaderReadingOf(progress, driverNow));
    CHECK_FALSE(roster.Isolated());
    // ...and it is a minute old on the roster's clock: the bound runs out a minute early.
    rosterClock.advance(Distributed::LeaderSilenceBound - std::chrono::minutes { 1 } + std::chrono::seconds { 1 });
    CHECK(roster.Isolated());
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
                                                                              .lastLeaderContact = std::nullopt,
                                                                              .matchIndex = {},
                                                                              .installRefusal = std::nullopt,
                                                                              .appliedIndex = {},
                                                                              .lastLogIndex = {} });

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
    // Held, never released: the tier is handed this socket, so the port is not free for anything
    // else on the host between choosing it and serving it.
    auto held = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(held);
    REQUIRE(held->IsBound());
    auto const port = held->boundPort();

    Testing::ScratchDirectory const scratchDirectory { "consensus-forget-only-voter" };
    scratchDirectory.Write("cluster.key", std::string(32, 'k'));
    auto const& scratch = scratchDirectory.Path();

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
        TestAdvertised(),
        Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        ConsensusTier::LeaderContactObserver {},
        metrics,
        logger,
        nullptr,
        FormationHooks {},
        std::move(held));
    INFO("start refused: " << RefusalOf(started));
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
                                                                   .publicKey = std::nullopt });
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
                                                      .publicKey = std::nullopt })
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
    core::platform::ManualClock rosterClock;
    auto const roster = NodeRoster::Build(Testing::FirstStart(NodeConfig {}), rosterClock, nullptr);
    REQUIRE(roster.has_value());
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };
    RecordingUpstream upstream { directory };
    AppliedSchedulers schedulers { cfg, AsConfigured };
    KnownSchedulingLeader knownLeader;
    SchedulingLeaderPublisher schedulingLeader { knownLeader };

    auto started =
        StartConsensusOrExplain(cfg,
                                noScheduler,
                                TestAdvertised(),
                                Testing::TestKeyPair("n1"),
                                membership,
                                *Unwrap(roster),
                                schedulers,
                                schedulingLeader,
                                SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = &upstream },
                                metrics,
                                logger,
                                nullptr,
                                FormationHooks {});
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
                                                      .publicKey = std::nullopt })
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

TEST_CASE("Every applied state reaches where this node registers, so a voter's recorded endpoint is its next round's",
          "[node][consensus][announce]")
{
    // T26's carry, at the door it is wired through: a node that serves no scheduler registers at the
    // voters its APPLIED state records (`AppliedSchedulers`), and the consensus tier's apply callback is
    // what tells it. A real one-voter tier records itself with the `0xFC` endpoint it advertises, and a
    // learner's source handed to that callback -- a learner whose formation remembered another endpoint
    // -- answers the recorded one from then on. Without the callback the source would answer the
    // remembered endpoint forever, and a voter that moved would be reached only after a reform.
    NullLogger logger;
    AtomicMetricsSink metrics;
    std::unique_ptr<SchedulerTier> const noScheduler;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    Testing::ScratchDirectory const scratch { "consensus-applied-schedulers" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch.Path() / "state";

    NodeMembership membership { cfg, membershipLog };
    core::platform::ManualClock rosterClock;
    auto const roster = NodeRoster::Build(Testing::FirstStart(NodeConfig {}), rosterClock, nullptr);
    REQUIRE(roster.has_value());
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n1", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n1", {} };

    auto learner = Testing::FirstStart(NodeConfig {});
    learner.nodeId = "n2";
    learner = Testing::LearnerRegisteringWith(std::move(learner), { "remembered.example:6674" });
    AppliedSchedulers schedulers { learner, AsConfigured };
    KnownSchedulingLeader knownLeader;
    SchedulingLeaderPublisher schedulingLeader { knownLeader };
    REQUIRE(schedulers.Current() == std::vector<std::string> { "remembered.example:6674" });

    auto started =
        StartConsensusOrExplain(cfg,
                                noScheduler,
                                TestAdvertised(),
                                Testing::TestKeyPair("n1"),
                                membership,
                                *Unwrap(roster),
                                schedulers,
                                schedulingLeader,
                                SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
                                metrics,
                                logger,
                                nullptr,
                                FormationHooks {});
    REQUIRE(started.has_value());
    auto const& tier = *started;
    REQUIRE(tier != nullptr);

    auto const recorded = std::vector<std::string> { std::string { TestAdvertised().Current() } };
    CHECK(Testing::WaitUntil(
        "the voter's recorded 0xFC endpoint to reach where the learner registers",
        [&schedulers, &recorded] { return schedulers.Current() == recorded; },
        [&schedulers, &tier] {
            return std::format(
                "registers at {} (commit {})", schedulers.Current().front(), tier->Status().commitIndex.value);
        }));
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

    Testing::ScratchDirectory const scratch { "consensus-learner-no-endpoint" };

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    auto started = ConsensusTier::Start(
        cfg,
        TestAdvertised(),
        Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        ConsensusTier::LeaderContactObserver {},
        metrics,
        logger,
        nullptr,
        FormationHooks {});
    INFO("start refused: " << RefusalOf(started));
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
                                                      .publicKey = Testing::TestKeyPair("laptop").PublicKey() })
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
                             .publicKey = Testing::TestKeyPair("tablet").PublicKey() });
    Apply(state,
          Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                             .key = "n1",
                             .value = "10.0.0.1:6680",
                             .schedulerEndpoint = {},
                             .publicKey = Testing::TestKeyPair("n1").PublicKey() });
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

    Testing::ScratchDirectory const scratch { "consensus-learner-restart" };

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    auto const start = [&cfg, &logger](IMetricsSink& metrics) {
        return ConsensusTier::Start(
            cfg,
            TestAdvertised(),
            Testing::TestKeyPair("n1"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            ConsensusTier::LeaderContactObserver {},
            metrics,
            logger,
            nullptr,
            FormationHooks {});
    };

    // First life: admit the learner and let the reconciler put it in the configuration.
    {
        AtomicMetricsSink metrics;
        auto started = start(metrics);
        INFO("start refused: " << RefusalOf(started));
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
                                                          .publicKey = Testing::TestKeyPair("laptop").PublicKey() })
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
    INFO("start refused: " << RefusalOf(started));
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

namespace
{
/// A forget revoking @p id's own test key, which no member holds: a roster fact that moves no
/// member and asks consensus to dial nobody.
/// @param id Whose key is revoked.
/// @return The command.
[[nodiscard]] Cluster::Command RevocationCommand(std::string id)
{
    auto const key = Testing::TestKeyPair(id).PublicKey();
    return Cluster::Command {
        .kind = Cluster::CommandKind::Forget, .key = std::move(id), .value = {}, .schedulerEndpoint = {}, .publicKey = key
    };
}

/// Whether @p state records a key revoked under @p id.
/// @param state The state.
/// @param id The id.
/// @return True when a revocation carries it.
[[nodiscard]] bool HasRevocation(Cluster::ClusterState const& state, std::string_view id)
{
    return std::ranges::contains(state.revokedKeys, id, &Cluster::RevokedKey::id);
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
    INFO("store refused: " << RefusalOf(store));
    REQUIRE(store.has_value());
    auto const term = Consensus::Term { .value = 1 };
    auto const saved = store->SaveState(Consensus::PersistentState { .currentTerm = term, .votedFor = std::nullopt });
    INFO("state refused: " << RefusalOf(saved));
    REQUIRE(saved.has_value());

    auto const covered = Cluster::Encode(RevocationCommand("w0"));
    auto entries = std::vector<Consensus::LogEntry> {};
    for ([[maybe_unused]] auto const index: std::views::iota(1, 4))
        entries.push_back(Consensus::LogEntry { .term = term, .kind = Consensus::EntryKind::Command, .payload = covered });
    entries.push_back(
        Consensus::LogEntry { .term = term, .kind = Consensus::EntryKind::Command, .payload = std::move(retained) });
    // Each written outside the assertion: a `REQUIRE` expands its expression more than
    // once, so a move inside one reads to the analyser as a use after the move.
    auto const logged = store->SaveLog(
        Consensus::LogAppend { .fromIndex = Consensus::LogIndex { .value = 1 }, .entries = std::move(entries) });
    INFO("log refused: " << RefusalOf(logged));
    REQUIRE(logged.has_value());

    auto const snapshotted = store->SaveSnapshot(
        Consensus::RaftSnapshot { .lastIncludedIndex = Consensus::LogIndex { .value = 3 },
                                  .lastIncludedTerm = term,
                                  .configuration = Consensus::Configuration { .voters = { "n1" }, .learners = {} },
                                  .state = std::move(snapshotState) });
    INFO("snapshot refused: " << RefusalOf(snapshotted));
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

    // Held, never released: the tier is handed this socket, so the port is not free for anything
    // else on the host between choosing it and serving it.
    auto held = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(held);
    REQUIRE(held->IsBound());
    auto const port = held->boundPort();

    // Cleared first and removed after, which a bare `UniqueScratchPath` is not: its name is the
    // pid and a counter, Windows reuses pids freely, and a directory an earlier run left behind
    // under the same name holds a log `PlantConsensusState` cannot write over -- every section
    // then failed at its `SaveLog`, in two runs of four on a host holding 2,933 such directories.
    Testing::ScratchDirectory const scratchDirectory { "consensus-unreadable-state" };
    scratchDirectory.Write("cluster.key", std::string(32, 'k'));
    auto const& scratch = scratchDirectory.Path();

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";
    auto const directory = NodeStateDirectory(cfg);

    // A revocation in the snapshot and another above it: what a node that ran on an unread
    // snapshot loses, and what one that skipped an unread command loses.
    auto current = Cluster::ClusterState {};
    Cluster::Apply(current, RevocationCommand("w1"));
    auto const currentSnapshot = Cluster::Encode(current);
    auto const currentCommand = Cluster::Encode(RevocationCommand("w2"));

    // Anything published at all is something applied: the observer is how the member set
    // reaches the fleet's oracle.
    auto published = std::make_shared<std::atomic<int>>(0);
    auto const start = [&] {
        return ConsensusTier::Start(
            cfg,
            TestAdvertised(),
            Testing::TestKeyPair("n1"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [published](Cluster::ClusterState const&) { published->fetch_add(1); },
            ConsensusTier::LeaderContactObserver {},
            metrics,
            logger,
            nullptr,
            FormationHooks {},
            std::move(held));
    };

    SECTION("control: state this build wrote starts, with the snapshot restored and the entry above it applied")
    {
        PlantConsensusState(directory, currentSnapshot, currentCommand);
        auto started = start();
        INFO("start refused: " << RefusalOf(started));
        REQUIRE(started.has_value());
        auto const& tier = *started;

        // Restored when the driver was built, before either loop ran.
        CHECK(HasRevocation(tier->ClusterState(), "w1"));
        CHECK(published->load() > 0);

        // And the entry above it, once the node leads again and commits it.
        CHECK(Testing::WaitUntil(
            "the retained admission to commit",
            [&tier] { return HasRevocation(tier->ClusterState(), "w2"); },
            [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));
    }

    SECTION("a snapshot the build before wrote refuses the start, naming the directory, both versions and the remedy")
    {
        PlantConsensusState(directory, Testing::EncodePreviousClusterState(), currentCommand);
        auto const started = start();
        REQUIRE_FALSE(started.has_value());
        // A state this build cannot read is still unreadable to it at the next start.
        CHECK(started.error().cause == NodeRefusalCause::ConsensusState);
        auto const& refusal = started.error().reason;
        CAPTURE(refusal);
        CHECK(refusal.contains(directory.string()));
        CHECK(refusal.contains("the snapshot as of log entry 3"));
        CHECK(refusal.contains(std::format("cluster state encoding version {}", Testing::PreviousClusterStateVersion)));
        CHECK(refusal.contains("reads 10"));
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
        CHECK(started.error().cause == NodeRefusalCause::ConsensusState);
        auto const& refusal = started.error().reason;
        CAPTURE(refusal);
        CHECK(refusal.contains(directory.string()));
        CHECK(refusal.contains("log entry 4"));
        CHECK(refusal.contains(std::format("cluster command encoding version {}", Testing::PreviousClusterCommandVersion)));
        CHECK(refusal.contains("reads 6"));
        CHECK(refusal.contains(Consensus::UnreadableStateRemedy));
        CHECK(published->load() == 0);
    }

    SECTION("a retained command carrying a retired verb refuses the start, naming the retirement")
    {
        // Byte 4 was a client verb an earlier build committed, and byte 6 the admission of a
        // principal by its key: this build's command version and layout, and a verb it retired.
        // Applied as nothing it would leave this node holding a state its log says otherwise
        // about -- a principal it would never record -- so it refuses the start by name.
        auto const [kind, key, publicKey] =
            GENERATE(table<Cluster::CommandKind, std::string, std::optional<Ed25519PublicKey>>({
                { Cluster::CommandKind::RetiredForgetClient, "10.0.0.7", std::nullopt },
                { Cluster::CommandKind::RetiredAdmitPrincipal, "w9", Testing::TestKeyPair("w9").PublicKey() },
            }));
        INFO("verb " << static_cast<unsigned>(kind));
        auto const retired = Cluster::Encode(
            Cluster::Command { .kind = kind, .key = key, .value = {}, .schedulerEndpoint = {}, .publicKey = publicKey });
        PlantConsensusState(directory, currentSnapshot, retired);
        auto const started = start();
        REQUIRE_FALSE(started.has_value());
        auto const& refusal = started.error();
        CAPTURE(refusal.reason);
        CHECK(refusal.reason.contains(directory.string()));
        CHECK(refusal.reason.contains("log entry 4"));
        CHECK(refusal.reason.contains(std::format("verb {} is retired", static_cast<unsigned>(kind))));
        CHECK(refusal.reason.contains(Consensus::UnreadableStateRemedy));
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

    // The tier's own port is held and handed over, so it is never free between choosing it and
    // serving it.
    auto held = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(held);
    REQUIRE(held->IsBound());
    auto const self = held->boundPort();
    // Nobody answers here. The tier dials its leader and fails, which is ordinary for a
    // follower that has not reached its leader yet; the leader reaches IT, below.
    auto const leaderPort = FreeLoopbackPort();

    Testing::ScratchDirectory const scratchDirectory { "consensus-unreadable-install" };
    scratchDirectory.Write("cluster.key", std::string(32, 'k'));
    auto const& scratch = scratchDirectory.Path();

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
        TestAdvertised(),
        Testing::TestKeyPair("n2"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        ConsensusTier::LeaderContactObserver {},
        metrics,
        logger,
        &conditions,
        FormationHooks {},
        std::move(held));
    INFO("start refused: " << RefusalOf(started));
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
        Cluster::Apply(current, RevocationCommand("w7"));
        auto const connection = OfferAsLeader(self, offer(Cluster::Encode(current)));

        REQUIRE(Testing::WaitUntil(
            "the offered snapshot to be taken on", [&tier] { return HasRevocation(tier->ClusterState(), "w7"); }, commit));
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
        CHECK(row->detail.contains("reads 10"));

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
            TestAdvertised(),
            Testing::TestKeyPair("n2"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            ConsensusTier::LeaderContactObserver {},
            metrics,
            logger,
            nullptr,
            FormationHooks {});
    };

    Testing::ScratchDirectory const founderDirectory { "consensus-bootstrap-founder" };
    Testing::ScratchDirectory const joinerDirectory { "consensus-bootstrap-joiner" };

    auto founder = Testing::FirstStart(NodeConfig {});
    founder.nodeId = "n2";
    founder.raftListen = std::format("127.0.0.1:{}", FreeLoopbackPort());
    founder.raftSelf = "127.0.0.1";
    founder.clusterDir = founderDirectory / "state";
    REQUIRE(founder.formation.has_value());

    auto joiner = founder;
    joiner.raftListen = std::format("127.0.0.1:{}", FreeLoopbackPort());
    joiner.clusterDir = joinerDirectory / "state";
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
        INFO("start refused: " << RefusalOf(started));
        REQUIRE(started.has_value());
        auto const status = (*started)->Status();
        CHECK(status.configuration.voters == std::vector<Consensus::NodeId> { "n2" });
        CHECK(status.configuration.learners.empty());
    }

    SECTION("the joiner bootstraps nothing, and waits to be admitted")
    {
        auto started = start(joiner);
        INFO("start refused: " << RefusalOf(started));
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
            TestAdvertised(),
            Testing::TestKeyPair("n2"),
            [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
            [](Cluster::ClusterState const&) {},
            ConsensusTier::LeaderContactObserver {},
            metrics,
            logger,
            nullptr,
            FormationHooks {});
    };

    SECTION("a learner's own record carries a learner's seat")
    {
        // Asked of `ConsensusSelfMemberOf`, the record `Start` announces verbatim, rather than of a
        // started tier: a learner binds no consensus port, so the record it would announce is the
        // observable. The voter section below is the real tier; the two together pin that the seat
        // follows the mode.
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
        INFO("start: " << (started.has_value() ? std::string {} : started.error().reason));
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

namespace
{
/// A port on the loopback nobody listens on now, for an address a case dials and nothing answers.
/// @return The port.
[[nodiscard]] std::uint16_t UnansweredPort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();
    return port;
}

/// A member of a fleet, as the roster an approval carried records it.
/// @param id Its id; its key is `TestKeyPair(id)`'s.
/// @param raftEndpoint Where it says it answers consensus; empty for one that dials in.
/// @param seat Where it sits.
/// @return The member.
[[nodiscard]] Cluster::ClusterMember FleetMember(std::string const& id, std::string raftEndpoint, Cluster::MemberSeat seat)
{
    return Cluster::ClusterMember { .id = id,
                                    .raftEndpoint = std::move(raftEndpoint),
                                    .schedulerEndpoint = {},
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = seat,
                                    .publicKey = Testing::TestKeyPair(id).PublicKey() };
}

/// A configuration in @p mode of a fleet this node joined, @p members its roster.
/// @param self This node's id.
/// @param mode A mode of a fleet: learner, or voter.
/// @param directory Its state directory.
/// @param members The fleet's roster, never this node.
/// @return The configuration, its formation applied.
[[nodiscard]] NodeConfig JoinedConfig(std::string const& self,
                                      Cluster::NodeMode mode,
                                      std::filesystem::path directory,
                                      std::vector<Cluster::ClusterMember> members)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = self;
    cfg.raftListen = std::format("127.0.0.1:{}", UnansweredPort());
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = std::move(directory);
    cfg.formation = NodeFormationView { .mode = mode,
                                        .clusterId = cfg.clusterId,
                                        .createdAtUnixSeconds = 0,
                                        .foundedHere = false,
                                        .fleetMembers = std::move(members),
                                        .fleetSchedulers = {} };
    return cfg;
}

/// How long a tier's teardown may take before a case calls it a hang: the reactor stops once the last
/// of its loops finishes, and a count that never reaches zero leaves it running for good.
constexpr std::chrono::seconds TierStopBound { 15 };

/// Destroy @p tier on a thread of its own and wait at most `TierStopBound` for it.
///
/// The thread owns the tier and @p keepAlive -- everything the tier still touches -- so a teardown
/// that never ends leaves nothing behind that this case destroys.
/// @param tier The tier; may be null.
/// @param keepAlive What the tier borrows, kept alive until it is gone.
/// @return Whether it stopped within the bound.
[[nodiscard]] bool StopTierWithin(std::unique_ptr<ConsensusTier> tier, std::shared_ptr<void const> keepAlive)
{
    if (tier == nullptr)
        return true;
    auto stopped = std::make_shared<std::promise<void>>();
    auto const done = stopped->get_future();
    std::thread { [tier = std::move(tier), keepAlive = std::move(keepAlive), stopped]() mutable {
        tier.reset();
        keepAlive.reset();
        stopped->set_value();
    } }.detach();
    return done.wait_for(TierStopBound) == std::future_status::ready;
}

/// How many of a stuck tier's last log records `StopTierOrExit` prints: enough for the stop's own
/// phase lines and both drains' ceilings, few enough to read.
constexpr std::size_t StuckTierRecordsShown = 40;

/// `StopTierWithin` for a fixture's teardown, which has nothing to keep alive past itself and no case
/// to fail: a tier that does not stop ENDS THE PROCESS saying which, rather than hanging it -- a
/// suite timeout names nothing, and nothing after a stuck teardown could run anyway.
///
/// And it prints what the tier last SAID, because the one time this fired (cl-debug, the
/// restarted-follower case) it said nothing else: the stop's phase lines and the drains' ceilings
/// are where the tier names which part did not end, and a `NullLogger` threw them away.
/// @param tier The tier; may be null.
/// @param who Whose it is, for the line that names it.
/// @param said The tier's own logger, or null when nothing was kept.
void StopTierOrExit(std::unique_ptr<ConsensusTier> tier, std::string_view who, CapturingLogger const* said)
{
    if (StopTierWithin(std::move(tier), nullptr))
        return;
    auto text = std::format(
        "the consensus tier of {} did not stop within {}; ending the process rather than hanging it\n", who, TierStopBound);
    if (said != nullptr)
    {
        auto const records = said->Snapshot();
        auto const shown = std::min(records.size(), StuckTierRecordsShown);
        text += std::format("its last {} of {} log record(s):\n", shown, records.size());
        for (auto const& record: std::span { records }.last(shown))
            text += std::format("  [{}] {}\n", static_cast<int>(record.level), record.message);
    }
    std::fputs(text.c_str(), stderr);
    std::_Exit(EXIT_FAILURE);
}

/// Start a tier for @p cfg under `TestKeyPair(cfg.nodeId)`, with no observers but @p hooks.
/// @param cfg The configuration.
/// @param metrics Where it counts.
/// @param logger Where it says what it does.
/// @param hooks What its formation is told.
/// @return The tier, or why it refused.
[[nodiscard]] std::expected<std::unique_ptr<ConsensusTier>, NodeRefusal> StartTier(NodeConfig const& cfg,
                                                                                   AtomicMetricsSink& metrics,
                                                                                   ILogger& logger,
                                                                                   FormationHooks hooks = {})
{
    return ConsensusTier::Start(
        cfg,
        TestAdvertised(),
        Testing::TestKeyPair(cfg.nodeId),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        ConsensusTier::LeaderContactObserver {},
        metrics,
        logger,
        nullptr,
        std::move(hooks));
}
} // namespace

TEST_CASE("A learner tier starts with no raft port and dials every voter of its fleet two-way",
          "[node][consensus][formation][learner]")
{
    // The go-live blocker at the door it was found at: a learner RUNS consensus, and its Raft
    // surface resolves no address by design, so a tier that asked for one refused every learner's
    // start. And who it dials is the seat's link column: the fleet's voter, never the other learner
    // -- whose recorded endpoint is stale here on purpose, so only the column can exclude it.
    //
    // Shared with the thread that stops the tier below, so a teardown that never ends -- the defect
    // that case exists to catch -- touches nothing this case has already destroyed.
    auto const logger = std::make_shared<NullLogger>();
    auto const metrics = std::make_shared<AtomicMetricsSink>();
    auto const scratch = std::make_shared<Testing::ScratchDirectory const>("consensus-learner-tier");
    auto const cfg =
        JoinedConfig("n-laptop",
                     Cluster::NodeMode::Learner,
                     *scratch / "state",
                     { FleetMember("office", std::format("127.0.0.1:{}", UnansweredPort()), Cluster::MemberSeat::Voter),
                       FleetMember("n-desk", std::format("127.0.0.1:{}", UnansweredPort()), Cluster::MemberSeat::Learner) });
    REQUIRE(RunsConsensus(cfg));
    REQUIRE(RowFor(NodeSurface::Raft).Resolve(cfg).empty()); // the premise: nothing to bind

    auto started = StartTier(cfg, *metrics, *logger);
    INFO("start refused: " << RefusalOf(started));
    REQUIRE(started.has_value());
    auto tier = *std::move(started);

    CHECK(tier->BoundEndpoint().empty());
    CHECK(tier->Direction() == Consensus::RaftWire::SessionDirection::TwoWay);
    CHECK(tier->DialTargets() == std::vector<Consensus::NodeId> { "office" });

    // What it announces about itself through the real tier (the step-6 M1 carry): a learner's seat,
    // and no endpoint, since nobody dials it.
    CHECK(tier->Self().seat == Cluster::MemberSeat::Learner);
    CHECK(tier->Self().raftEndpoint.empty());

    // And it STOPS, within a bound: a learner runs ONE reactor loop, and the last loop to finish is
    // what stops the reactor -- so a count that said two would leave it running and the teardown
    // would never end. Stopped on a thread of its own and waited for, so that defect is a red here
    // rather than a hang; the thread owns everything it touches.
    CHECK(StopTierWithin(
        std::move(tier),
        std::make_shared<std::tuple<decltype(logger), decltype(metrics), decltype(scratch)>>(logger, metrics, scratch)));
}

TEST_CASE("A voter whose fleet records a learner starts and dials only the voters", "[node][consensus][formation]")
{
    // The second go-live blocker: the roster names a learner with NO endpoint, which is the ordinary
    // case, and the tier dialled every member -- so the empty one refused the voter's start. A stale
    // endpoint for the other learner keeps the case from passing on emptiness alone.
    NullLogger logger;
    AtomicMetricsSink metrics;
    Testing::ScratchDirectory const scratch { "consensus-voter-with-learner" };
    auto const cfg =
        JoinedConfig("n2",
                     Cluster::NodeMode::Voter,
                     scratch / "state",
                     { FleetMember("n1", std::format("127.0.0.1:{}", UnansweredPort()), Cluster::MemberSeat::Voter),
                       FleetMember("n-laptop", {}, Cluster::MemberSeat::Learner),
                       FleetMember("n-desk", std::format("127.0.0.1:{}", UnansweredPort()), Cluster::MemberSeat::Learner) });

    auto started = StartTier(cfg, metrics, logger);
    INFO("start refused: " << RefusalOf(started));
    REQUIRE(started.has_value());
    auto const& tier = *started;

    CHECK_FALSE(tier->BoundEndpoint().empty());
    CHECK(tier->Direction() == Consensus::RaftWire::SessionDirection::OneWay);
    CHECK(tier->DialTargets() == std::vector<Consensus::NodeId> { "n1" });
    CHECK(tier->Self().seat == Cluster::MemberSeat::Voter);
}

TEST_CASE("A leader dials the voters its state records and never a learner", "[node][consensus][formation]")
{
    // The node honouring "a leader never dials a learner": the reconciler learns every member the
    // applied state records, and the seat's link column decides who of them is dialled. The voter
    // `n3` is the positive control -- recorded in the same pass, it IS dialled, so an absent
    // learner is a decision rather than a reconciler that has not run.
    NullLogger logger;
    AtomicMetricsSink metrics;
    Testing::ScratchDirectory const scratch { "consensus-leader-dials" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", UnansweredPort());
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";

    auto started = StartTier(cfg, metrics, logger);
    INFO("start refused: " << RefusalOf(started));
    REQUIRE(started.has_value());
    auto const& tier = *started;
    CHECK_FALSE(tier->BoundEndpoint().empty());
    CHECK(tier->Direction() == Consensus::RaftWire::SessionDirection::OneWay);
    REQUIRE(Testing::WaitUntil(
        "the one-voter tier to lead",
        [&tier] { return tier->Status().role == Consensus::Role::Leader; },
        [&tier] { return std::format("commit index {}", tier->Status().commitIndex.value); }));

    // A learner recorded WITH an endpoint, which only the column can then keep undialled.
    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                                      .key = "n-laptop",
                                                      .value = std::format("127.0.0.1:{}", UnansweredPort()),
                                                      .schedulerEndpoint = {},
                                                      .publicKey = Testing::TestKeyPair("n-laptop").PublicKey() })
                .has_value());
    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                                      .key = "n3",
                                                      .value = std::format("127.0.0.1:{}", UnansweredPort()),
                                                      .schedulerEndpoint = {},
                                                      .publicKey = Testing::TestKeyPair("n3").PublicKey() })
                .has_value());

    REQUIRE(Testing::WaitUntil(
        "the reconciler to dial the voter the state records",
        [&tier] { return std::ranges::contains(tier->DialTargets(), Consensus::NodeId { "n3" }); },
        [&tier] { return std::format("{} member(s) recorded", tier->ClusterState().members.size()); }));
    REQUIRE(std::ranges::contains(tier->ClusterState().members, std::string { "n-laptop" }, &Cluster::ClusterMember::id));
    CHECK_FALSE(std::ranges::contains(tier->DialTargets(), Consensus::NodeId { "n-laptop" }));
}

TEST_CASE("A tier tells its formation every applied state with its cluster and who leads", "[node][consensus][formation]")
{
    // The hook `main` hands the formation controller: the APPLIED state, the cluster it belongs to
    // -- which a state does not name, and a controller must not mistake a left cluster's for its
    // own -- and the leader with where it answers, once one is known.
    NullLogger logger;
    AtomicMetricsSink metrics;
    Testing::ScratchDirectory const scratch { "consensus-formation-hooks" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", UnansweredPort());
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";
    cfg.clusterId = "c-office";

    struct Told
    {
        std::mutex lock;
        std::vector<std::string> clusters;
        std::optional<Consensus::NodeId> leader;
        std::string leaderNodeEndpoint;
    };
    Told told;
    auto hooks = FormationHooks { .onState =
                                      [&told](Cluster::ClusterState const&,
                                              std::string_view clusterId,
                                              std::optional<Consensus::NodeId> const& leader,
                                              std::string_view leaderNodeEndpoint) {
                                          std::scoped_lock const lock { told.lock };
                                          told.clusters.emplace_back(clusterId);
                                          told.leader = leader;
                                          told.leaderNodeEndpoint = leaderNodeEndpoint;
                                      },
                                  .onOwnKeyRevoked = {} };

    auto started = StartTier(cfg, metrics, logger, std::move(hooks));
    INFO("start refused: " << RefusalOf(started));
    REQUIRE(started.has_value());
    auto const& tier = *started;

    auto const toldLeader = [&told] {
        std::scoped_lock const lock { told.lock };
        return told.leader;
    };
    REQUIRE(Testing::WaitUntil(
        "the formation to be told this node leads",
        [&toldLeader] { return toldLeader() == std::optional<Consensus::NodeId> { "n1" }; },
        [&tier] { return std::format("role {}", static_cast<int>(tier->Status().role)); }));
    {
        std::scoped_lock const lock { told.lock };
        REQUIRE_FALSE(told.clusters.empty());
        CHECK(std::ranges::all_of(told.clusters, [](std::string const& cluster) { return cluster == "c-office"; }));
        CHECK(told.leaderNodeEndpoint.empty()); // the state records no 0xFC endpoint for n1 yet
    }

    // Where the leader answers is the STATE's to say -- the one argument a controller follows a leader
    // by -- so once the cluster records n1's endpoint, the formation is told it. The one its own
    // reconcile asserts, so the two never contend for the record.
    auto const leaderNodeEndpoint = std::string { TestAdvertised().Current() };
    REQUIRE(tier->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                                      .key = "n1",
                                                      .value = cfg.raftListen,
                                                      .schedulerEndpoint = leaderNodeEndpoint,
                                                      // A member record carries its key: the state refuses one without.
                                                      .publicKey = Testing::TestKeyPair("n1").PublicKey() })
                .has_value());
    auto const toldEndpoint = [&told] {
        std::scoped_lock const lock { told.lock };
        return told.leaderNodeEndpoint;
    };
    REQUIRE(Testing::WaitUntil(
        "the formation to be told where the leader answers",
        [&toldEndpoint, &leaderNodeEndpoint] { return toldEndpoint() == leaderNodeEndpoint; },
        [&toldEndpoint] { return std::format("told `{}`", toldEndpoint()); }));
    CHECK(toldLeader() == std::optional<Consensus::NodeId> { "n1" });
}

namespace
{
/// A fleet of two REAL tiers on the loopback: `n1` founded it, and `n2` joined it in a seat. The
/// founder's own state records the joiner under its key -- the way an approval replicates it -- and
/// the joiner starts from the roster that approval handed it, exactly as a reformed body does.
///
/// Reusable on purpose: a fleet property that needs a founder and a joined member over real sessions
/// -- what a keyed desire reaches, what a forget closes -- is built on this rather than on a third
/// copy of the start-up below. Members are declared so they unwind in order: the joiner's tier first,
/// then the founder's, then the directories they ran over.
struct TwoTierFleet
{
    TwoTierFleet() = default;
    TwoTierFleet(TwoTierFleet const&) = delete;
    TwoTierFleet(TwoTierFleet&&) = delete;
    TwoTierFleet& operator=(TwoTierFleet const&) = delete;
    TwoTierFleet& operator=(TwoTierFleet&&) = delete;

    /// Stops the joiner's tier, then the founder's, each within `TierStopBound` (`StopTierOrExit`).
    ~TwoTierFleet()
    {
        StopTierOrExit(std::move(joinerTier), "n2", &joinerLogger);
        StopTierOrExit(std::move(founderTier), "n1", &founderLogger);
    }

    Testing::ScratchDirectory founderDirectory { "consensus-two-tier-founder" }; ///< `n1`'s state directory lives here.
    Testing::ScratchDirectory joinerDirectory { "consensus-two-tier-joiner" };   ///< `n2`'s.
    CapturingLogger founderLogger { LogLevel::Debug }; ///< What `n1` says, printed should it not stop.
    CapturingLogger joinerLogger { LogLevel::Debug };  ///< What `n2` says, the same way.
    AtomicMetricsSink founderMetrics;                  ///< What `n1` counts.
    AtomicMetricsSink joinerMetrics;                   ///< What `n2` counts.
    NodeConfig founder;                                ///< `n1`'s configuration.
    NodeConfig joiner;                                 ///< `n2`'s, its formation applied.
    std::unique_ptr<ConsensusTier> founderTier;        ///< `n1`, leading.
    std::unique_ptr<ConsensusTier> joinerTier;         ///< `n2`, joined.
};

/// Start `n1` as a founder, and return once it leads its cluster of one: the half of a two-tier
/// fleet both ways in share -- an admission proposed directly (`StartTwoTierFleet`) and one that
/// travels the enrollment route.
/// @param fleet Where the founder's tier and directory live.
void StartTwoTierFounder(TwoTierFleet& fleet)
{
    fleet.founder = Testing::FirstStart(NodeConfig {});
    fleet.founder.nodeId = "n1";
    fleet.founder.raftListen = std::format("127.0.0.1:{}", UnansweredPort());
    fleet.founder.raftSelf = "127.0.0.1";
    fleet.founder.clusterDir = fleet.founderDirectory / "state";
    auto founded = StartTier(fleet.founder, fleet.founderMetrics, fleet.founderLogger);
    INFO("the founder's start refused: " << RefusalOf(founded));
    REQUIRE(founded.has_value());
    fleet.founderTier = std::move(*founded);
    auto const& founderTier = fleet.founderTier;
    REQUIRE(Testing::WaitUntil(
        "the founder to lead its cluster of one",
        [&founderTier] { return founderTier->Status().role == Consensus::Role::Leader; },
        [&founderTier] { return std::format("role {}", static_cast<int>(founderTier->Status().role)); }));
}

/// Start a founder, have it record `n2` in @p seat under `n2`'s key, and write `n2`'s configuration
/// as a member of that fleet in that seat -- everything but starting `n2`, which a case may start
/// through `StartTier` or through production's own `StartConsensusOrExplain`.
/// @param fleet Where the two tiers and their directories live.
/// @param seat Where the joiner sits.
void AdmitTwoTierJoiner(TwoTierFleet& fleet, Cluster::MemberSeat seat)
{
    StartTwoTierFounder(fleet);
    auto const joinerRaft =
        seat == Cluster::MemberSeat::Voter ? std::format("127.0.0.1:{}", UnansweredPort()) : std::string {};
    auto const admitted = fleet.founderTier->ProposeToCluster(Cluster::Command {
        .kind = seat == Cluster::MemberSeat::Voter ? Cluster::CommandKind::AddMember : Cluster::CommandKind::AddLearner,
        .key = "n2",
        .value = joinerRaft,
        .schedulerEndpoint = {},
        .publicKey = Testing::TestKeyPair("n2").PublicKey(),
        .createdAtUnixSeconds = std::nullopt });
    INFO("the admission was refused: " << (admitted.has_value() ? std::string {} : admitted.error().context));
    REQUIRE(admitted.has_value());

    fleet.joiner = JoinedConfig("n2",
                                seat == Cluster::MemberSeat::Voter ? Cluster::NodeMode::Voter : Cluster::NodeMode::Learner,
                                fleet.joinerDirectory / "state",
                                { FleetMember("n1", fleet.founder.raftListen, Cluster::MemberSeat::Voter) });
    if (!joinerRaft.empty())
        fleet.joiner.raftListen = joinerRaft;
    fleet.joiner.clusterId = fleet.founder.clusterId;
    REQUIRE(fleet.joiner.formation.has_value());
    auto formation = Testing::Unwrap(fleet.joiner.formation);
    formation.clusterId = fleet.founder.clusterId;
    fleet.joiner.formation = std::move(formation);
}

/// Start a founder, have it record `n2` in @p seat under `n2`'s key, and start `n2` as a member of
/// that fleet in that seat. Returns once both tiers run; what they converge to is the caller's to
/// wait for.
/// @param fleet Where the two tiers and their directories live.
/// @param seat Where the joiner sits.
/// @param joinerHooks What the joiner's formation is told.
void StartTwoTierFleet(TwoTierFleet& fleet, Cluster::MemberSeat seat, FormationHooks joinerHooks)
{
    AdmitTwoTierJoiner(fleet, seat);
    auto joined = StartTier(fleet.joiner, fleet.joinerMetrics, fleet.joinerLogger, std::move(joinerHooks));
    INFO("the joiner's start refused: " << RefusalOf(joined));
    REQUIRE(joined.has_value());
    fleet.joinerTier = std::move(*joined);
}

/// Whether @p state records `n2` in @p seat under `n2`'s key.
/// @param state An applied state.
/// @param seat The seat.
/// @return True when it does.
[[nodiscard]] bool RecordsJoiner(Cluster::ClusterState const& state, Cluster::MemberSeat seat)
{
    return std::ranges::any_of(state.members, [seat](Cluster::ClusterMember const& member) {
        return member.id == "n2" && member.seat == seat
               && member.publicKey == std::optional { Testing::TestKeyPair("n2").PublicKey() };
    });
}
} // namespace

TEST_CASE("A joined voter is counted by its founder's quorum and follows its leader over real tiers",
          "[node][consensus][formation][fleet]")
{
    TwoTierFleet fleet;
    StartTwoTierFleet(fleet, Cluster::MemberSeat::Voter, FormationHooks {});
    auto const& founder = fleet.founderTier;
    auto const& joiner = fleet.joinerTier;

    REQUIRE(Testing::WaitUntil(
        "the founder to count the joined voter",
        [&founder] { return std::ranges::contains(founder->Status().configuration.voters, Consensus::NodeId { "n2" }); },
        [&founder] { return std::format("{} voter(s)", founder->Status().configuration.voters.size()); }));
    REQUIRE(Testing::WaitUntil(
        "the joined voter to follow the founder and hold the state that records it",
        [&joiner] {
            return joiner->Status().knownLeader == std::optional<Consensus::NodeId> { "n1" }
                   && RecordsJoiner(joiner->ClusterState(), Cluster::MemberSeat::Voter);
        },
        [&joiner] { return std::format("commit {}", joiner->Status().commitIndex.value); }));
    CHECK(joiner->Direction() == Consensus::RaftWire::SessionDirection::OneWay);
    CHECK_FALSE(joiner->BoundEndpoint().empty());
}

TEST_CASE("A node's applied state is behind while short of the log it recovered, and caught up through the shorter",
          "[node][consensus][boot-order]")
{
    // Batch 3's M3, as the rule a proof surface asks (`AppliedStateOf`): what decides whether a key
    // absent from the roster is absent from the cluster or only not applied yet.
    auto const at = [](std::uint64_t value) {
        return Consensus::LogIndex { .value = value };
    };
    // Just restarted -- a lone voter before its election commits, a follower before its leader's
    // first word: nothing applied, a log recovered.
    CHECK(AppliedStateOf(at(0), at(12), at(12)) == AppliedStateReading::Behind);
    // The leader's no-op committed, and everything before it applied with it.
    CHECK(AppliedStateOf(at(13), at(13), at(12)) == AppliedStateReading::CaughtUp);
    // A follower whose recovered tail no leader committed: truncated to 8, applied through 8. Measured
    // against the recovered 10 alone it would answer "not yet" until the log grew past it -- forever,
    // in a quiet fleet.
    CHECK(AppliedStateOf(at(8), at(8), at(10)) == AppliedStateReading::CaughtUp);
    CHECK(AppliedStateOf(at(7), at(8), at(10)) == AppliedStateReading::Behind);
    // A first start recovered nothing, so there is nothing to catch up with.
    CHECK(AppliedStateOf(at(0), at(0), at(0)) == AppliedStateReading::CaughtUp);

    // And the slot a proof surface is built over answers `Unknown` until a tier is attached -- which
    // the surface treats as not caught up, since nothing has been applied at all.
    ConsensusStandingSlot const slot;
    CHECK(slot.CurrentAppliedState() == AppliedStateReading::Unknown);
}

TEST_CASE("A restarted follower is behind until its leader speaks, then caught up, over real tiers",
          "[node][consensus][formation][fleet][boot-order]")
{
    // The follower half of M3, MEASURED on a scratch fleet before the fix: a restarted follower
    // answered its fleet's proofs `node-key-unknown` for the 234 ms before its leader's first
    // AppendEntries. Over real tiers: a two-voter fleet forms, both stop -- the founder FIRST, so the
    // follower's port is left with no connection it closed and it rebinds at once -- and the follower
    // restarts alone. With half the voters it can elect nobody and hears nobody, so it STAYS behind,
    // which is the window, held open. Then the founder returns, a leader commits, and it catches up.
    TwoTierFleet fleet;
    StartTwoTierFleet(fleet, Cluster::MemberSeat::Voter, FormationHooks {});
    REQUIRE(Testing::WaitUntil(
        "the joined voter to follow the founder and hold the state that records it",
        [&fleet] {
            return fleet.joinerTier->Status().knownLeader == std::optional<Consensus::NodeId> { "n1" }
                   && RecordsJoiner(fleet.joinerTier->ClusterState(), Cluster::MemberSeat::Voter);
        },
        [&fleet] { return std::format("commit {}", fleet.joinerTier->Status().commitIndex.value); }));
    CHECK(fleet.joinerTier->CurrentAppliedState() == AppliedStateReading::CaughtUp);

    REQUIRE(StopTierWithin(std::move(fleet.founderTier), nullptr));
    REQUIRE(StopTierWithin(std::move(fleet.joinerTier), nullptr));

    auto restarted = StartTier(fleet.joiner, fleet.joinerMetrics, fleet.joinerLogger);
    INFO("the follower's restart refused: " << RefusalOf(restarted));
    REQUIRE(restarted.has_value());
    fleet.joinerTier = std::move(*restarted);
    auto const& joiner = fleet.joinerTier;
    CHECK(joiner->CurrentAppliedState() == AppliedStateReading::Behind);
    // Still behind two reconcile passes later: no quorum, so no leader and nothing applied. A fixed
    // wait, because what is asserted is that NOTHING moves; the leader check beside it is the reason.
    std::this_thread::sleep_for(2 * ConsensusTier::ReconcileInterval);
    CHECK(joiner->CurrentAppliedState() == AppliedStateReading::Behind);
    CHECK_FALSE(joiner->Status().knownLeader.has_value());

    auto founder = StartTier(fleet.founder, fleet.founderMetrics, fleet.founderLogger);
    INFO("the founder's restart refused: " << RefusalOf(founder));
    REQUIRE(founder.has_value());
    fleet.founderTier = std::move(*founder);
    CHECK(Testing::WaitUntil(
        "the restarted follower to catch up once a leader commits",
        [&joiner] { return joiner->CurrentAppliedState() == AppliedStateReading::CaughtUp; },
        [&joiner] {
            return std::format("known leader {}, commit {}",
                               joiner->Status().knownLeader.value_or("(none)"),
                               joiner->Status().commitIndex.value);
        }));
}

TEST_CASE("A two-voter fleet's follower holds its leader as a voter, under its key, in the state it applied",
          "[node][consensus][formation][fleet]")
{
    // H7, checked on the merged tree (batch 3, B3-R4): measured on master-era --raft-peer formation, a
    // two-voter fleet's FOLLOWER never registered -- its roster answered `NotVoter` for the leader,
    // because consensus counted two voters while the replicated state recorded one. Formation by record
    // records every voter WITH its key (the founder at its first pass, a joiner at its admission), so
    // the follower's own roster -- built the way `main` builds it, fed the state the follower applied --
    // holds the leader as a voter. Kept as the production-seam control the ruling asks for.
    TwoTierFleet fleet;
    StartTwoTierFleet(fleet, Cluster::MemberSeat::Voter, FormationHooks {});
    auto const recordsVoter = [](Cluster::ClusterState const& state, std::string const& id) {
        return std::ranges::any_of(state.members, [&id](Cluster::ClusterMember const& member) {
            return member.id == id && member.seat == Cluster::MemberSeat::Voter
                   && member.publicKey == std::optional { Testing::TestKeyPair(id).PublicKey() };
        });
    };
    auto const& joiner = fleet.joinerTier;
    REQUIRE(Testing::WaitUntil(
        "the follower to apply a state recording both voters under their keys",
        [&joiner, &recordsVoter] {
            auto const state = joiner->ClusterState();
            // And the configuration counting both, which the leader proposes once the joiner is
            // dialable and caught up (#1537) -- a moment after the record, so it is waited for too.
            return joiner->Status().knownLeader == std::optional<Consensus::NodeId> { "n1" } && recordsVoter(state, "n1")
                   && recordsVoter(state, "n2") && joiner->Status().configuration.voters.size() == 2;
        },
        [&joiner] {
            return std::format("{} member(s) recorded, {} voter(s) counted",
                               joiner->ClusterState().members.size(),
                               joiner->Status().configuration.voters.size());
        }));

    auto const roster = NodeRoster::Build(fleet.joiner, RosterClock(), nullptr);
    INFO("the follower's roster refused: " << (roster.has_value() ? std::string {} : roster.error().reason));
    REQUIRE(roster.has_value());
    Unwrap(roster)->Applied(joiner->ClusterState());
    CHECK(Unwrap(roster)->StandingOf("n1", Testing::TestKeyPair("n1").PublicKey()) == ServerStanding::Voter);
}

TEST_CASE("A joined learner is replicated to over the session it dialled and is counted by no quorum",
          "[node][consensus][formation][fleet][learner]")
{
    // A learner binds no Raft port, so nothing can dial it: the leader's entries reach it only over
    // the TWO-WAY session the learner itself dialled. A learner that holds the state recording it
    // got there that way and no other.
    TwoTierFleet fleet;
    StartTwoTierFleet(fleet, Cluster::MemberSeat::Learner, FormationHooks {});
    auto const& founder = fleet.founderTier;
    auto const& joiner = fleet.joinerTier;
    REQUIRE(joiner->BoundEndpoint().empty());
    REQUIRE(joiner->Direction() == Consensus::RaftWire::SessionDirection::TwoWay);

    REQUIRE(Testing::WaitUntil(
        "the joined learner to hold the state that records it",
        [&joiner] {
            return joiner->Status().knownLeader == std::optional<Consensus::NodeId> { "n1" }
                   && RecordsJoiner(joiner->ClusterState(), Cluster::MemberSeat::Learner);
        },
        [&joiner] { return std::format("commit {}", joiner->Status().commitIndex.value); }));
    CHECK(founder->Status().configuration.voters == std::vector<Consensus::NodeId> { "n1" });
}

TEST_CASE("A learner started as main starts it names its leader's scheduling endpoint, never its Raft one",
          "[node][consensus][formation][fleet][learner][scheduling-redirect]")
{
    // #1639 at the door it is wired through: `StartConsensusOrExplain`'s role observer tells the
    // publisher who leads on every node, a learner that runs no scheduler included, and what it
    // tells is the leader's RECORDED `0xFC` endpoint -- the founder's `office:6674` -- never the
    // Raft endpoint the learner dials it at, nor the learner's own `0xFC` endpoint.
    //
    // Everything the joiner's tier borrows is declared ABOVE the fleet, so the fleet's destructor
    // stops the tier before any of it goes -- a failed REQUIRE below included.
    NullLogger logger;
    AtomicMetricsSink metrics;
    std::unique_ptr<SchedulerTier> const noScheduler;
    AnnouncedEndpoint const joinerAdvertised { "laptop.example:6674" };
    auto const roster = NodeRoster::Build(Testing::FirstStart(NodeConfig {}), RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    Testing::MemoryOpener opener;
    SharedCacheHost sharedCache { "n2", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheDirectory directory { "n2", {} };
    AppliedSchedulers schedulers { Testing::FirstStart(NodeConfig {}), AsConfigured };
    KnownSchedulingLeader knownLeader;
    SchedulingLeaderPublisher schedulingLeader { knownLeader };
    std::optional<NodeMembership> membership;

    TwoTierFleet fleet;
    AdmitTwoTierJoiner(fleet, Cluster::MemberSeat::Learner);
    REQUIRE(RunsConsensus(fleet.joiner));
    REQUIRE_FALSE(ServesScheduler(fleet.joiner));
    membership.emplace(fleet.joiner, membershipLog);

    auto joined =
        StartConsensusOrExplain(fleet.joiner,
                                noScheduler,
                                joinerAdvertised,
                                Testing::TestKeyPair("n2"),
                                *membership,
                                *Unwrap(roster),
                                schedulers,
                                schedulingLeader,
                                SharedCacheListeners { .directory = directory, .host = sharedCache, .upstream = nullptr },
                                metrics,
                                logger,
                                nullptr,
                                FormationHooks {});
    INFO("the joiner's start refused: " << RefusalOf(joined));
    REQUIRE(joined.has_value());
    fleet.joinerTier = std::move(*joined);
    REQUIRE(fleet.joinerTier != nullptr);

    auto const leaderEndpoint = std::string { TestAdvertised().Current() };
    REQUIRE(leaderEndpoint != fleet.founder.raftListen);
    REQUIRE(leaderEndpoint != joinerAdvertised.Current());
    auto const& joiner = fleet.joinerTier;
    CHECK(Testing::WaitUntil(
        "the learner to name its leader's recorded 0xFC endpoint",
        [&knownLeader, &leaderEndpoint] { return knownLeader.LeaderSchedulingEndpoint() == leaderEndpoint; },
        [&knownLeader, &joiner] {
            return std::format("names '{}' (known leader {}, commit {})",
                               knownLeader.LeaderSchedulingEndpoint(),
                               joiner->Status().knownLeader.value_or("(none)"),
                               joiner->Status().commitIndex.value);
        }));
}

namespace
{
/// The founder's half of the enrollment route, wired the way `main` wires it over a REAL tier: its
/// scheduler administers the founder's consensus, and the responder answers a joiner's `Enroll` and an
/// operator's approval through that scheduler -- so an approval is a proposal to the founder's own
/// Raft log, committed and applied there, never a fake's copy of `Apply`.
struct FounderEnrollment
{
    /// @param fleet The fleet whose founder answers; its tier must lead.
    explicit FounderEnrollment(TwoTierFleet& fleet):
        self { CompileCacheWire::FleetSummary { .clusterId = fleet.founder.clusterId,
                                                .state = CompileCacheWire::FleetState::Established,
                                                .leaderId = "n1",
                                                .nodeId = "n1" } }
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        service.AdministerWith(*fleet.founderTier);
    }

    /// The machine asking to join: on no member list.
    static constexpr std::string_view JoinerAddress = "198.51.100.4";

    /// The operator's machine: where it dials from, and the node it is, whose key the roster holds in
    /// a VOTER's seat, so the ticket it presents may decide (W-1).
    static constexpr std::string_view OperatorAddress = "10.0.0.7";
    static constexpr std::string_view OperatorMachine = "operator-pc";

    core::platform::ManualClock clock;                                         ///< The window's clock.
    core::platform::ManualWallClock wallClock;                                 ///< The scheduler's wall clock.
    AtomicMetricsSink metrics;                                                 ///< What the route counts.
    NullLogger logger;                                                         ///< Where it says what it does.
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner(); ///< What the scheduler signs with.
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} }; ///< The founder's scheduler.
    /// Who may decide: the production fold over a roster holding the operator machine's key in a
    /// voter's seat. A host list labelled `MachineTicket` would admit an address with no ticket at all,
    /// a route production does not have, so the operator PRESENTS its ticket (`Ask`).
    Testing::RosterFold const operators { { std::string { OperatorMachine } }, {}, { std::string { OperatorMachine } } };
    Distributed::IMembershipOracle const& membership = operators.admitted; ///< What the responder asks.
    NodeConditions conditions;                                             ///< What the window raises.
    EnrollmentWindow window { clock, &conditions, &metrics, wallClock };   ///< The pending list.
    Testing::ScriptedSummarySource self;                                   ///< What the founder says about itself.
    Ed25519KeyPair const identity = Testing::TestKeyPair("n1");            ///< The founder's identity key.
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::Ascending(256) }; ///< Each row's challenge.
    EnrollmentResponder responder { window, service, membership, self, identity, random, metrics, logger };

    /// Put @p frame to the responder as a peer at @p peer, and return the reply's ENROLL payload.
    /// @param frame The request.
    /// @param peer Who asks.
    /// @return The reply, its payload copied out; a refusal fails the case.
    [[nodiscard]] std::vector<std::byte> Ask(std::span<std::byte const> frame, std::string_view peer)
    {
        // The operator's connection carries the ticket its own node minted, verified by the endpoint's
        // AUTH; every other peer proves nothing.
        auto const facts =
            peer == OperatorAddress
                ? PeerIdentity { .host = std::string { peer },
                                 .authenticatedMachine = Testing::IdentityOf(std::string { OperatorMachine }) }
                : PeerIdentity { .host = std::string { peer } };
        auto const reply = core::async::syncRun(responder.Answer(frame, facts)).bytes;
        auto const header = CompileCacheWire::DecodeReplyHeader(reply);
        REQUIRE(header.has_value());
        REQUIRE(reply.size() >= CompileCacheWire::ReplyHeaderSize + Unwrap(header).payloadLength);
        auto const payload =
            std::span<std::byte const> { reply }.subspan(CompileCacheWire::ReplyHeaderSize, Unwrap(header).payloadLength);
        if (Unwrap(header).status == CompileCacheWire::Status::Error)
        {
            auto const refusal = CompileCacheWire::DecodeErrorPayload(payload);
            REQUIRE(refusal.has_value());
            FAIL("refused: " << Unwrap(refusal).second);
        }
        return { payload.begin(), payload.end() };
    }
};
} // namespace

TEST_CASE("A learner enrolled through its leader's own route is recorded under the key it enrolled with, "
          "and a keyless desire keeps it",
          "[node][consensus][formation][fleet][learner][enrollment]")
{
    // The record half of the enrollment route over REAL tiers (step 20's C-5, carried to batch 3):
    // a signed `Enroll`, the leader's challenge, an operator's approval proposed to the founder's own
    // log, and the joiner started from the roster that approval answered -- the roster's bytes, read
    // the way `ApplyFormation` reads a formation record, never a member list the case typed.
    TwoTierFleet fleet;
    StartTwoTierFounder(fleet);
    // The founder records ITSELF at its first reconcile pass as leader, long before an operator
    // approves anybody in a real fleet -- and an approval's roster is the state's, so one answered
    // before then would name no voter for the joiner to dial.
    REQUIRE(Testing::WaitUntil(
        "the founder to record itself",
        [&fleet] {
            return std::ranges::contains(fleet.founderTier->ClusterState().members, "n1", &Cluster::ClusterMember::id);
        },
        [&fleet] { return std::format("commit {}", fleet.founderTier->Status().commitIndex.value); }));
    FounderEnrollment seed { fleet };

    auto const joinerPair = Testing::TestKeyPair("n2");
    auto self = JoinerIdentity { .nodeId = "n2",
                                 .nodeEndpoint = "n2.fleet.test:6674",
                                 .role = CompileCacheWire::EnrollRole::Learner,
                                 .publicKey = joinerPair.PublicKey() };
    auto nonce = std::array<std::byte, CompileCacheWire::NodeChallengeBytes> {};
    nonce.fill(std::byte { 0x5A });
    auto const poll = [&] {
        auto const payload = seed.Ask(EncodeSignedEnroll(self, joinerPair, nonce), FounderEnrollment::JoinerAddress);
        auto const reply = CompileCacheWire::DecodeEnrollReply(payload);
        REQUIRE(reply.has_value());
        // A `Pending` answer hands the row's challenge, and the joiner's next request signs over it.
        if (Unwrap(reply).challenge.has_value())
            self.challenge = Unwrap(reply).challenge;
        return std::pair { Unwrap(reply).outcome,
                           std::vector<std::byte> { Unwrap(reply).roster.begin(), Unwrap(reply).roster.end() } };
    };

    REQUIRE(poll().first == CompileCacheWire::EnrollOutcome::Pending);
    REQUIRE(self.challenge.has_value());
    static_cast<void>(
        seed.Ask(CompileCacheWire::EncodeEnrollApprove("n2", joinerPair.PublicKey()), FounderEnrollment::OperatorAddress));

    // Answered `Approved` only once the founder's own state RECORDS the joiner, which is the commit.
    auto roster = std::vector<std::byte> {};
    REQUIRE(Testing::WaitUntil(
        "the founder to answer the approved joiner with its roster",
        [&] {
            auto [outcome, bytes] = poll();
            if (outcome != CompileCacheWire::EnrollOutcome::Approved)
                return false;
            roster = std::move(bytes);
            return true;
        },
        [&fleet] { return std::format("commit {}", fleet.founderTier->Status().commitIndex.value); }));
    auto const admitted = DescribeAdmission(self, roster, fleet.joinerDirectory / "state");
    INFO("the roster was refused: " << (admitted.has_value() ? std::string {} : admitted.error()));
    REQUIRE(admitted.has_value());

    // The record carries the ENROLLED key and the 0xFC endpoint the `Enroll` stated, in a learner's
    // seat -- and the voters are the founder alone.
    auto const recordsEnrolled = [&self](Cluster::ClusterState const& state) {
        return std::ranges::any_of(state.members, [&self](Cluster::ClusterMember const& member) {
            return member.id == "n2" && member.seat == Cluster::MemberSeat::Learner && member.publicKey == self.publicKey
                   && member.schedulerEndpoint == self.nodeEndpoint;
        });
    };
    CHECK(recordsEnrolled(fleet.founderTier->ClusterState()));

    // The joiner, started from the roster the approval carried, signed by the founder's key.
    fleet.joiner = Testing::FirstStart(NodeConfig {});
    fleet.joiner.nodeId = "n2";
    fleet.joiner.raftListen = std::format("127.0.0.1:{}", UnansweredPort());
    fleet.joiner.raftSelf = "127.0.0.1";
    fleet.joiner.clusterDir = fleet.joinerDirectory / "state";
    auto const record = Cluster::FormationRecord {
        .mode = Cluster::NodeMode::Learner,
        .own = {},
        .joining = std::nullopt,
        .fleet = Cluster::FleetMembership { .clusterId = fleet.founder.clusterId,
                                            .roster = roster,
                                            .createdAtUnixSeconds = 0,
                                            .admittedBy = seed.identity.PublicKey() },
        .archivePending = std::nullopt,
        .rejectedBy = std::nullopt,
        .askedJoins = {},
    };
    auto const applied = ApplyFormation(fleet.joiner, record, Cluster::FleetEndpoints {});
    INFO("the formation was refused: " << (applied.has_value() ? std::string {} : applied.error()));
    REQUIRE(applied.has_value());
    auto joined = StartTier(fleet.joiner, fleet.joinerMetrics, fleet.joinerLogger);
    INFO("the joiner's start refused: " << RefusalOf(joined));
    REQUIRE(joined.has_value());
    fleet.joinerTier = std::move(*joined);
    auto const& joiner = fleet.joinerTier;
    REQUIRE(Testing::WaitUntil(
        "the enrolled learner to follow its leader and apply the state that records it",
        [&joiner, &recordsEnrolled] {
            return joiner->Status().knownLeader == std::optional<Consensus::NodeId> { "n1" }
                   && recordsEnrolled(joiner->ClusterState());
        },
        [&joiner] { return std::format("commit {}", joiner->Status().commitIndex.value); }));
    CHECK(fleet.founderTier->Status().configuration.voters == std::vector<Consensus::NodeId> { "n1" });

    // A KEYLESS desire for the learner, shaped as discovery hands one over (`ConsensusTier::Desire`):
    // where it answers consensus, no `0xFC` opinion, and no key. It MOVES the record -- the reconciler
    // re-proposes the learner at the stated endpoint, which is what this case waits on -- and the key
    // the enrollment recorded stands: a command with no key keeps the recorded one (`KeyToRecord`).
    // For a member the state already records, that rule is the whole of the protection;
    // `WithLiveKeys` fills a key only for an id the state does not record.
    auto const desiredRaft = std::format("127.0.0.1:{}", UnansweredPort());
    auto const desire = std::array { Cluster::DesiredMember {
        .id = "n2", .raftEndpoint = desiredRaft, .schedulerEndpoint = std::nullopt, .publicKey = std::nullopt } };
    fleet.founderTier->Desire(desire);
    auto const moved = [&desiredRaft](Cluster::ClusterState const& state) {
        auto const* const member = core::findOrNull(state.members, std::string_view { "n2" }, &Cluster::ClusterMember::id);
        return member != nullptr && member->raftEndpoint == desiredRaft;
    };
    REQUIRE(Testing::WaitUntil(
        "the founder to re-propose the learner its keyless desire names",
        [&fleet, &moved] { return moved(fleet.founderTier->ClusterState()); },
        [&fleet] { return std::format("commit {}", fleet.founderTier->Status().commitIndex.value); }));
    auto const after = fleet.founderTier->ClusterState();
    auto const* const kept = core::findOrNull(after.members, std::string_view { "n2" }, &Cluster::ClusterMember::id);
    REQUIRE(kept != nullptr);
    CHECK(kept->publicKey == self.publicKey);
    CHECK(kept->seat == Cluster::MemberSeat::Learner);
    CHECK(kept->schedulerEndpoint == self.nodeEndpoint);
    CHECK(fleet.founderTier->Status().configuration.voters == std::vector<Consensus::NodeId> { "n1" });
}

TEST_CASE("A forgotten learner hears its key revoked through its own tier, from the voter it dials",
          "[node][consensus][formation][fleet][learner]")
{
    // The route the formation harness can only stand in for: the founder forgets the learner, its
    // roster revokes the learner's key, and the learner's OWN transport -- dialling that voter --
    // hears the signed KeyRevoked verdict and hands it to the formation hook the tier was started
    // with. Nothing here tells the learner the state; it is no longer replicated to.
    struct Heard
    {
        std::mutex lock;                          ///< Guards the list: the hook runs on the reactor.
        std::vector<Consensus::NodeId> acceptors; ///< Every acceptor that said so.
    };
    auto const heard = std::make_shared<Heard>();
    TwoTierFleet fleet;
    StartTwoTierFleet(fleet,
                      Cluster::MemberSeat::Learner,
                      FormationHooks { .onState = {}, .onOwnKeyRevoked = [heard](Consensus::NodeId const& acceptor) {
                                          auto const guard = std::scoped_lock { heard->lock };
                                          heard->acceptors.push_back(acceptor);
                                      } });
    auto const& joiner = fleet.joinerTier;
    REQUIRE(Testing::WaitUntil(
        "the learner to follow its leader",
        [&joiner] { return joiner->Status().knownLeader == std::optional<Consensus::NodeId> { "n1" }; },
        [&joiner] { return std::format("known leader {}", joiner->Status().knownLeader.value_or("(none)")); }));

    auto const forget = Cluster::PrepareForget(fleet.founderTier->ClusterState(),
                                               Consensus::Configuration { .voters = { "n1" }, .learners = { "n2" } },
                                               "n2",
                                               Testing::TestKeyPair("n2").PublicKey());
    INFO((forget.has_value() ? std::string {} : forget.error().context));
    REQUIRE(forget.has_value());
    auto const proposed = fleet.founderTier->ProposeToCluster(Testing::Unwrap(forget));
    INFO((proposed.has_value() ? std::string {} : proposed.error().context));
    REQUIRE(proposed.has_value());
    auto const told = [&heard] {
        auto const guard = std::scoped_lock { heard->lock };
        return heard->acceptors;
    };

    // What the route costs, from its waits. The founder's roster adopts what its configuration
    // counts once a reconcile pass, so whether it counted the new learner when the forget landed
    // decides between two routes, and the case takes the longer:
    //
    //  - counted: the commit's own frame keeps the session (the key stays live for a member the
    //    configuration counts, #1555); the pass that drops the learner wakes it; the learner's redial
    //    one backoff later hears the verdict. One pass, one backoff.
    //  - not yet counted: the commit's frame closes the session; the redial one backoff later is
    //    accepted, the pass between having counted it; the configuration drops it at the next pass,
    //    which wakes that IDLE session -- the leader sends a member it no longer counts nothing --
    //    and the redial after the doubled backoff (the session carried nothing each way) hears the
    //    verdict. Two passes, two backoffs.
    //
    // One pass more for the commit before the first and a host running the suite in parallel.
    // Never the hang guard, which a session nobody asks would sit out in full.
    //
    // Its rate, not a run: `scripts/flake-rate.sh --runs 50` over this case's tag passed 50 of 50 (N=50,
    // so a rate below about 1 in 50 is not excluded) on Linux clang-debug, 32 cores, with the whole
    // FastCacheTest suite running at -j32 beside it for the full 316 s (nine complete passes).
    constexpr auto Learner = Consensus::RaftWire::SessionDirection::TwoWay;
    auto const firstBackoff = Consensus::DialBackoffOf(Learner).initial;
    auto const within = std::chrono::duration_cast<std::chrono::milliseconds>(
        (3 * ConsensusTier::ReconcileInterval) + firstBackoff + Consensus::NextBackoff(Learner, firstBackoff));
    REQUIRE(within < Testing::WaitHangGuard);
    CHECK(Testing::WaitUntil(
        "the learner's tier to hear its key revoked",
        [&told] { return !told().empty(); },
        [&fleet] {
            auto const status = fleet.founderTier->Status();
            auto const state = fleet.founderTier->ClusterState();
            return std::format("no KeyRevoked verdict reached the hook; the founder counts {} learner(s), records n2: "
                               "{}, n2's key revoked: {}; the learner's own-key-revoked dial refusals: {}",
                               status.configuration.learners.size(),
                               std::ranges::contains(state.members, std::string { "n2" }, &Cluster::ClusterMember::id),
                               state.IsRevoked(Testing::TestKeyPair("n2").PublicKey()),
                               fleet.joinerMetrics.Read(IMetricsSink::Counter::RaftPeerDialsRefusedOwnKeyRevoked));
        },
        Testing::WaitOptions { .step = {}, .context = {}, .bound = within, .rest = Testing::WaitRest }));
    auto const acceptors = told();
    REQUIRE_FALSE(acceptors.empty());
    CHECK(acceptors.front() == "n1");
}
