// SPDX-License-Identifier: Apache-2.0
#include "FormationRuntime.hpp"
#include "LiveNodeConfig.hpp"
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeReload.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/ManualClockWait.hpp>
#include <tests/NodeFormationControllerFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// How long the admin below waits for the controller to answer while a proposal is in its hands: a
/// controller still holding its lock never answers, and the probe says so instead of hanging.
constexpr auto OffLockBound = std::chrono::seconds { 5 };

/// The consensus tier, on paper: every proposal recorded, and -- while it is being proposed -- whether
/// the controller answered another thread, which it cannot while it holds its own lock.
class ProbingClusterAdmin final: public Distributed::IClusterAdmin
{
  public:
    /// @param controller The controller to ask while a proposal is in hand; must outlive every proposal.
    void Probe(FormationController const* controller) noexcept
    {
        _controller.store(controller);
    }

    /// @return Nothing applied.
    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        return {};
    }

    /// @copydoc Distributed::IClusterAdmin::ProposeToCluster
    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
    {
        auto answered = std::optional<bool> {};
        auto answer = std::future<CompileCacheWire::FleetSummary> {};
        if (auto const* controller = _controller.load(); controller != nullptr)
        {
            answer = std::async(std::launch::async, [controller] { return controller->Current(); });
            answered = answer.wait_for(OffLockBound) == std::future_status::ready;
        }
        std::scoped_lock const lock { _lock };
        _proposed.push_back(command);
        _answeredWhileProposing.push_back(answered);
        // Kept, never destroyed here: a future of `std::async` waits for its answer as it is destroyed,
        // and a controller calling this under its lock answers only once this call has returned.
        _answers.push_back(std::move(answer));
        return {};
    }

    /// @return Every command proposed, in order.
    [[nodiscard]] std::vector<Cluster::Command> Proposed() const
    {
        std::scoped_lock const lock { _lock };
        return _proposed;
    }

    /// @return Per proposal: whether the controller answered another thread while it was in hand;
    ///         nothing when no controller was being probed.
    [[nodiscard]] std::vector<std::optional<bool>> AnsweredWhileProposing() const
    {
        std::scoped_lock const lock { _lock };
        return _answeredWhileProposing;
    }

  private:
    std::atomic<FormationController const*> _controller { nullptr };   ///< What is asked mid-proposal.
    mutable std::mutex _lock;                                          ///< Guards what is recorded.
    std::vector<Cluster::Command> _proposed;                           ///< Every command, in order.
    std::vector<std::optional<bool>> _answeredWhileProposing;          ///< Per proposal, the probe.
    std::vector<std::future<CompileCacheWire::FleetSummary>> _answers; ///< Each probe's answer, held.
};

/// One body's formation over fakes: every seam injected, none built inside the runtime.
struct RuntimeRig
{
    /// @param nodeId This node's id; its key is `TestKeyPair(nodeId)`.
    /// @param record The record its body was adopted from.
    RuntimeRig(std::string const& nodeId, Cluster::FormationRecord record):
        body { .record = std::move(record),
               .durables = FormationDurables { store, endpoints, reform, Parts() },
               .reloader = nullptr }
    {
        cfg.nodeId = nodeId;
        cfg.identityPublicKey = Testing::TestKeyPair(nodeId).PublicKey();
        cfg.hostNames = NodeHostNames { .fqdn = nodeId.substr(2) + ".corp.example", .dnsSuffix = {}, .withheld = {} };
        // Every move is judged by the startup rules (`StartupShapeJudge`), and today those refuse a
        // worker that names no scheduler (Task 24 moves registration to `SchedulersOf`): named here as
        // an operator must name it now, its own node port.
        cfg.schedulers = { nodeId.substr(2) + ".corp.example:6674" };
        REQUIRE(ApplyFormation(cfg, body.record, {}).has_value());
    }

    /// @return The seams, reading this rig.
    [[nodiscard]] FormationRuntimeParts Parts()
    {
        return FormationRuntimeParts { .wall = wall, .random = random, .dialer = dialer, .srv = srv, .wait = wait };
    }

    Testing::ManualClockWait wait; ///< What the beat waits its interval through, and the clock it reads.
    core::platform::ManualWallClock wall;
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::Ascending(64) };
    Testing::UnreachableDialer dialer;
    Testing::ScriptedSrvResolver srv;
    Testing::InMemoryFormationStore store;
    Testing::ScratchDirectory scratch { "formation-runtime" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    ReformRequest reform;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    NodeConfig cfg;
    FormationBody body;
};

