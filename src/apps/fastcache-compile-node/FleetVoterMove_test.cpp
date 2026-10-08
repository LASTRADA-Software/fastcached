// SPDX-License-Identifier: Apache-2.0
//
// A VOTER whose address vanished, announcing where it answers now through its OWN rounds -- the
// presence loop's NODE-ANNOUNCE and the worker's REGISTER -- rather than an announcement a case
// hands the leader directly.
//
// The membership side of a move (`AnnouncedEndpointDesires` coupling the Raft host, the commit,
// every node learning the new endpoint) is `Cluster/MembershipCluster_test.cpp`'s, and every case
// there injects the announcement AT THE LEADER. What only a fleet shows is whether the announcement
// gets there at all: a voter whose old address is gone hears no leader, so its own scheduler -- the
// first endpoint its rounds dial -- knows nobody to redirect to and answers `NotLeader` naming
// NOBODY. Before the fix the voter's rounds dialled nothing else (`AppliedSchedulers::Current`
// answered its own endpoint alone), so the leader never heard of the move and the voter stayed
// unreachable until its old address came back.
#include "AnnounceTestFixture.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodePresenceTier.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/FleetHarness.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace FastCache::Node::AnnounceTesting;

namespace
{

/// The moving voter's member id.
constexpr std::string_view Laptop = "laptop";

/// Where the laptop's own scheduler answers its own rounds: `SchedulersOf` for a node serving one on
/// the default wildcard bind.
constexpr std::string_view LaptopOwnScheduler = "127.0.0.1:6674";

/// The two voters that did not move, each a machine of its own. Their member ids are their
/// endpoints, as the harness keys every scheduler it signs for.
constexpr std::string_view Office = "10.0.0.1:6674";
constexpr std::string_view Annex = "10.0.0.2:6674";

/// Where the laptop answered before, as every applied state still records it.
constexpr std::string_view LaptopBefore = "10.0.0.3:6674";

/// Where the laptop answers now, on the network it woke on.
constexpr std::string_view LaptopNow = "192.168.1.50:6674";

/// A voter record for @p id, answering `0xFC` at @p schedulerEndpoint.
/// @param id The member.
/// @param schedulerEndpoint Where its scheduler answers.
/// @return The record, keyed with the member's test key.
[[nodiscard]] Cluster::ClusterMember VoterRecord(std::string_view id, std::string_view schedulerEndpoint)
{
    return Cluster::ClusterMember { .id = std::string { id },
                                    .raftEndpoint = std::string { schedulerEndpoint },
                                    .schedulerEndpoint = std::string { schedulerEndpoint },
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                    .seat = Cluster::MemberSeat::Voter,
                                    .publicKey = Testing::TestKeyPair(std::string { id }).PublicKey() };
}

/// The fleet's applied state: three voters, the laptop at its OLD endpoint. The annex is listed
/// before the office, so a round that walks the list past the laptop's own scheduler reaches a
/// FOLLOWER first, and the leader only through the redirect that follower answers.
/// @return The state every node applied.
[[nodiscard]] Cluster::ClusterState ThreeVoters()
{
    auto state = Cluster::ClusterState {};
    state.members = { VoterRecord(Annex, Annex), VoterRecord(Laptop, LaptopBefore), VoterRecord(Office, Office) };
    state.rosterVersion = 1;
    return state;
}

/// The laptop's configuration: a voter of the fleet, serving its own scheduler on the default
/// wildcard bind, so its rounds start at `LaptopOwnScheduler`.
/// @return The configuration.
[[nodiscard]] NodeConfig LaptopVoter()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = std::string { Laptop };
    REQUIRE(cfg.formation.has_value());
    auto formation = Testing::Unwrap(cfg.formation);
    formation.mode = Cluster::NodeMode::Voter;
    formation.clusterId = std::string { Testing::FleetHarness::ClusterId };
    cfg.formation = std::move(formation);
    REQUIRE(ServesScheduler(cfg));
    REQUIRE(SchedulersOf(cfg, AsConfigured) == std::vector<std::string> { std::string { LaptopOwnScheduler } });
    return cfg;
}

/// A fleet in which the laptop's address has vanished: the office leads, the annex follows it, and
/// the laptop's own scheduler -- which no leader's message reaches any more -- knows no leader.
struct MovedVoterFleet
{
    /// @param leader Which unmoved voter leads.
    explicit MovedVoterFleet(std::string_view leader)
    {
        for (auto const endpoint: { Office, Annex, LaptopOwnScheduler })
        {
            fleet.AddScheduler(std::string { endpoint });
            fleet.SetClusterStateAt(endpoint, ThreeVoters());
        }
        fleet.ElectLeader(leader);
        fleet.ForgetLeaderAt(LaptopOwnScheduler);
        // Every round the laptop makes arrives from its new address, proving its own key.
        fleet.SetCallerHost("192.168.1.50");
        fleet.SetCallerIdentity(ProvenIdentity { .id = std::string { Laptop },
                                                 .key = Testing::TestKeyPair(std::string { Laptop }).PublicKey() });
        schedulers.Applied(ThreeVoters());
    }

    Testing::FleetHarness fleet;
    AnnounceFixture fix;
    NodeConfig cfg { LaptopVoter() };
    /// The production answer to "where does this node register", told what the laptop applied.
    AppliedSchedulers schedulers { cfg, AsConfigured };
};

