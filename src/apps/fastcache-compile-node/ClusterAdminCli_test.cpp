// SPDX-License-Identifier: Apache-2.0
#include "ClusterAdminCli.hpp"
#include "EndpointDialerTestUtils.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ErrorOf;
using FastCache::Testing::PayloadOf;
using FastCache::Testing::StatusOf;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A caller the fleet has admitted.
Distributed::CallerContext const Insider { .membership = Distributed::Membership::Member, .peerId = "peer-1" };

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
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    }

    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    core::platform::ManualWallClock wallClock;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, schedulerLogger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };

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

/// The message of a refusal.
[[nodiscard]] std::string MessageOf(std::span<std::byte const> reply)
{
    auto const payload = reply.subspan(Wire::ReplyHeaderSize);
    if (payload.empty())
        return {};
    return std::string { Wire::AsStringView(payload.subspan(1)) };
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
                              .schedulerEndpoint = std::move(scheduler),
                              .publicKey = std::nullopt,
                              .role = std::nullopt };
}

/// A cluster-administration request, spelled once so a field added to
/// `ClusterRequest` lands in one place rather than in every case.
/// @param action What to do.
/// @param key The setting name or member id.
/// @param value The setting's new value.
/// @param publicKey The member's identity key, for an admission that states one.
/// @return The request.
[[nodiscard]] ClusterRequest Ask(ClusterAction action,
                                 std::string key = {},
                                 std::string value = {},
                                 std::optional<Ed25519PublicKey> publicKey = std::nullopt)
{
    return ClusterRequest { .action = action, .key = std::move(key), .value = std::move(value), .publicKey = publicKey };
}

/// A cluster that has agreed something, for the rendering cases.
/// @return The state.
[[nodiscard]] Cluster::ClusterState Agreed()
{
    Cluster::ClusterState state;
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "n1", "10.0.0.1:6680", "10.0.0.1:6675"));
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "n2", "10.0.0.2:6680"));
    Apply(state, Cmd(Cluster::CommandKind::SetSetting, "lease-lifetime", "20min"));
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

    auto const set = ParsedFrom({ "--cluster-set=lease-lifetime=20min" });
    CHECK(set.cluster.action == ClusterAction::Set);
    CHECK(set.cluster.key == "lease-lifetime");
    CHECK(set.cluster.value == "20min");

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
    std::vector<char const*> const bad { "--cluster-set=lease-lifetime" };
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
    CHECK(rendered->contains("20min"));
    CHECK(rendered->contains("fleet-open"));
}

TEST_CASE("A status report says whether an absent scheduler endpoint was never announced or cleared", "[node][clusteradmin]")
{
    // #1340. Both members carry no scheduler endpoint, and one of them HAD one until a
    // re-admit applied wholesale. Read back through the reply body, because that is how
    // the report acquires the state: a history the encoder dropped would render both
    // alike from here and from nowhere else.
    Cluster::ClusterState state;
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "quiet", "10.0.0.1:6680"));
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "moved", "10.0.0.2:6680", "10.0.0.2:6675"));
    Apply(state, Cmd(Cluster::CommandKind::AddMember, "moved", "10.0.0.2:6680"));

    auto const rendered = InterpretClusterReply(ClusterAction::Status, Cluster::Encode(state));
    REQUIRE(rendered.has_value());

    auto const lineOf = [&rendered](std::string_view id) {
        auto const at = rendered->find(std::string { "  " } + std::string { id } + " ");
        REQUIRE(at != std::string::npos);
        return rendered->substr(at, rendered->find('\n', at) - at);
    };
    auto const quiet = lineOf("quiet");
    auto const moved = lineOf("moved");
    INFO(*rendered);

    // What distinguishes, on each side, and the dash both still carry.
    CHECK(quiet.contains("scheduler=- (never-announced)"));
    CHECK(moved.contains("scheduler=- (cleared)"));
    CHECK(quiet.substr(quiet.find("scheduler=")) != moved.substr(moved.find("scheduler=")));
}