/// A member of an applied state.
/// @param id Its id; its key is `TestKeyPair(id)`.
/// @param raftEndpoint Where its consensus port answers; empty for a learner.
/// @param seat Its seat.
/// @return The member.
[[nodiscard]] Cluster::ClusterMember Member(std::string const& id, std::string raftEndpoint, Cluster::MemberSeat seat)
{
    return Cluster::ClusterMember { .id = id,
                                    .raftEndpoint = std::move(raftEndpoint),
                                    .schedulerEndpoint = {},
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = seat,
                                    .publicKey = Testing::TestKeyPair(id).PublicKey() };
}

/// The office's applied state: `n-office` and `n-desk` voters, `n-laptop` a learner.
/// @return The state.
[[nodiscard]] Cluster::ClusterState OfficeWithLaptop()
{
    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", Cluster::MemberSeat::Voter));
    state.members.push_back(Member("n-desk", "desk:6680", Cluster::MemberSeat::Voter));
    state.members.push_back(Member("n-laptop", "", Cluster::MemberSeat::Learner));
    return state;
}

/// What `c-home` says, older than the office, spoken by the office's own voter `n-desk`: evidence a
/// leader of the office proposes a dissolve on.
/// @return The proven fleet.
[[nodiscard]] Cluster::ProvenFleet HomeSpokenByTheDesk()
{
    return Testing::ProvenBeacon(CompileCacheWire::FleetSummary { .clusterId = "c-home",
                                                                  .state = CompileCacheWire::FleetState::Established,
                                                                  .createdAtUnixSeconds = 100,
                                                                  .leaderId = "n-home",
                                                                  .leaderNodeEndpoint = "home:6674",
                                                                  .nodeId = "n-desk",
                                                                  .raftEndpoint = "",
                                                                  .members = { "n-home", "n-desk", "n-office" },
                                                                  .memberTotal = 3,
                                                                  .nodeEndpoint = "",
                                                                  .leaderKey = Testing::TestKeyPair("n-home").PublicKey(),
                                                                  .pointsAt = {} });
}

/// Where the reload case's operator moves `--advertise` to.
constexpr std::string_view ReloadedAdvertise = "elsewhere.corp.example:6674";

/// A startup rule over a RELOADABLE flag, stated through the judge's own seam: it refuses a node
/// advertising `ReloadedAdvertise`.
/// @param cfg A shaped configuration.
/// @return The refusal, or nothing.
[[nodiscard]] std::optional<std::string> RefuseAdvertisingElsewhere(NodeConfig const& cfg)
{
    if (cfg.advertise == ReloadedAdvertise)
        return std::string { "a test rule refuses advertising elsewhere.corp.example:6674" };
    return std::nullopt;
}

/// @param logger What was said.
/// @param text A fragment.
/// @return Whether any line said @p text.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view text)
{
    return std::ranges::any_of(logger.Snapshot(),
                               [text](CapturingLogger::Record const& record) { return record.message.contains(text); });
}

} // namespace

