// SPDX-License-Identifier: Apache-2.0
#include "ClusterAdminCli.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A caller the fleet has admitted.
constexpr Distributed::CallerContext Insider { .membership = Distributed::Membership::Member, .peerId = "peer-1" };

/// A cluster whose answers a test scripts.
///
/// The seam is what makes this file possible at all: the verbs it exercises are
/// decided by a leader with a durable log and two threads behind it, and none of
/// that is needed to check that a request reaches the cluster as the command it
/// names.
struct FakeCluster final: public Distributed::IClusterAdmin
{
    Cluster::ClusterState state;
    std::vector<Cluster::Command> proposed;

    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        return state;
    }

    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
    {
        proposed.push_back(command);
        return {};
    }
};

/// A leading scheduler, plus the protocol in front of it.
struct Fixture
{
    Fixture()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {});
    }

    ManualClock clock;
    AtomicMetricsSink metrics;
    Distributed::SchedulerService service { clock, metrics };
    Distributed::SchedulerProtocol protocol { service };

    /// Frame `request`, answer it, and hand back the reply bytes.
    /// @param request What the operator asked for.
    /// @param caller Who is asking.
    /// @return The reply.
    [[nodiscard]] std::vector<std::byte> Ask(ClusterRequest const& request,
                                             Distributed::CallerContext const& caller = Insider)
    {
        return protocol.Answer(EncodeClusterRequest(request), caller);
    }
};

/// The status byte of a reply, or nullopt when the reply is unreadable.
[[nodiscard]] std::optional<Wire::Status> StatusOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    return header.has_value() ? std::optional { header->status } : std::nullopt;
}

/// The error code of a refusal, or nullopt when the reply is not one.
[[nodiscard]] std::optional<Wire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error || header->payloadLength == 0)
        return std::nullopt;
    return static_cast<Wire::ErrorCode>(reply[Wire::ReplyHeaderSize]);
}

/// The message of a refusal.
[[nodiscard]] std::string MessageOf(std::span<std::byte const> reply)
{
    auto const payload = reply.subspan(Wire::ReplyHeaderSize);
    if (payload.empty())
        return {};
    return std::string { Wire::AsStringView(payload.subspan(1)) };
}

/// The payload of a reply, as bytes.
[[nodiscard]] std::span<std::byte const> PayloadOf(std::span<std::byte const> reply)
{
    return reply.subspan(Wire::ReplyHeaderSize);
}

/// Parse a command line the way `main` does.
/// @param args The tokens after the program name.
/// @return The configuration.
[[nodiscard]] NodeConfig ParsedFrom(std::vector<char const*> const& args)
{
    NodeConfig cfg;
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg);
    REQUIRE(flow.has_value());
    return cfg;
}

/// A command, spelled once so a field added to `Command` lands in one place.
/// @param kind What it does.
/// @param key The member id or setting name.
/// @param value The consensus endpoint or setting value.
/// @param scheduler Where clients reach this member while it leads.
/// @return The command.
[[nodiscard]] Cluster::Command Cmd(Cluster::CommandKind kind,
                                   std::string key,
                                   std::string value = {},
                                   std::string scheduler = {})
{
    return Cluster::Command { .kind = kind,
                              .key = std::move(key),
                              .value = std::move(value),
                              .schedulerEndpoint = std::move(scheduler) };
}

/// A cluster-administration request, spelled once so a field added to
/// `ClusterRequest` lands in one place rather than in every case.
/// @param action What to do.
/// @param key The setting name or member id.
/// @param value The setting's new value.
/// @return The request.
[[nodiscard]] ClusterRequest Ask(ClusterAction action, std::string key = {}, std::string value = {})
{
    return ClusterRequest { .action = action, .key = std::move(key), .value = std::move(value) };
}

/// A cluster that has agreed something, for the rendering cases.
/// @return The state.
[[nodiscard]] Cluster::ClusterState Agreed()
{
    Cluster::ClusterState state;
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "n1", "10.0.0.1:6680", "10.0.0.1:6675"));
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "n2", "10.0.0.2:6680"));
    Apply(state, Cmd(Cluster::CommandKind::SetSetting, "upstream", "cache.internal:6674"));
    return state;
}
} // namespace