TEST_CASE("A status reply another build encoded is refused by its version", "[node][clusteradmin]")
{
    // Not *a format this build cannot read* and nothing more: that sentence fits a
    // damaged body as well, and the two send an operator to different machines.
    auto body = Cluster::Encode(Agreed());
    // The state's version is the first field's only byte, after its u32 length prefix.
    REQUIRE(body.size() > 4);
    body[4] = std::byte { 2 };

    auto const refused = InterpretClusterReply(ClusterAction::Status, body);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("cannot read"));
    CHECK(refused.error().contains("version 2"));
}

TEST_CASE("A change reaches the cluster as the command it names", "[node][clusteradmin]")
{
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);

    CHECK(StatusOf(fixture.Ask(Ask(ClusterAction::Set, "lease-lifetime", "20min"))) == Wire::Status::Ok);
    CHECK(StatusOf(fixture.Ask(Ask(ClusterAction::Forget, "n3"))) == Wire::Status::Ok);

    REQUIRE(cluster.proposed.size() == 2);
    CHECK(cluster.proposed[0] == Cmd(Cluster::CommandKind::SetSetting, "lease-lifetime", "20min"));
    CHECK(cluster.proposed[1] == Cmd(Cluster::CommandKind::Forget, "n3"));
}

TEST_CASE("A key this cluster refuses never reaches the log", "[node][clusteradmin]")
{
    // #1123 at the door an operator types through. Two things, and the second is the
    // one a message assertion cannot give: the refusal names the flag that does the
    // job, and NOTHING was proposed -- refused on the leader before the append, which
    // is the whole reason a key nobody can act on is refused here rather than ignored
    // by each applier.
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);

    auto const refused = fixture.Ask(Ask(ClusterAction::Set, "upstream", "cache.internal:6674"));
    CHECK(ErrorOf(refused) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(MessageOf(refused).contains("--upstream"));
    CHECK(MessageOf(refused).contains("--requirepass"));
    CHECK(cluster.proposed.empty());
}

