// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "NodeMembership.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;

/// `NodeMembership` reports the keyless-widening refusal here; no case asserts on it.
namespace
{
FastCache::NullLogger membershipLog;
}
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
    // No I/O in either case. The `--node-id` case returns before anything is built;
    // the `--listen-raft` case is given no `--raft-peer` naming itself, so it reaches
    // `ConsensusTier::Start` and is refused there, on the rule the startup table owns
    // -- which is the observation, because a tier that had skipped the gate would have
    // returned a null tier instead.
    NullLogger logger;
    std::unique_ptr<SchedulerTier> const noScheduler;

    SECTION("--node-id with no --listen-raft builds no tier")
    {
        NodeConfig cfg;
        cfg.nodeId = "n1";
        cfg.raftPeers = { Unwrap(Cluster::ParseMemberSpec("n1=10.0.0.1:6680")) };
        NodeMembership membership { cfg, membershipLog };

        auto const tier = StartConsensusOrExplain(cfg, noScheduler, "127.0.0.1:6674", membership, logger);
        REQUIRE(tier.has_value());
        CHECK(*tier == nullptr);
    }

    SECTION("--listen-raft with no --node-id gets past the gate")
    {
        NodeConfig cfg;
        cfg.raftListen = "6680";
        NodeMembership membership { cfg, membershipLog };

        // Refused, and refused by NAME: a null tier here would mean the gate is still
        // reading the id, and any other refusal would mean it got somewhere this test
        // does not intend to reach.
        auto const tier = StartConsensusOrExplain(cfg, noScheduler, "127.0.0.1:6674", membership, logger);
        REQUIRE_FALSE(tier.has_value());
        CHECK(tier.error() == ConsensusNamesNoSelfPeerRefusal);
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
    auto const progress = Consensus::RaftDriver::Progress { .members = { "n1", "n2", "n3" },
                                                            .commitIndex = Consensus::LogIndex { .value = 12 },
                                                            .term = Consensus::Term { .value = 4 },
                                                            .role = Consensus::Role::Leader,
                                                            .knownLeader = Consensus::NodeId { "n1" } };

    auto const status = ConsensusStatusFrom(progress);

    // Verbatim, in the order consensus holds it: a caller comparing two nodes needs
    // to see the order a configuration was adopted in, and sorting here would take
    // that away with nothing saying so.
    CHECK(status.members == std::vector<Consensus::NodeId> { "n1", "n2", "n3" });
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
    auto const status = ConsensusStatusFrom(Consensus::RaftDriver::Progress { .members = {},
                                                                              .commitIndex = Consensus::LogIndex {},
                                                                              .term = Consensus::Term {},
                                                                              .role = Consensus::Role::Follower,
                                                                              .knownLeader = Consensus::NodeId { "n1" } });

    CHECK(status.members.empty());
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
