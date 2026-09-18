// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"

#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryDatagram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <tests/CoHostedDatagram.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::CoHostedDatagramSocket;
using FastCache::Testing::RosterPeerKeys;
using FastCache::Testing::SharedRoster;
using FastCache::Testing::TestBeaconPort;
using FastCache::Testing::TestKeyPair;

namespace
{
/// What one node announces about itself.
/// @param nodeId Its identity.
/// @param beaconAddress Where this node announces itself.
/// @return The configuration.
[[nodiscard]] Cluster::DiscoveryConfig ConfigFor(std::string const& nodeId, DatagramAddress beaconAddress)
{
    return Cluster::DiscoveryConfig { .clusterId = "fleet",
                                      .nodeId = nodeId,
                                      .raftEndpoint = nodeId + ".local:6675",
                                      .beaconAddress = std::move(beaconAddress),
                                      .beaconInterval = 15s,
                                      .challengeLifetime = 30s };
}

/// One node's tier, plus what its observer was told.
///
/// Named `Peer` rather than `Node`, which is the namespace this whole file is
/// `using` -- a struct by that name makes every later mention of it ambiguous.
struct Peer
{
    NullLogger logger;
    std::vector<Cluster::DesiredMember> seen;
    AtomicMetricsSink metrics;
    RosterPeerKeys keys;
    std::unique_ptr<DiscoveryTier> tier;

    /// One node per machine, which is the ordinary deployment.
    /// @param bus The segment.
    /// @param nodeId This node's identity; its key is `TestKeyPair(nodeId)`.
    /// @param roster Whose key every id is, as this node's replicated state says.
    Peer(DatagramBus& bus, std::string const& nodeId, std::shared_ptr<SharedRoster const> roster):
        Peer(bus.Open(DatagramAddress { .host = nodeId, .port = TestBeaconPort }),
             DatagramBus::BroadcastAddress(),
             nodeId,
             std::move(roster))
    {
    }

    /// Over a socket and a beacon address somebody else chose.
    /// @param socket Where this node's datagrams come from and go.
    /// @param beaconAddress Where it announces itself.
    /// @param nodeId This node's identity; its key is `TestKeyPair(nodeId)`.
    /// @param roster Whose key every id is, as this node's replicated state says.
    Peer(std::unique_ptr<IDatagramSocket> socket,
         DatagramAddress beaconAddress,
         std::string const& nodeId,
         std::shared_ptr<SharedRoster const> roster):
        keys { TestKeyPair(nodeId), std::move(roster) },
        tier { DiscoveryTier::Over(
            std::move(socket),
            ConfigFor(nodeId, std::move(beaconAddress)),
            keys,
            [this](std::span<Cluster::DesiredMember const> peers) { seen.assign(peers.begin(), peers.end()); },
            metrics,
            logger) }
    {
    }
};

/// Step both sides @p rounds times.
/// @param a One side.
/// @param b The other.
/// @param rounds How many turns each gets.
void Settle(Peer& a, Peer& b, int rounds);

/// A step short enough that a whole handshake costs milliseconds.
///
/// A timeout rather than a sleep: the bus delivers into an inbox synchronously, so
/// every assertion below is decided by the ORDER of the steps and not by how long
/// any of them waits. What the timeout bounds is only the last step of each side,
/// where there is nothing left to read.
constexpr auto Step = 5ms;

void Settle(Peer& a, Peer& b, int rounds)
{
    for ([[maybe_unused]] auto const round: std::views::iota(0, rounds))
    {
        CHECK(a.tier->Step(Step));
        CHECK(b.tier->Step(Step));
    }
}
} // namespace

TEST_CASE("Two nodes the roster knows find and prove each other", "[node][discovery]")
{
    // The whole handshake, driven by hand: no threads, no sleeps, and a failure
    // names the step it happened at rather than timing out.
    DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer first { bus, "n1", roster };
    Peer second { bus, "n2", roster };

    // Each announces itself; the bus doubles a broadcast back to its sender, exactly
    // as a real one does, which is the case `PeerDirectory` must ignore.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    // Each reads the other's beacon and challenges it.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    // Each answers the challenge it was given.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    // And each checks the proof it was sent.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    REQUIRE(first.tier->AuthenticatedCount() == 1);
    REQUIRE(second.tier->AuthenticatedCount() == 1);

    // What reaches the observer is a DESIRE with no opinion about the scheduler
    // endpoint. Discovery proved where `n2` answers CONSENSUS -- that is what the
    // signature covered -- and knows nothing about the port clients speak to, so saying
    // `""` would clear whatever `n2` had announced about itself.
    REQUIRE(first.seen.size() == 1);
    CHECK(first.seen.front().id == "n2");
    CHECK(first.seen.front().raftEndpoint == "n2.local:6675");
    CHECK_FALSE(first.seen.front().schedulerEndpoint.has_value());

    // And no opinion about the KEY either, although one was just proved (#178): it is the
    // key the roster already holds, and a desire outlives the moment it was stated, so a
    // key named here would be proposed straight back over an operator who re-keyed `n2`.
    CHECK_FALSE(first.seen.front().publicKey.has_value());
}