TEST_CASE("The operator's door refuses a member it could not name, and forgets one anyway", "[node][clusteradmin]")
{
    // #159 at the door an operator types through, which is the one where a refusal is
    // safe: `--cluster-admit` and `--cluster-set` are one-shot and somebody is
    // reading the answer. The reconciler is the door where the same refusal is a
    // trap, because it would retry the identical command every interval forever.
    Fixture fixture;
    FakeCluster cluster;
    fixture.service.AdministerWith(cluster);

    auto const admit = fixture.Ask(Ask(ClusterAction::Admit, "n\x80", "10.0.0.4:6680"));
    CHECK(ErrorOf(admit) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(MessageOf(admit).contains("a member id"));

    // A second verb, because what this case is about is the DOOR: which field of
    // which command is at fault is `ClusterState_test`'s question, and it asks it
    // over every field there.
    auto const set = fixture.Ask(Ask(ClusterAction::Set, "upstream", "cache.internal:6674\xE2\x82"));
    CHECK(ErrorOf(set) == Wire::ErrorCode::InvalidClusterChange);

    // Refused before the cluster was asked, not by it: the whole point of shutting
    // this door is that no such entry ever reaches a log the fleet replicates.
    CHECK(cluster.proposed.empty());

    // And the removal goes through, which is what keeps the refusal from being a
    // trap. A member that reached replicated state through a peer built before any
    // of this existed is exactly the one an operator has to be able to name, and its
    // id is the offending value itself.
    CHECK(StatusOf(fixture.Ask(Ask(ClusterAction::Forget, "n\x80"))) == Wire::Status::Ok);
    REQUIRE(cluster.proposed.size() == 1);
    CHECK(cluster.proposed[0] == Cmd(Cluster::CommandKind::Forget, "n\x80"));
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
                                           Ask(ClusterAction::Forget, "n3"),
                                           Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680") })
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
    fixture.service.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:6675", Distributed::StandaloneSchedulerTerm);

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

    Distributed::CallerContext const Stranger { .membership = Distributed::Membership::Outsider, .peerId = "stranger" };

    auto const reply = fixture.Ask(Ask(ClusterAction::Set, "upstream", "x"), Stranger);
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

TEST_CASE("A member can be admitted, which is what --cluster-forget had no counterpart for", "[node][clusteradmin]")
{
    // Nothing anywhere could put a member INTO the replicated state without
    // `--discovery`, so a fleet with a typed peer list could shrink and never grow:
    // adding a machine meant editing `--raft-peer` on every other machine and
    // restarting them, which is the cost this whole change exists to remove.
    FakeCluster cluster;
    Fixture fixture;
    fixture.service.AdministerWith(cluster);

    auto const reply = fixture.Ask(Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680"));
    CHECK(StatusOf(reply) == Wire::Status::Ok);

    REQUIRE(cluster.proposed.size() == 1);
    CHECK(cluster.proposed[0] == Cmd(Cluster::CommandKind::AddMember, "n4", "10.0.0.4:6680"));

    // The scheduler endpoint is deliberately empty: a member announces its own once
    // elected, and a value typed here about somebody else would be a guess that
    // outranks what they say about themselves.
    CHECK(cluster.proposed[0].schedulerEndpoint.empty());
}

TEST_CASE("Admitting takes the same token --raft-peer does", "[node][clusteradmin]")
{
    // One grammar for "this member, at this address", because a second spelling of
    // one thing is a second thing to get wrong -- and the operator has already
    // typed this one into `--raft-peer` on the machine being added.
    auto const cfg = ParsedFrom({ "--cluster-admit=n4=10.0.0.4:6680" });
    CHECK(cfg.cluster.action == ClusterAction::Admit);
    CHECK(cfg.cluster.key == "n4");
    CHECK(cfg.cluster.value == "10.0.0.4:6680");
}

TEST_CASE("An admission with no dialable address is refused where it is typed", "[node][clusteradmin]")
{
    // An id with no address a peer can dial is a node the cluster counts towards
    // quorum and never reaches, and the command line is the one place an operator
    // is watching. Both shapes: nothing after the id, and something that is not a
    // host and a port.
    for (auto const* const spec: { "--cluster-admit=n4", "--cluster-admit=n4=", "--cluster-admit=n4=nowhere" })
    {
        INFO("spec: " << spec);
        NodeConfig cfg;
        std::vector<char const*> const args { spec };
        CHECK_FALSE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg).has_value());
    }
}

TEST_CASE("An admission prints back the id and the endpoint the leader recorded", "[node][clusteradmin]")
{
    // #1296, end to end through the real service and the real protocol rather than
    // against a hand-built payload: what this case is for is that the three halves
    // AGREE -- the service builds a receipt from the command it proposed, the
    // protocol carries it, and this renderer reads it. A payload written here by hand
    // would pass with any two of the three wired to each other.
    FakeCluster cluster;
    Fixture fixture;
    fixture.service.AdministerWith(cluster);

    auto const reply = fixture.Ask(Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680"));
    REQUIRE(StatusOf(reply) == Wire::Status::Ok);

    auto const rendered = InterpretClusterReply(ClusterAction::Admit, PayloadOf(reply));
    REQUIRE(rendered.has_value());

    // Against what reached CONSENSUS, not only against the literals this case typed:
    // a renderer printing a constant, or the wrong field, passes a literal check.
    REQUIRE(cluster.proposed.size() == 1);
    CHECK(rendered->contains(cluster.proposed.front().key));
    CHECK(rendered->contains(cluster.proposed.front().value));
    CHECK(rendered->contains("10.0.0.4:6680"));
}

TEST_CASE("An admission carries the member's key from the command line and prints back the key the leader recorded",
          "[node][clusteradmin][identity]")
{
    // #178, from argv to the rendered receipt through the real service and protocol, for
    // #1296's reason: the halves must AGREE, and a hand-built payload passes with any two of
    // them wired to each other. Both directions, because a renderer that always prints a key,
    // or never does, passes one of them.
    auto key = Ed25519PublicKey {};
    key.fill(std::byte { 0x6E });
    auto const keyText = FormatEd25519PublicKey(key);

    SECTION("a key typed after @ is recorded and read back")
    {
        FakeCluster cluster;
        Fixture fixture;
        fixture.service.AdministerWith(cluster);

        auto const flag = std::format("--cluster-admit=n4=10.0.0.4:6680@{}", keyText);
        auto const cfg = ParsedFrom({ flag.c_str() });
        auto const reply = fixture.Ask(cfg.cluster);
        REQUIRE(StatusOf(reply) == Wire::Status::Ok);
        REQUIRE(cluster.proposed.size() == 1);
        CHECK(cluster.proposed.front().publicKey == std::optional { key });

        auto const rendered = InterpretClusterReply(ClusterAction::Admit, PayloadOf(reply));
        REQUIRE(rendered.has_value());
        CHECK(rendered->contains("identity key"));
        CHECK(rendered->contains(keyText));
        CHECK_FALSE(rendered->contains("none stated"));
    }

    SECTION("no key is recorded as none, and says what none means")
    {
        FakeCluster cluster;
        Fixture fixture;
        fixture.service.AdministerWith(cluster);

        auto const reply = fixture.Ask(Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680"));
        REQUIRE(StatusOf(reply) == Wire::Status::Ok);
        REQUIRE(cluster.proposed.size() == 1);
        CHECK_FALSE(cluster.proposed.front().publicKey.has_value());

        auto const rendered = InterpretClusterReply(ClusterAction::Admit, PayloadOf(reply));
        REQUIRE(rendered.has_value());
        CHECK(rendered->contains("identity key"));
        CHECK(rendered->contains("none stated"));
        CHECK_FALSE(rendered->contains(keyText));
    }
}

TEST_CASE("An admission is reported as recorded and appended, never as in force", "[node][clusteradmin]")
{
    // **The wording half of #1296's acceptance, and it is a CEILING rather than a
    // vocabulary.** What the leader wrote down it knows instantly and alone; whether a
    // majority has taken it it cannot know at all. An echo that reads as the second
    // when it is the first is worse than the silence it replaces, because an operator
    // reads their own endpoint back and stops looking.
    FakeCluster cluster;
    Fixture fixture;
    fixture.service.AdministerWith(cluster);

    auto const reply = fixture.Ask(Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680"));
    auto const rendered = InterpretClusterReply(ClusterAction::Admit, PayloadOf(reply));
    REQUIRE(rendered.has_value());

    CHECK(rendered->contains("recorded"));
    CHECK(rendered->contains("as received"));
    // `SchedulerService::Offer`'s own phrase, not a second spelling of one state.
    CHECK(rendered->contains("Appended, not committed"));

    // The forbidden claims. Checked as ABSENCES, which is the half that discriminates:
    // every wrong renderer this clause exists to stop would still contain the endpoint,
    // so a positive check alone passes under all of them.
    CHECK_FALSE(rendered->contains("admitted"));
    CHECK_FALSE(rendered->contains("added"));
    CHECK_FALSE(rendered->contains("in force"));

    // *committed* needs its own treatment rather than a bare absence, because the
    // ceiling phrase CONTAINS it -- negated. A substring check would refuse the one
    // wording this clause demands, which is how a guard ends up asserting the opposite
    // of its rule. So the negated occurrence is removed first and what remains must
    // hold none.
    auto residue = *rendered;
    auto const negated = residue.find("not committed");
    REQUIRE(negated != std::string::npos);
    residue.erase(negated, std::string_view { "not committed" }.size());
    CHECK_FALSE(residue.contains("committed"));
}

TEST_CASE("An admission answered with a body this build cannot read is refused", "[node][clusteradmin]")
{
    // Not rendered as a receipt with blank fields, which is the missing string the
    // whole change exists to prevent arriving through the renderer -- an operator
    // comparing two blank columns finds them equal.
    //
    // And deliberately NOT reported as *an older leader*: `MinSupportedVersion` equals
    // `CurrentVersion`, so a leader speaking another version is refused by name at the
    // header and never reaches this arm at all.
    auto const empty = InterpretClusterReply(ClusterAction::Admit, {});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().contains("cannot read"));

    auto const shortBody = Wire::EncodeClusterAdmitReceipt(Wire::ClusterAdmitReceipt {});
    auto truncated = std::vector<std::byte> { shortBody.begin(), shortBody.end() };
    truncated.pop_back();
    CHECK_FALSE(InterpretClusterReply(ClusterAction::Admit, truncated).has_value());
}

TEST_CASE("The other two cluster verbs still report exactly as they did", "[node][clusteradmin]")
{
    // The receipt is `cluster-admit`'s alone, and this is what makes that observable
    // at the surface an operator reads. Both verbs in ONE case, and the admission
    // beside them, so "all three changed" and "none changed" are each red rather than
    // one of them passing quietly.
    auto const set = InterpretClusterReply(ClusterAction::Set, {});
    REQUIRE(set.has_value());
    CHECK(*set == "accepted; the change is replicating\n");

    auto const forget = InterpretClusterReply(ClusterAction::Forget, {});
    REQUIRE(forget.has_value());
    CHECK(*forget == "accepted; the change is replicating\n");

    // The same empty body that satisfies those two is refused for an admission.
    CHECK_FALSE(InterpretClusterReply(ClusterAction::Admit, {}).has_value());
}

TEST_CASE("A cluster command asks the next --scheduler when the first cannot be reached, and only then",
          "[node][clusteradmin][fallback]")
{
    // #1310. The reply is the real protocol's answer to a status request, so what is
    // asserted beyond the dials is that the SECOND endpoint's answer is what the operator
    // is shown.
    Fixture fixture;
    FakeCluster cluster;
    cluster.state = Agreed();
    fixture.service.AdministerWith(cluster);
    auto const answer = fixture.Ask(Ask(ClusterAction::Status));
    REQUIRE(StatusOf(answer) == Wire::Status::Ok);

    NodeConfig cfg;
    cfg.schedulers = { "sched-a.internal:6675", "sched-b.internal:6675" };
    ConfiguredCredential const credential { cfg, nullptr };

    SECTION("the first is unreachable: the second is asked and its answer rendered")
    {
        Testing::ScriptedDialer dialer { { {}, answer } };

        auto const rendered = RunClusterAdmin(cfg, Ask(ClusterAction::Status), credential, dialer);

        INFO("result: " << rendered.value_or(rendered.error_or("")));
        REQUIRE(rendered.has_value());
        CHECK(rendered->contains("10.0.0.1:6675"));
        CHECK(dialer.Dialed() == std::vector<std::string> { "sched-a.internal:6675", "sched-b.internal:6675" });
        CHECK(dialer.SentOn(0).empty());
        CHECK_FALSE(dialer.SentOn(1).empty());
    }

    SECTION("control: a first that answers is the only one asked")
    {
        Testing::ScriptedDialer dialer { { answer } };

        REQUIRE(RunClusterAdmin(cfg, Ask(ClusterAction::Status), credential, dialer).has_value());
        CHECK(dialer.Dialed() == std::vector<std::string> { "sched-a.internal:6675" });
    }

    SECTION("a first that connects and then fails is reported, never retried elsewhere")
    {
        // `--cluster-admit` may already have been proposed where it landed.
        Testing::ScriptedDialer dialer { { std::vector<std::byte> { std::byte { 0xFF } } } };

        auto const admitted = RunClusterAdmin(cfg, Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680"), credential, dialer);

        REQUIRE_FALSE(admitted.has_value());
        CHECK(dialer.Dialed() == std::vector<std::string> { "sched-a.internal:6675" });
    }

    SECTION("none reachable: the refusal names every scheduler it tried")
    {
        Testing::ScriptedDialer dialer { { {}, {} } };

        auto const refused = RunClusterAdmin(cfg, Ask(ClusterAction::Status), credential, dialer);

        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("sched-a.internal:6675, sched-b.internal:6675"));
    }
}

TEST_CASE("The two client flags select their action and carry a host", "[node][clusteradmin][forget]")
{
    // #1309. A CLIENT, so a bare host: it never joins consensus and has no id for
    // `--cluster-admit` to name.
    auto const admit = ParsedFrom({ "--cluster-admit-client=10.0.0.7" });
    CHECK(admit.cluster.action == ClusterAction::AdmitClient);
    CHECK(admit.cluster.key == "10.0.0.7");
    CHECK(admit.cluster.value.empty());

    auto const forget = ParsedFrom({ "--cluster-forget-client=ci-runner-3.example" });
    CHECK(forget.cluster.action == ClusterAction::ForgetClient);
    CHECK(forget.cluster.key == "ci-runner-3.example");

    // An endpoint is accepted and reaches the wire whole; `Cluster::Validate` is where
    // the port is dropped, so the flag is not a second place that decision is made.
    CHECK(ParsedFrom({ "--cluster-forget-client=10.0.0.7:6674" }).cluster.key == "10.0.0.7:6674");

    // Both refuse an empty operand, naming themselves rather than the flag beside them:
    // a parser that stamped one spelling for both would send an operator to the wrong
    // flag, and the two differ by six characters.
    for (auto const* const spelling: { "--cluster-admit-client=", "--cluster-forget-client=" })
    {
        NodeConfig cfg;
        std::vector<char const*> const argv { spelling };
        auto const parsed = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
        REQUIRE_FALSE(parsed.has_value());
        INFO("spelling " << spelling);
        // `field` names the flag and `context` carries the reason -- two fields, because
        // an operator needs both and a message that merges them can only be searched.
        // Asserting the NAME is what catches the copy-paste these two flags invite: they
        // differ by six characters, and a wrong stamp sends somebody to the other one.
        CHECK(parsed.error().field.contains(std::string_view { spelling }.substr(2, 20)));
        CHECK(parsed.error().context.contains("names no host"));
    }
}

TEST_CASE("A client ADMIT is text-gated and a client FORGET is deliberately not", "[node][clusteradmin][forget]")
{
    // **The assertion is the asymmetry**, and it is issue #159's trap one verb along.
    // An admit COMMITS a host every renderer of the state prints, so text that is not
    // UTF-8 is refused where the operator is watching. A forget's operand IS the
    // offending host -- so gating it would make a client recorded by a peer that did
    // not check it permanently unremovable, and it would go on being served forever.
    //
    // A test asserting only that both parse, or only that both refuse, passes under
    // either half being wrong. What distinguishes them is that one refuses this input
    // and the other takes it.
    auto const* const bad = "--cluster-admit-client=\xffhost";
    NodeConfig admitCfg;
    std::vector<char const*> const admitArgv { bad };
    CHECK_FALSE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { admitArgv }, admitCfg).has_value());

    NodeConfig forgetCfg;
    std::vector<char const*> const forgetArgv { "--cluster-forget-client=\xffhost" };
    auto const forgetParsed = ParseOptionsInto(NodeOptions(), std::span<char const* const> { forgetArgv }, forgetCfg);
    REQUIRE(forgetParsed.has_value());
    CHECK(forgetCfg.cluster.action == ClusterAction::ForgetClient);
    CHECK(forgetCfg.cluster.key == "\xffhost");
}

TEST_CASE("Each client verb encodes as its own op, over one encoder", "[node][clusteradmin][forget]")
{
    // The bytes, not the symbol: both ends spell `Op::ClusterAdmitClient`, so a test
    // comparing the enumerator to itself cannot see a value that moved. The op sits in
    // the request header, which `DecodeRequestHeader` reads back.
    auto const admit = EncodeClusterRequest(Ask(ClusterAction::AdmitClient, "10.0.0.7"));
    auto const admitHeader = Wire::DecodeRequestHeader(admit);
    REQUIRE(admitHeader.has_value());
    CHECK(Unwrap(admitHeader).opRaw == 0x16);

    auto const forget = EncodeClusterRequest(Ask(ClusterAction::ForgetClient, "10.0.0.7"));
    auto const forgetHeader = Wire::DecodeRequestHeader(forget);
    REQUIRE(forgetHeader.has_value());
    CHECK(Unwrap(forgetHeader).opRaw == 0x17);

    // And they are not the same frame, which is what a shared encoder could get wrong
    // while both cases above still passed.
    CHECK(admit != forget);
}

// --------------------------------------------------------------------------
// Learners (#1449).

TEST_CASE("A learner admission takes --cluster-admit's token and encodes as its own op", "[node][clusteradmin][learner]")
{
    auto const cfg = ParsedFrom({ "--cluster-admit-learner=laptop=10.0.0.9:6680" });
    CHECK(cfg.cluster.action == ClusterAction::AdmitLearner);
    CHECK(cfg.cluster.key == "laptop");
    CHECK(cfg.cluster.value == "10.0.0.9:6680");

    // The same refusals, since it is the same grammar through the same parser.
    for (auto const* const spec: { "--cluster-admit-learner=laptop", "--cluster-admit-learner=laptop=nowhere" })
    {
        INFO("spec: " << spec);
        NodeConfig refused;
        std::vector<char const*> const args { spec };
        CHECK_FALSE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, refused).has_value());
    }

    // The BYTE the request carries, and not the voter admission's: a learner admitted
    // as a voter grows the very quorum it was admitted to stay out of.
    auto const learner = EncodeClusterRequest(Ask(ClusterAction::AdmitLearner, "laptop", "10.0.0.9:6680"));
    auto const learnerHeader = Wire::DecodeRequestHeader(learner);
    REQUIRE(learnerHeader.has_value());
    CHECK(Unwrap(learnerHeader).opRaw == 0x1C);

    auto const voterHeader =
        Wire::DecodeRequestHeader(EncodeClusterRequest(Ask(ClusterAction::Admit, "laptop", "10.0.0.9:6680")));
    REQUIRE(voterHeader.has_value());
    CHECK(Unwrap(voterHeader).opRaw == 0x0B);
}