TEST_CASE("A setting assignment splits at the first separator", "[node][clusteradmin]")
{
    // The same rule `ParsePeerSpec` applies and for the same reason: a value may
    // contain an `=` and a name may not. Splitting at the last one would read
    // `upstream=cache=1:6674` as a setting called `upstream=cache`, which is then
    // refused as unknown while naming something the operator never typed.
    auto const pair = ParseSettingAssignment("upstream=cache.internal:6674");
    REQUIRE(pair.has_value());
    CHECK(Unwrap(pair).first == "upstream");
    CHECK(Unwrap(pair).second == "cache.internal:6674");

    auto const withEquals = ParseSettingAssignment("upstream=host=1:6674");
    REQUIRE(withEquals.has_value());
    CHECK(Unwrap(withEquals).first == "upstream");
    CHECK(Unwrap(withEquals).second == "host=1:6674");

    // An empty VALUE is legitimate -- clearing a setting is a real change -- while
    // an empty name names nothing.
    auto const cleared = ParseSettingAssignment("upstream=");
    REQUIRE(cleared.has_value());
    CHECK(Unwrap(cleared).second.empty());

    CHECK_FALSE(ParseSettingAssignment("upstream").has_value());
    CHECK_FALSE(ParseSettingAssignment("=value").has_value());
    CHECK_FALSE(ParseSettingAssignment("").has_value());
}

TEST_CASE("Each cluster flag selects its action and carries its operand", "[node][clusteradmin]")
{
    // One row sets both, because a row that set only one would leave the other half
    // to a second row nobody would remember to add.
    CHECK(ParsedFrom({ "--cluster-status" }).cluster.action == ClusterAction::Status);

    auto const set = ParsedFrom({ "--cluster-set=upstream=cache:6674" });
    CHECK(set.cluster.action == ClusterAction::Set);
    CHECK(set.cluster.key == "upstream");
    CHECK(set.cluster.value == "cache:6674");

    auto const forget = ParsedFrom({ "--cluster-forget=n3" });
    CHECK(forget.cluster.action == ClusterAction::Forget);
    CHECK(forget.cluster.key == "n3");

    // And the default is to serve, which is what a worker starting up does.
    CHECK(ParsedFrom({ "--toolchain=/usr/bin/g++" }).cluster.action == ClusterAction::None);
}

TEST_CASE("A malformed cluster assignment is refused at the command line", "[node][clusteradmin]")
{
    // Refused where the operator is watching rather than sent to a leader that would
    // refuse it as an unknown setting -- a message that names the wrong problem.
    NodeConfig cfg;
    std::vector<char const*> const bad { "--cluster-set=upstream" };
    CHECK_FALSE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { bad }, cfg).has_value());

    NodeConfig other;
    std::vector<char const*> const empty { "--cluster-forget=" };
    CHECK_FALSE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { empty }, other).has_value());
}

TEST_CASE("A status request round-trips through the real protocol", "[node][clusteradmin]")
{
    // End to end with no socket: the client frames it, the scheduler's own router
    // decodes it, and the client reads back what came out. That is the exchange the
    // operator makes, minus the network.
    Fixture fixture;
    FakeCluster cluster;
    cluster.state = Agreed();
    fixture.service.AdministerWith(cluster);

    auto const reply = fixture.Ask(Ask(ClusterAction::Status));
    REQUIRE(StatusOf(reply) == Wire::Status::Ok);

    auto const rendered = InterpretClusterReply(ClusterAction::Status, PayloadOf(reply));
    REQUIRE(rendered.has_value());

    // The report answers the operator's real question, which is usually "what CAN I
    // set" -- so every key this build knows appears whether or not somebody has set
    // it, and a member that has never led shows a dash rather than a blank.
    CHECK(rendered->contains("n1"));
    CHECK(rendered->contains("10.0.0.1:6675"));
    CHECK(rendered->contains("scheduler=-"));
    CHECK(rendered->contains("cache.internal:6674"));
    CHECK(rendered->contains("fleet-open"));
}