TEST_CASE("A node that cannot name itself is never desired", "[node][discovery]")
{
    // #159 at the far end of the path it travels: what this tier hands its observer
    // becomes `ConsensusTier::Desire`, then a `MembershipProposals` entry, then a
    // `ClusterMember` in replicated state.
    //
    // Stopped at the directory rather than filtered here, so that no proposal is
    // ever GENERATED. One a leader would refuse every pass, forever, is the shape
    // that stalls a cluster: `Reconcile` abandons the pass at the first refusal and
    // never reaches `ReconcileQuorum`.
    DatagramBus bus;
    // A truncated three-byte sequence -- the shape is right and the bytes stop
    // early -- which reaches the endpoint too, since a node's endpoint is derived
    // from its id here exactly as an operator's `--raft-peer` derives from what
    // they typed.
    auto const namelessId = std::string { "n2-\xE2\x82" };
    auto const roster = SharedRoster::Of({ "n1", namelessId });
    Peer good { bus, "n1", roster };
    Peer nameless { bus, namelessId, roster };

    // Four rounds is well over the three legs a handshake takes, so this fails as
    // "it was admitted" rather than as "it had not finished yet".
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        CHECK(good.tier->Step(Step));
        CHECK(nameless.tier->Step(Step));
    }

    // Never seen, so never proved, so never desired. The key was one the roster holds and
    // made no difference: this is a refusal about what the peer CLAIMS rather than about
    // what it holds.
    CHECK(good.tier->AuthenticatedCount() == 0);
    CHECK(good.seen.empty());

    // And the other way round, which is what makes this a one-sided refusal rather
    // than a segment that stops working: the refusal is about what a peer CLAIMS,
    // so the node nobody can name still sees, proves and desires everybody else.
    CHECK(nameless.tier->AuthenticatedCount() == 1);
    REQUIRE(nameless.seen.size() == 1);
    CHECK(nameless.seen.front().id == "n1");
}

TEST_CASE("A beacon from an UNKNOWN key is reported and never desired", "[node][discovery][security]")
{
    // **The acceptance case for #178's discovery half.** Under the shared key a proof WAS
    // membership, so any machine holding the file was desired -- and a desired machine the
    // reconciler would admit. Now a machine proves possession of a key of its own, and
    // proving it perfectly is not enough: the roster does not hold that key for `n2`, so
    // `n2` is seen, counted, reported, and never handed to `Desire`.
    DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1" });
    Peer honest { bus, "n1", roster };
    Peer stranger { bus, "n2", roster };

    Settle(honest, stranger, 6);

    CHECK(honest.tier->AuthenticatedCount() == 0);
    CHECK(honest.seen.empty());
    CHECK(honest.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) >= 1);
}

TEST_CASE("The same stranger IS desired once the roster holds its key", "[node][discovery][security]")
{
    // The control for the case above, through the same arrangement with ONE fact changed:
    // the roster now holds `n2`'s key. Without it the refusal above passes under a tier
    // that desires nobody at all -- which would also end auto-admission, and would also
    // break every discovered address change.
    DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1" });
    roster->Admit("n2", TestKeyPair("n2").PublicKey());
    Peer honest { bus, "n1", roster };
    Peer stranger { bus, "n2", roster };

    Settle(honest, stranger, 6);

    CHECK(honest.tier->AuthenticatedCount() == 1);
    REQUIRE(honest.seen.size() == 1);
    CHECK(honest.seen.front().id == "n2");
    CHECK(honest.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 0);
}

TEST_CASE("A peer whose proven key the roster has since revoked is not desired again", "[node][discovery][security]")
{
    // The authenticated set is re-published whenever ANY peer proves itself, and the
    // directory remembers a proof from whenever it was taken. So what is published is
    // re-asked of the roster at the moment it is published: `n2` proved its key, the key
    // was then revoked, and `n3` proving ITS key must not carry `n2` along with it.
    DatagramBus bus;
    auto roster = SharedRoster::Of({ "n1", "n2", "n3" });
    Peer first { bus, "n1", roster };
    Peer second { bus, "n2", roster };

    Settle(first, second, 4);
    REQUIRE(first.seen.size() == 1);
    REQUIRE(first.seen.front().id == "n2");

    roster->Revoke("n2");

    Peer third { bus, "n3", roster };
    Settle(first, third, 4);

    REQUIRE_FALSE(first.seen.empty());
    CHECK(std::ranges::any_of(first.seen, [](Cluster::DesiredMember const& m) { return m.id == "n3"; }));
    CHECK(std::ranges::none_of(first.seen, [](Cluster::DesiredMember const& m) { return m.id == "n2"; }));
}