TEST_CASE("Seeds are the remembered endpoints, then the typed ones, then the SRV record's", "[node][formation][runtime]")
{
    Testing::ScratchDirectory const scratch { "formation-runtime-seeds" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    REQUIRE(endpoints
                .Save(Cluster::FleetEndpoints { .clusterId = "c-office",
                                                .voters = { Cluster::FleetEndpoint { .id = "n-office",
                                                                                     .raftEndpoint = "office:6680",
                                                                                     .nodeEndpoint = "office:6674" } } })
                .has_value());
    auto const typed = std::vector<std::string> { "desk:6674" };
    Testing::ScriptedSrvResolver srv;
    srv.Answer("_fastcache._tcp.corp.example",
               std::vector { SrvTarget { .host = "build.corp.example", .port = 6674, .priority = 0, .weight = 0 } });

    auto const seeds = SeedsNow(endpoints, typed, srv, "corp.example");
    CHECK(seeds
          == std::vector {
              Cluster::SeedCandidate { .endpoint = "office:6674", .source = Cluster::SeedSource::Remembered },
              Cluster::SeedCandidate { .endpoint = "desk:6674", .source = Cluster::SeedSource::FleetSeedFlag },
              Cluster::SeedCandidate { .endpoint = "build.corp.example:6674", .source = Cluster::SeedSource::DnsSrv },
          });

    // A lookup that fails costs that source and nothing else.
    auto const unanswered = SeedsNow(endpoints, typed, srv, "elsewhere.example");
    CHECK(unanswered
          == std::vector {
              Cluster::SeedCandidate { .endpoint = "office:6674", .source = Cluster::SeedSource::Remembered },
              Cluster::SeedCandidate { .endpoint = "desk:6674", .source = Cluster::SeedSource::FleetSeedFlag },
          });

    // No domain asks no question at all.
    auto const asked = srv.Asked();
    auto const local = SeedsNow(endpoints, typed, srv, "");
    CHECK(local.size() == 2);
    CHECK(srv.Asked() == asked);
}

TEST_CASE("A body whose configuration runs no consensus builds no formation, and every reader falls back",
          "[node][formation][runtime]")
{
    RuntimeRig rig { "n-office", Testing::Minted("c-office", 200) };
    auto const closed = NodeConfig {};
    REQUIRE_FALSE(RunsConsensus(closed));
    auto const none = MakeFormationRuntime(closed, rig.body, nullptr, rig.metrics, rig.logger, nullptr);
    REQUIRE(none.has_value());
    CHECK(*none == nullptr);
    REQUIRE(RunsConsensus(rig.cfg));
    auto const built = MakeFormationRuntime(rig.cfg, rig.body, nullptr, rig.metrics, rig.logger, nullptr);
    REQUIRE(built.has_value());
    CHECK(*built != nullptr);

    // Every reader of a body with no formation answers what such a node answers, never a dangling one.
    Testing::ScriptedSummarySource const fallback { CompileCacheWire::FleetSummary {} };
    CHECK(&SummarySourceOf(nullptr, &fallback) == &fallback);
    CHECK(AskedJoinsOf(nullptr) == nullptr);
    auto const discovery = DiscoveryFormationOf(nullptr);
    CHECK(discovery.evidence == nullptr);
    CHECK(discovery.fleets == nullptr);
    auto const hooks = FormationHooksOf(nullptr);
    CHECK_FALSE(static_cast<bool>(hooks.onState));
    CHECK_FALSE(static_cast<bool>(hooks.onOwnKeyRevoked));
}

TEST_CASE("A formation's beat proposes off the controller's lock, and once it stops the tier is never called again",
          "[node][formation][runtime][teardown]")
{
    // The two properties a body's teardown rests on: the beat calls the tier only outside the
    // controller's lock -- the tier's hooks call back INTO the controller -- and once the beat is
    // ended, which a body does before its consensus tier is destroyed, no proposal reaches that tier.
    RuntimeRig rig { "n-office", Testing::Minted("c-office", 200) };
    auto made = MakeFormationRuntime(rig.cfg, rig.body, nullptr, rig.metrics, rig.logger, nullptr);
    REQUIRE(made.has_value());
    auto runtime = *std::move(made);
    REQUIRE(runtime != nullptr);
    ProbingClusterAdmin tier;
    tier.Probe(&runtime->Controller());

    // The office leads its fleet and sees an older split of it, spoken by its own voter: its leader
    // proposes a dissolve on the next beat.
    runtime->Hooks().onState(OfficeWithLaptop(), "c-office", Consensus::NodeId { "n-office" }, "office:6674");
    REQUIRE(runtime->Controller().Mode() == Cluster::NodeMode::Voter);
    runtime->Controller().OnFleetProven(HomeSpokenByTheDesk());

    auto beat = BeginFormation(runtime.get(), &tier);
    REQUIRE(Testing::WaitUntil(
        "the beat to propose through the tier",
        [&tier] { return !tier.Proposed().empty(); },
        [] { return std::string { "nothing proposed yet" }; }));
    beat.reset(); // what a body's teardown does before its consensus tier goes

    REQUIRE(tier.Proposed().size() == 1);
    CHECK(tier.Proposed()[0].kind == Cluster::CommandKind::DissolveInto);
    CHECK(tier.AnsweredWhileProposing() == std::vector { std::optional { true } });

    // Proven again past the interval, the controller proposes again -- and the tier is not reached.
    rig.wait.Advance(SeedProbeInterval);
    runtime->Controller().OnFleetProven(HomeSpokenByTheDesk());
    runtime->Controller().Tick();
    CHECK(tier.Proposed().size() == 1);
    CHECK(Logged(rig.logger, "cannot propose dissolving this fleet into c-home"));
}

TEST_CASE("The startup judge reads the configuration in force, so an accepted reload changes what it judges",
          "[node][formation][runtime][judge][reload]")
{
    // `--advertise` is `Reloadable::Yes`: a judge holding the configuration its body started with
    // would go on judging the old flag after an operator's accepted reload moved it.
    RuntimeRig rig { "n-laptop", Testing::Minted("c-laptop", 500) };
    auto reloaded = rig.cfg;
    reloaded.advertise = std::string { ReloadedAdvertise };
    NodeReloader reloader { rig.cfg,
                            std::filesystem::path { "unread.yaml" },
                            [reloaded](std::filesystem::path const&) -> std::expected<NodeConfig, ConfigError> {
                                return reloaded;
                            },
                            &ValidateNodeReloadable };
    LiveNodeConfig const live { rig.cfg, &reloader };
    StartupShapeJudge const judge { live, rig.endpoints, RefuseAdvertisingElsewhere };
    CHECK_FALSE(judge.RefusalOf(rig.body.record).has_value());

    auto const accepted = reloader.Reload();
    INFO((accepted.has_value() ? std::string {} : accepted.error().context));
    REQUIRE(accepted.has_value());
    CHECK(judge.RefusalOf(rig.body.record)
          == std::optional { std::string { "a test rule refuses advertising elsewhere.corp.example:6674" } });
}

TEST_CASE("The startup judge refuses a loopback-named founder's voter shape, unreachable by a controller on this branch",
          "[node][formation][runtime][judge]")
{
    // The shape I-1's check exists for: a solitary founder whose name reaches only this machine, moved
    // to a VOTER, whose consensus port peers dial. The judge every move asks refuses it, by name.
    auto cfg = NodeConfig {};
    cfg.nodeId = "n-solo";
    cfg.slots = 0; // no worker: the rule judged is consensus's, and nothing else is asked
    cfg.identityPublicKey = Testing::TestKeyPair("n-solo").PublicKey();
    ApplyHostNames(cfg, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" });
    auto const solitary = Testing::Minted("c-solo", 500);
    REQUIRE(ApplyFormation(cfg, solitary, {}).has_value());
    REQUIRE_FALSE(StartupPolicyRejection(cfg).has_value()); // the start passes it

    Testing::ScratchDirectory const scratch { "shape-judge" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    LiveNodeConfig const live { cfg, nullptr };
    StartupShapeJudge const judge { live, endpoints, StartupPolicyRejection };
    CHECK_FALSE(judge.RefusalOf(solitary).has_value());
    auto voter = solitary;
    voter.mode = Cluster::NodeMode::Voter;
    CHECK(judge.RefusalOf(voter) == std::optional { std::string { ConsensusNameReachesOnlyThisMachineRefusal } });

    // And why no production transition reaches it ON THIS BRANCH ONLY: such a founder's consensus
    // stands down here, so its body builds no formation at all -- no controller exists to take the move.
    // Task 24 reverses that premise (a loopback-named node runs SOLITARY on loopback, consensus ON), so
    // after that merge the founder HAS a controller and this judge is what refuses its move into a
    // multi-member shape, raising `formation-move-refused`. The assertions below are then false by
    // design, and this half is replaced at that integration by a case driving that refusal.
    CHECK_FALSE(RunsConsensus(cfg));
    RuntimeRig rig { "n-solo", solitary };
    auto const none = MakeFormationRuntime(cfg, rig.body, nullptr, rig.metrics, rig.logger, nullptr);
    REQUIRE(none.has_value());
    CHECK(*none == nullptr);
}

TEST_CASE("A formation's beat waits its interval on the injected clock, never the wall's", "[node][formation][runtime]")
{
    // The one interval the runtime keeps that is not a reading of `FormationRuntimeParts::clock`
    // would be the beat's own: waited on the wall, a case could only ever see its FIRST beat.
    RuntimeRig rig { "n-office", Testing::Minted("c-office", 200) };
    auto made = MakeFormationRuntime(rig.cfg, rig.body, nullptr, rig.metrics, rig.logger, nullptr);
    REQUIRE(made.has_value());
    auto runtime = *std::move(made);
    REQUIRE(runtime != nullptr);
    ProbingClusterAdmin tier;

    auto beat = BeginFormation(runtime.get(), &tier);
    auto const entered = [&rig](std::size_t intervals) {
        return Testing::WaitUntil(
            std::format("the beat to enter interval {}", intervals),
            [&rig, intervals] { return rig.wait.Waits() >= intervals; },
            [&rig] { return std::format("{} interval(s) entered", rig.wait.Waits()); });
    };
    auto const first = entered(1);
    // A whole interval on the manual clock is what moves it -- and nothing else.
    rig.wait.Advance(FormationRuntime::TickInterval);
    auto const second = entered(2);
    beat.reset();

    CHECK(first);
    CHECK(second);
    CHECK(rig.wait.Waits() == 2);
}

TEST_CASE("A formation is refused, by name, on a node that runs consensus and holds no identity key",
          "[node][formation][runtime]")
{
    // The key is what every Enroll states and every summary is signed under. Defaulted to zero, it
    // passes every check this side makes -- the window records zero, an approval made from the row
    // admits zero, the joiner validates the roster against the zero it sent -- so it is refused here.
    RuntimeRig rig { "n-laptop", Testing::Minted("c-laptop", 200) };
    auto keyless = rig.cfg;
    keyless.identityPublicKey.reset();
    REQUIRE(RunsConsensus(keyless));
    auto const refused = MakeFormationRuntime(keyless, rig.body, nullptr, rig.metrics, rig.logger, nullptr);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == FormationNeedsIdentityKey);
}
