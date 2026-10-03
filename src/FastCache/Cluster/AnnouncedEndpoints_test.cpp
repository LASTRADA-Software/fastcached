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