TEST_CASE("Two nodes on one host find and prove each other", "[node][discovery]")
{
    // Issue #126, at the tier that owns the sockets. Both nodes listen on the one
    // beacon port -- they have to, a beacon being a broadcast -- so both hear each
    // other. What used to fail is everything after that: a unicast to a shared
    // port reaches only one of the sockets on it, and the challenge and the proof
    // are both unicast to wherever the last datagram came from, so each landed on
    // whichever co-hosted node the kernel picked. Nothing logged; the pair simply
    // stayed seen-and-unproved.
    //
    // Driven step by step rather than by a settle loop, because each step here is
    // one leg of the handshake and a failure should name the leg.
    DatagramBus bus;
    auto const beacon = DatagramBus::BroadcastAddressOn(TestBeaconPort);

    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer first { CoHostedDatagramSocket(bus, "host", 40001), beacon, "n1", roster };
    Peer second { CoHostedDatagramSocket(bus, "host", 40002), beacon, "n2", roster };

    // Each announces itself, then reads the other's beacon and challenges it, then
    // answers the challenge it was given, then checks the proof it was sent. The
    // extra rounds are slack: a shared-port socket looks at one of its halves and
    // waits on the other, so a datagram can need one more step to be reached.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 8))
    {
        CHECK(first.tier->Step(Step));
        CHECK(second.tier->Step(Step));
    }

    REQUIRE(first.tier->AuthenticatedCount() == 1);
    REQUIRE(second.tier->AuthenticatedCount() == 1);

    // And each learned where the OTHER answers Raft, not where it does -- the
    // failure a co-hosted pair would show if the two ever crossed.
    REQUIRE(first.seen.size() == 1);
    CHECK(first.seen.front().id == "n2");
    CHECK(first.seen.front().raftEndpoint == "n2.local:6675");

    REQUIRE(second.seen.size() == 1);
    CHECK(second.seen.front().id == "n1");
    CHECK(second.seen.front().raftEndpoint == "n1.local:6675");
}

TEST_CASE("A peer discovery proves is recorded as a learner", "[node][discovery][learner]")
{
    // #1535, from the proof to the command a leader proposes: what this tier hands its
    // observer is what `ConsensusTier::Desire` stores and `MembershipProposals` decides
    // from. `n1` bootstrapped alone and `n2` has never been recorded or counted, so the
    // proof buys `n2` a learner's record -- replicated to, counted by nothing -- and a
    // vote stays the operator's to give.
    //
    // Since #178 the proof counts only because `n2`'s KEY is one `n1` already holds -- here
    // the roster knows it, as a `--raft-peer n2=...@<key>` typed on `n1` makes it known
    // before the state records any `n2`. A key nobody holds is reported and never desired
    // (the UNKNOWN-key case above), so this path records a learner and admits nobody new.
    DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer first { bus, "n1", roster };
    Peer second { bus, "n2", roster };

    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        CHECK(first.tier->Step(Step));
        CHECK(second.tier->Step(Step));
    }
    REQUIRE(first.seen.size() == 1);

    auto const plan = Cluster::MembershipProposals(
        Cluster::ClusterState {}, Consensus::Configuration { .voters = { "n1" }, .learners = {} }, first.seen);

    REQUIRE(plan.proposals.size() == 1);
    CHECK(plan.proposals.front()
          == Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                .key = "n2",
                                .value = "n2.local:6675",
                                .schedulerEndpoint = {},
                                .publicKey = std::nullopt,
                                .role = std::nullopt });
    CHECK(plan.forgotten.empty());
}

TEST_CASE("Two fleets on one segment ignore each other at the node's discovery tier", "[node][discovery]")
{
    // A cluster id is routing rather than authentication, and this is what it buys:
    // the challenge is never even issued, so a roster that knew both would not help.
    // Checked before a challenge goes out AND before one is answered.
    DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer ours { bus, "n1", roster };

    NullLogger otherLogger;
    AtomicMetricsSink otherMetrics;
    RosterPeerKeys otherKeys { TestKeyPair("n2"), roster };
    auto otherConfig = ConfigFor("n2", DatagramBus::BroadcastAddress());
    otherConfig.clusterId = "somebody-elses";
    auto const theirs = DiscoveryTier::Over(bus.Open(DatagramAddress { .host = "n2", .port = TestBeaconPort }),
                                            std::move(otherConfig),
                                            otherKeys,
                                            {},
                                            otherMetrics,
                                            otherLogger);

    for ([[maybe_unused]] auto const round: std::views::iota(0, 6))
    {
        CHECK(ours.tier->Step(Step));
        CHECK(theirs->Step(Step));
    }

    CHECK(ours.tier->AuthenticatedCount() == 0);
    CHECK(theirs->AuthenticatedCount() == 0);
}