TEST_CASE("A change reaches the cluster as the command it names", "[node][clusteradmin]")
{
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);

    CHECK(StatusOf(fixture.Ask(Ask(ClusterAction::Set, "upstream", "cache.internal:6674")))
          == Wire::Status::Ok);
    CHECK(StatusOf(fixture.Ask(Ask(ClusterAction::Forget, "n3"))) == Wire::Status::Ok);

    REQUIRE(cluster.proposed.size() == 2);
    CHECK(cluster.proposed[0] == Cmd(Cluster::CommandKind::SetSetting, "upstream", "cache.internal:6674"));
    CHECK(cluster.proposed[1] == Cmd(Cluster::CommandKind::RemoveMember, "n3"));
}

TEST_CASE("A node with no cluster says so rather than pretending", "[node][clusteradmin]")
{
    // The state of a single node started without `--node-id`: it leads itself and
    // has no replicated state for anybody to change. Distinct from `NotLeader`,
    // which names somewhere else to ask -- here the question does not apply at all,
    // and an operator sent elsewhere would go looking for a node that does not exist.
    Fixture fixture;

    for (auto const& request: std::array { Ask(ClusterAction::Status),
                                           Ask(ClusterAction::Set, "upstream", "x"),
                                           Ask(ClusterAction::Forget, "n3") })
    {
        auto const reply = fixture.Ask(request);
        CHECK(StatusOf(reply) == Wire::Status::Error);
        CHECK(ErrorOf(reply) == Wire::ErrorCode::NoCluster);
    }
}

TEST_CASE("A follower is refused and told who to ask", "[node][clusteradmin]")
{
    // The same gate the dispatch verbs go through, and it applies to the READ too.
    // A follower's copy is perfectly valid and merely older, so this could have
    // answered -- one rule for the whole surface is what makes "a verb added without
    // the gate" impossible, and the operator is sent to the node they would need
    // anyway to change anything.
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);
    fixture.service.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:6675");

    auto const reply = fixture.Ask(Ask(ClusterAction::Status));
    CHECK(StatusOf(reply) == Wire::Status::Error);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::NotLeader);
    CHECK(MessageOf(reply) == "10.0.0.9:6675");
}

TEST_CASE("A non-member may not change what the fleet believes", "[node][clusteradmin]")
{
    // Anti-leeching reaches this surface too, and here it is not about capacity: a
    // stranger who could set `upstream` would point the whole fleet's cache at a host
    // of their choosing.
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);

    constexpr Distributed::CallerContext Stranger { .membership = Distributed::Membership::Outsider,
                                                    .peerId = "stranger" };

    auto const reply =
        fixture.Ask(Ask(ClusterAction::Set, "upstream", "x"), Stranger);
    CHECK(StatusOf(reply) == Wire::Status::Error);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::NotAMember);
    CHECK(cluster.proposed.empty());
}

TEST_CASE("A change the cluster would refuse is refused with its reason", "[node][clusteradmin]")
{
    // Refused at the PROPOSER, which is the only place a change can be refused: an
    // entry is applied after it is committed, when there is nobody left to report to.
    // The reason travels as the message because it is read by a person -- "no such
    // cluster setting: upsteam" is actionable and a numeric code is not.
    struct RefusingCluster final: public Distributed::IClusterAdmin
    {
        [[nodiscard]] Cluster::ClusterState ClusterState() const override
        {
            return {};
        }

        [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
        {
            return Cluster::Validate(command);
        }
    } cluster;

    Fixture fixture;
    fixture.service.AdministerWith(cluster);

    auto const reply = fixture.Ask(Ask(ClusterAction::Set, "upsteam", "x"));
    CHECK(StatusOf(reply) == Wire::Status::Error);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(MessageOf(reply).contains("upsteam"));
}

TEST_CASE("A status request carrying anything is malformed", "[node][clusteradmin]")
{
    // The table says this verb carries no fields, so an empty payload is the only one
    // that decodes. A request with something in it is a client this build does not
    // understand, and answering it would be guessing at what they meant.
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);

    // A CLUSTER-FORGET payload behind a CLUSTER-STATUS header: well-formed bytes that
    // this verb has no reading for.
    auto frame = Wire::EncodeClusterForget("n3");
    frame[2] = static_cast<std::byte>(Wire::Op::ClusterStatus);

    auto const reply = fixture.protocol.Answer(frame, Insider);
    CHECK(StatusOf(reply) == Wire::Status::Error);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::MalformedFrame);
}
