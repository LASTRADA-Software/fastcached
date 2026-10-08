// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/AnnouncedEndpoints.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;

namespace
{
/// A state recording the laptop as a learner whose `0xFC` port answers at @p endpoint.
/// @param endpoint Its recorded endpoint.
/// @return The state.
[[nodiscard]] ClusterState WithLearner(std::string endpoint)
{
    auto state = ClusterState {};
    state.members.push_back(ClusterMember { .id = "laptop",
                                            .raftEndpoint = "",
                                            .schedulerEndpoint = std::move(endpoint),
                                            .schedulerEndpointHistory = SchedulerEndpointHistory::Announced,
                                            .seat = MemberSeat::Learner,
                                            .publicKey = Testing::TestKeyPair("laptop").PublicKey() });
    return state;
}
} // namespace

TEST_CASE("A proven announcement that moved a member's endpoint re-proposes its record and keeps its key",
          "[cluster][formation][endpoint]")
{
    auto const desires = AnnouncedEndpointDesires(WithLearner("laptop:6674"), { { "laptop", "10.9.0.4:6674" } }, {});
    REQUIRE(desires.size() == 1);
    CHECK(desires[0].id == "laptop");
    CHECK(desires[0].raftEndpoint.empty()); // the recorded one: a learner dials in
    CHECK(desires[0].schedulerEndpoint == std::optional<std::string> { "10.9.0.4:6674" });
    CHECK_FALSE(desires[0].publicKey.has_value()); // keep the recorded key
}

TEST_CASE("An unchanged endpoint an unknown id a malformed endpoint and a change in flight propose nothing",
          "[cluster][formation][endpoint]")
{
    CHECK(AnnouncedEndpointDesires(WithLearner("laptop:6674"), { { "laptop", "laptop:6674" } }, {}).empty());
    CHECK(AnnouncedEndpointDesires(WithLearner("laptop:6674"), { { "stranger", "10.9.0.5:6674" } }, {}).empty());
    CHECK(AnnouncedEndpointDesires(WithLearner("laptop:6674"), { { "laptop", ":6674" } }, {}).empty());
    // A proven member announcing an endpoint only it reaches is not recorded at it: whoever dialled
    // it would reach ITSELF (`IsPeerDialableEndpoint`, the rule `Validate` holds the record to).
    for (auto const* const only: { "127.0.0.1:6674", "localhost:6674", "0.0.0.0:6674" })
    {
        INFO(only);
        CHECK(AnnouncedEndpointDesires(WithLearner("laptop:6674"), { { "laptop", only } }, {}).empty());
    }
    CHECK(AnnouncedEndpointDesires(WithLearner("laptop:6674"), { { "laptop", "10.9.0.4:6674" } }, { "laptop" }).empty());
}

TEST_CASE("An announcement folds into a desire with no opinion and never outranks one that asserts",
          "[cluster][formation][endpoint]")
{
    auto const announced = std::vector {
        DesiredMember {
            .id = "desk", .raftEndpoint = "desk:6680", .schedulerEndpoint = "10.9.0.8:6674", .publicKey = std::nullopt },
        DesiredMember {
            .id = "office", .raftEndpoint = "office:6680", .schedulerEndpoint = "10.9.0.9:6674", .publicKey = std::nullopt },
        DesiredMember { .id = "laptop", .raftEndpoint = "", .schedulerEndpoint = "10.9.0.4:6674", .publicKey = std::nullopt }
    };
    // The office desires itself and says where it answers; discovery desires the desk with no
    // opinion about its `0xFC` port; nothing desires the laptop.
    auto const desired = std::vector {
        DesiredMember {
            .id = "office", .raftEndpoint = "office:6680", .schedulerEndpoint = "office:6674", .publicKey = std::nullopt },
        DesiredMember {
            .id = "desk", .raftEndpoint = "desk:6680", .schedulerEndpoint = std::nullopt, .publicKey = std::nullopt }
    };

    auto const merged = WithAnnouncedEndpoints(desired, announced);
    REQUIRE(merged.size() == 3); // one desire per member: two records for one id would undo each other
    CHECK(merged[0].schedulerEndpoint == std::optional<std::string> { "office:6674" }); // its own word wins
    CHECK(merged[1].schedulerEndpoint == std::optional<std::string> { "10.9.0.8:6674" });
    CHECK(merged[2].id == "laptop");
    CHECK(merged[2].schedulerEndpoint == std::optional<std::string> { "10.9.0.4:6674" });
}