/// Announce the laptop's presence once, as its presence loop's round does.
/// @param moved The fleet.
/// @param link The presence loop's link.
/// @return Whether a scheduler recorded the machine.
[[nodiscard]] bool AnnouncePresenceOnce(MovedVoterFleet& moved, SchedulerLink& link)
{
    auto const capacity = CompileCacheWire::CapacityFields {};
    auto const load = CompileCacheWire::LoadFields {};
    return AnnouncePresence(PresenceMessage { .endpoint = LaptopNow,
                                              .capacity = capacity,
                                              .load = load,
                                              .logger = moved.fix.logger,
                                              .prover = nullptr,
                                              .reachability = moved.fix.reachability,
                                              .joinMemos = {} },
                            link,
                            moved.fleet);
}

/// The endpoints the laptop's rounds were answered from, in the order they were dialled.
/// @param fleet The harness.
/// @return One per exchange.
[[nodiscard]] std::vector<std::string> Asked(Testing::FleetHarness const& fleet)
{
    auto asked = std::vector<std::string> {};
    for (auto const& call: fleet.Calls())
        asked.push_back(call.endpoint);
    return asked;
}

/// What the leader noted, as `(member, endpoint)`: the moved laptop at its new endpoint.
/// @return The one entry a delivered announcement leaves.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> LaptopNoted()
{
    return { { std::string { Laptop }, std::string { LaptopNow } } };
}

} // namespace

TEST_CASE("A moved voter's presence reaches the leader through the voters its applied state records",
          "[node][fleet][presence][roaming]")
{
    // RED before the fix: the round dialled the laptop's own scheduler alone, which answered
    // `NotLeader` naming nobody, and gave up -- the office never noted the move.
    MovedVoterFleet moved { Office };
    auto link = Testing::Unwrap(SchedulerLink::Over(moved.schedulers));

    CHECK(AnnouncePresenceOnce(moved, link));

    // Its own scheduler first -- a voter whose scheduler still knows the leader redirects from
    // there, as before -- then the first recorded voter, a follower, which redirects to the leader.
    CHECK(Asked(moved.fleet)
          == std::vector<std::string> { std::string { LaptopOwnScheduler }, std::string { Annex }, std::string { Office } });
    CHECK(moved.fleet.AnnouncedEndpointsAt(Office) == LaptopNoted());
    // Nobody but the leader notes anything: a follower refuses before it reads the payload.
    CHECK(moved.fleet.AnnouncedEndpointsAt(Annex).empty());
    CHECK(moved.fleet.AnnouncedEndpointsAt(LaptopOwnScheduler).empty());
}

TEST_CASE("A deposed leader whose address vanished announces through the voters that elected another",
          "[node][fleet][presence][roaming]")
{
    // The moved LEADER: CheckQuorum deposed it, the annex and the office elected the annex, and the
    // laptop's own scheduler -- the old leader's -- now knows nobody. The annex is the first recorded
    // voter, so it is asked directly and takes the announcement without a redirect.
    MovedVoterFleet moved { Annex };
    auto link = Testing::Unwrap(SchedulerLink::Over(moved.schedulers));

    CHECK(AnnouncePresenceOnce(moved, link));
    CHECK(Asked(moved.fleet) == std::vector<std::string> { std::string { LaptopOwnScheduler }, std::string { Annex } });
    CHECK(moved.fleet.AnnouncedEndpointsAt(Annex) == LaptopNoted());
    CHECK(moved.fleet.AnnouncedEndpointsAt(Office).empty());
}

TEST_CASE("A moved voter's worker registers with the leader through the voters its applied state records",
          "[node][fleet][announce][roaming]")
{
    // The worker's REGISTER rides the same link rules, through `AnnounceRound`: RED before the fix
    // for the presence case's reason -- its own scheduler answered `NotLeader` naming nobody.
    MovedVoterFleet moved { Office };
    moved.fix.cfg = moved.cfg;
    moved.fix.registrars.clear();
    moved.fix.registrars.push_back(Registrar("gcc-14", LaptopNow));
    auto link = Testing::Unwrap(SchedulerLink::Over(moved.schedulers));

    CHECK(AnnounceRound(moved.fix.Round(), link, moved.fleet) == 1);
    CHECK(Asked(moved.fleet)
          == std::vector<std::string> { std::string { LaptopOwnScheduler }, std::string { Annex }, std::string { Office } });
    auto const machines = moved.fleet.MachinesAt(Office);
    CHECK(std::ranges::any_of(machines, [](auto const& row) { return row.endpoint == LaptopNow; }));
}

TEST_CASE("A voter whose own scheduler knows the leader still registers through it alone",
          "[node][fleet][announce][roaming]")
{
    // The control: the list grew, and the ordinary case must not pay for it. A voter that did not
    // move hears its leader, so its own scheduler redirects and nothing else on the list is dialled.
    MovedVoterFleet moved { Office };
    moved.fleet.ElectLeader(Office); // the laptop's scheduler follows the office again
    auto link = Testing::Unwrap(SchedulerLink::Over(moved.schedulers));

    CHECK(AnnouncePresenceOnce(moved, link));
    CHECK(Asked(moved.fleet) == std::vector<std::string> { std::string { LaptopOwnScheduler }, std::string { Office } });
    CHECK(moved.fleet.AnnouncedEndpointsAt(Office) == LaptopNoted());
}