TEST_CASE("A learner admission reaches the cluster as AddLearner and says which seat it asked for",
          "[node][clusteradmin][learner]")
{
    // End to end through the real service and protocol, for the receipt case's reason:
    // the flag, the op, the scheduler's verb choice and the renderer must all AGREE.
    FakeCluster cluster;
    Fixture fixture;
    fixture.service.AdministerWith(cluster);

    auto const reply = fixture.Ask(Ask(ClusterAction::AdmitLearner, "laptop", "10.0.0.9:6680"));
    REQUIRE(StatusOf(reply) == Wire::Status::Ok);
    REQUIRE(cluster.proposed.size() == 1);
    CHECK(cluster.proposed[0] == Cmd(Cluster::CommandKind::AddLearner, "laptop", "10.0.0.9:6680"));

    auto const rendered = InterpretClusterReply(ClusterAction::AdmitLearner, PayloadOf(reply));
    REQUIRE(rendered.has_value());
    CHECK(rendered->contains("10.0.0.9:6680"));
    CHECK(rendered->contains("learner (the verb this request was sent as)"));
    // The ceiling holds for this verb too.
    CHECK(rendered->contains("Appended, not committed"));
    CHECK_FALSE(rendered->contains("admitted"));

    // And the voter admission names its own seat, so a renderer spelling one seat for
    // both verbs is red here.
    auto const voter = InterpretClusterReply(ClusterAction::Admit,
                                             PayloadOf(fixture.Ask(Ask(ClusterAction::Admit, "n4", "10.0.0.4:6680"))));
    REQUIRE(voter.has_value());
    CHECK(voter->contains("voter (the verb this request was sent as)"));
}

