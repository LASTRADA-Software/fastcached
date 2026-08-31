// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;
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