namespace
{
/// A state recording one voter, `desk`, at the two endpoints given.
/// @param raftEndpoint Where its consensus port answers.
/// @param schedulerEndpoint Where its `0xFC` port answers.
/// @return The state.
[[nodiscard]] ClusterState WithVoter(std::string raftEndpoint, std::string schedulerEndpoint)
{
    auto state = ClusterState {};
    state.members.push_back(ClusterMember { .id = "desk",
                                            .raftEndpoint = std::move(raftEndpoint),
                                            .schedulerEndpoint = std::move(schedulerEndpoint),
                                            .schedulerEndpointHistory = SchedulerEndpointHistory::Announced,
                                            .seat = MemberSeat::Voter,
                                            .publicKey = Testing::TestKeyPair("desk").PublicKey() });
    return state;
}

/// The one desire `desk`'s announcement of @p announced produces over @p state.
/// @param state The state recording it.
/// @param announced What it announced.
/// @return The desire.
[[nodiscard]] DesiredMember DeskDesire(ClusterState const& state, std::string announced)
{
    auto const desires = AnnouncedEndpointDesires(state, { { "desk", std::move(announced) } }, {});
    REQUIRE(desires.size() == 1);
    return desires.front();
}
} // namespace

TEST_CASE("An announced move of a member whose Raft and scheduler hosts match moves its Raft endpoint too",
          "[cluster][formation][endpoint]")
{
    auto const desire = DeskDesire(WithVoter("10.0.0.5:6680", "10.0.0.5:6674"), "192.168.7.2:6674");
    CHECK(desire.raftEndpoint == "192.168.7.2:6680"); // the announced host, on the recorded Raft port
    CHECK(desire.schedulerEndpoint == std::optional<std::string> { "192.168.7.2:6674" });
    CHECK_FALSE(desire.publicKey.has_value());
}

TEST_CASE("An announced move of an IPv6 member keeps the brackets on its moved Raft endpoint",
          "[cluster][formation][endpoint]")
{
    auto const desire = DeskDesire(WithVoter("[fd00::5]:6680", "[fd00::5]:6674"), "[fd00::9]:6674");
    CHECK(desire.raftEndpoint == "[fd00::9]:6680");
}

TEST_CASE("A member whose Raft host differs from its scheduler host keeps its Raft endpoint",
          "[cluster][formation][endpoint]")
{
    // Pinned apart: the node said where each answers, and an announcement speaks for the `0xFC` one.
    auto const desire = DeskDesire(WithVoter("peer.lan:6680", "10.0.0.5:6674"), "192.168.7.2:6674");
    CHECK(desire.raftEndpoint == "peer.lan:6680");
    CHECK_FALSE(SpeaksForRaftEndpoint(desire));
}

TEST_CASE("A record no dial endpoint parses from is never coupled", "[cluster][formation][endpoint]")
{
    auto const member = [](std::string raft, std::string scheduler) {
        return ClusterMember { .id = "desk",
                               .raftEndpoint = std::move(raft),
                               .schedulerEndpoint = std::move(scheduler),
                               .schedulerEndpointHistory = SchedulerEndpointHistory::Announced,
                               .seat = MemberSeat::Voter,
                               .publicKey = Testing::TestKeyPair("desk").PublicKey() };
    };
    CHECK(CoupledRaftEndpoint(member("", "10.0.0.5:6674"), "192.168.7.2:6674").empty()); // a learner's
    CHECK(CoupledRaftEndpoint(member("10.0.0.5:6680", ""), "192.168.7.2:6674") == "10.0.0.5:6680");
    CHECK(CoupledRaftEndpoint(member("10.0.0.5", "10.0.0.5:6674"), "192.168.7.2:6674") == "10.0.0.5");
    CHECK(CoupledRaftEndpoint(member("10.0.0.5:6680", "10.0.0.5:6674"), "6674") == "10.0.0.5:6680");
    // The IPv4-mapped spelling of one machine is that machine.
    CHECK(CoupledRaftEndpoint(member("[::ffff:10.0.0.5]:6680", "10.0.0.5:6674"), "192.168.7.2:6674") == "192.168.7.2:6680");
}