TEST_CASE("A status report names the seat each member was admitted into", "[node][clusteradmin][learner]")
{
    auto state = Agreed();
    Apply(state, Cmd(Cluster::CommandKind::AddLearner, "laptop", "10.0.0.9:6680"));

    auto const rendered = RenderClusterState(state);
    CHECK(rendered.contains("seat=learner raft=10.0.0.9:6680"));
    CHECK(rendered.contains("seat=voter raft=10.0.0.1:6680"));
    CHECK(rendered.contains("seat=voter raft=10.0.0.2:6680"));
}

TEST_CASE("A status report shows each member's key, the principals and the revoked keys", "[node][clusteradmin][identity]")
{
    // #178. Whole keys, in the one spelling `--node-status` prints on the machine itself, and
    // ABSENT as the dash every other absent field here is -- never an empty `key=` somebody
    // could read as a key. The two new sections say "(none)" when empty, for the members'
    // reason: after a revocation an operator needs "none" to be an answer.
    auto keyed = Cmd(Cluster::CommandKind::AddMember, "keyed", "10.0.0.1:6680");
    keyed.publicKey = Ed25519PublicKey {};
    keyed.publicKey->fill(std::byte { 0x2B });
    auto principal = Cmd(Cluster::CommandKind::AdmitPrincipal, "worker-1");
    principal.publicKey = Ed25519PublicKey {};
    principal.publicKey->fill(std::byte { 0x2C });
    principal.role = Cluster::PrincipalRole::Worker;
    // Revoked the one way a key is: its holder was forgotten (#1555).
    auto revoked = Cmd(Cluster::CommandKind::AdmitPrincipal, "gone");
    revoked.publicKey = Ed25519PublicKey {};
    revoked.publicKey->fill(std::byte { 0x2D });
    revoked.role = Cluster::PrincipalRole::Worker;

    SECTION("an empty roster says so")
    {
        Cluster::ClusterState state;
        Apply(state, Cmd(Cluster::CommandKind::AddMember, "plain", "10.0.0.2:6680"));
        auto const rendered = RenderClusterState(state);
        CHECK(rendered.contains("key=-"));
        CHECK(rendered.contains("principals (0):\n  (none)"));
        CHECK(rendered.contains("revoked keys (0):\n  (none)"));
    }

    SECTION("a full one names every key whole")
    {
        Cluster::ClusterState state;
        Apply(state, keyed);
        Apply(state, principal);
        Apply(state, revoked);
        Apply(state, Cmd(Cluster::CommandKind::Forget, "gone"));
        auto const rendered = InterpretClusterReply(ClusterAction::Status, Cluster::Encode(state));
        REQUIRE(rendered.has_value());
        INFO(*rendered);
        CHECK(rendered->contains(std::format("key={}", FormatEd25519PublicKey(*keyed.publicKey))));
        CHECK(rendered->contains("principals (1):"));
        CHECK(rendered->contains(std::format("role=worker key={}", FormatEd25519PublicKey(*principal.publicKey))));
        CHECK(rendered->contains("revoked keys (1):"));
        CHECK(rendered->contains(std::format("key={}", FormatEd25519PublicKey(*revoked.publicKey))));
    }
}