TEST_CASE("A coupled announcement moves the Raft endpoint of a desire with no scheduler opinion, held and folded",
          "[cluster][formation][endpoint]")
{
    // Discovery desires the desk at the Raft endpoint a beacon stated before the move; the desk's own
    // proven announcement says it moved.
    auto const announcements =
        AnnouncedEndpointDesires(WithVoter("10.0.0.5:6680", "10.0.0.5:6674"), { { "desk", "192.168.7.2:6674" } }, {});
    auto const discovered = std::vector { DesiredMember {
        .id = "desk", .raftEndpoint = "10.0.0.5:6680", .schedulerEndpoint = std::nullopt, .publicKey = std::nullopt } };

    auto const folded = WithAnnouncedEndpoints(discovered, announcements);
    REQUIRE(folded.size() == 1);
    CHECK(folded[0].raftEndpoint == "192.168.7.2:6680");
    CHECK(folded[0].schedulerEndpoint == std::optional<std::string> { "192.168.7.2:6674" });

    // What the leader keeps: the newer Raft endpoint, and still no `0xFC` opinion, so the next
    // announcement folds in as this one did.
    auto const held = WithAnnouncedRaftEndpoints(discovered, announcements);
    REQUIRE(held.size() == 1);
    CHECK(held[0].raftEndpoint == "192.168.7.2:6680");
    CHECK_FALSE(held[0].schedulerEndpoint.has_value());

    // A desire that asserts its own endpoints is never moved by an announcement about it.
    auto const asserted = std::vector { DesiredMember {
        .id = "desk", .raftEndpoint = "10.0.0.5:6680", .schedulerEndpoint = "10.0.0.5:6674", .publicKey = std::nullopt } };
    CHECK(WithAnnouncedEndpoints(asserted, announcements)[0].raftEndpoint == "10.0.0.5:6680");
    CHECK(WithAnnouncedRaftEndpoints(asserted, announcements)[0].raftEndpoint == "10.0.0.5:6680");
}

TEST_CASE("An uncoupled announcement leaves a held desire's Raft endpoint alone", "[cluster][formation][endpoint]")
{
    auto const announcements =
        AnnouncedEndpointDesires(WithVoter("peer.lan:6680", "10.0.0.5:6674"), { { "desk", "192.168.7.2:6674" } }, {});
    auto const discovered = std::vector { DesiredMember {
        .id = "desk", .raftEndpoint = "peer2.lan:6680", .schedulerEndpoint = std::nullopt, .publicKey = std::nullopt } };
    CHECK(WithAnnouncedRaftEndpoints(discovered, announcements)[0].raftEndpoint == "peer2.lan:6680");
    CHECK(WithAnnouncedEndpoints(discovered, announcements)[0].raftEndpoint == "peer2.lan:6680");
}

TEST_CASE("A learner's empty Raft endpoint stays empty", "[cluster][formation][endpoint]")
{
    // A learner dials in: it has no Raft endpoint for an announcement to move.
    auto const desires = AnnouncedEndpointDesires(WithLearner("10.9.0.4:6674"), { { "laptop", "10.9.0.7:6674" } }, {});
    REQUIRE(desires.size() == 1);
    CHECK(desires[0].raftEndpoint.empty());
    CHECK_FALSE(SpeaksForRaftEndpoint(desires[0]));
}
