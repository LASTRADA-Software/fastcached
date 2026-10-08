// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "NodeFormation.hpp"
#include "NodeReload.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

// **The removal direction is the test** (#405).
//
// `--fleet-open` is the admission setting a reload can move, and its two directions
// are not symmetric. Turned ON and not yet in force it fails CLOSED: a caller is
// refused until somebody restarts the node, which is annoying, self-healing and
// visible from the machine being refused. Turned OFF and still in force it fails
// OPEN: every caller the roster does not admit keeps being served, and nothing reports
// it, because admission succeeding is the ordinary case.
//
// So the cases are about narrowing, and about the one widening that is refused.

using namespace FastCache;

/// `NodeMembership` reports an unreadable `fleet-open` row here; no case asserts on it.
namespace
{
FastCache::NullLogger membershipLog;
}
using namespace FastCache::Node;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A peer that is not this machine. `LoopbackMembership` answers `Member` for the whole
/// of `127.0.0.0/8`, so a loopback caller could never show that a policy decides anything.
constexpr std::string_view Stranger = "10.0.0.99";

/// Write @p text, verbatim, to a configuration file this case owns.
/// @param dir Scratch directory.
/// @param text The whole file.
/// @return The path written.
[[nodiscard]] std::filesystem::path WriteFile(std::filesystem::path const& dir, std::string_view text)
{
    std::filesystem::create_directories(dir);
    auto const path = dir / "node.yaml";
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out << text;
    return path;
}

/// Where the worker in these cases keeps its identity (#178): a node naming a scheduler must
/// hold one, and `cluster_dir` is not reloadable, so the file and the live configuration name
/// the same one. Never read -- validation asks the configuration, not the disk.
constexpr std::string_view StateDirectory = "node-state";

/// Write @p body to a worker's configuration file this case owns.
///
/// It names no `scheduler:`: a node that serves is refused one, and registers where its
/// formation record says. And it leaves consensus on, as it is by default: a worker with its
/// consensus port closed never registers anywhere and is refused at startup.
/// @param dir Scratch directory.
/// @param body The keys this case is about; the state-directory line is added.
/// @return The path written.
[[nodiscard]] std::filesystem::path WriteConfig(std::filesystem::path const& dir, std::string_view body)
{
    return WriteFile(dir, std::format("cluster_dir: {}\n{}", StateDirectory, body));
}

/// Read @p path into a fresh configuration, exactly as the worker's reloader does.
/// @param path The configuration file.
/// @return The candidate, or why it could not be read.
[[nodiscard]] std::expected<NodeConfig, ConfigError> Reparse(std::filesystem::path const& path)
{
    NodeConfig candidate;
    auto const loaded = ReadYamlSettings(path).and_then([&candidate, &path](std::vector<YamlSetting> const& settings) {
        return ApplyNodeConfiguration(settings, path, {}, candidate);
    });
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    return candidate;
}

/// The configuration a node in these cases is running with.
/// @param open Whether `--fleet-open` was given.
/// @return The live configuration.
[[nodiscard]] NodeConfig RunningNode(bool open = false)
{
    NodeConfig cfg;
    cfg.clusterDir = StateDirectory;
    cfg.fleetOpen = open;
    return cfg;
}

/// Whether an oracle a surface bound at STARTUP admits @p host right now.
///
/// **The parameter is the oracle and not the membership object, and that is what makes
/// these cases able to fail.** Production surfaces take an `IMembershipOracle const&`
/// once, at construction, and hold it for their lifetime -- `SchedulerTier::Start` and
/// `CompileResponder` both do. A case that re-asked `membership.Oracle()` after every
/// reload would pass for an implementation whose `Oracle()` still handed out one of two
/// owned objects chosen by a flag read once, which is precisely the shape that cannot
/// see `--fleet-open` change. Bind it before the reload, classify through it after.
/// @param oracle The seam a surface is holding.
/// @param host A caller's address.
/// @return True when that caller is a member.
[[nodiscard]] bool Admits(Distributed::IMembershipOracle const& oracle, std::string_view host)
{
    return oracle.Classify(host) == Distributed::Membership::Member;
}

} // namespace

TEST_CASE("A reload candidate is shaped by the formation, the names and the identity the start resolved",
          "[node][reload][formation]")
{
    // Through the reader `main` hands the reloader, so a shaping step dropped from it is a red
    // here rather than a declined SIGHUP in production. The formation is the one that decides
    // most: a candidate shaped by no record runs no consensus and serves no scheduler, while the
    // running node does both.
    Testing::ScratchDirectory const scratch { "node-reload-shape" };
    auto const path = WriteFile(scratch.Path(), std::format("cluster_dir: {}\n", (scratch / "state").generic_string()));
    auto const record = Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                                   .own = { .clusterId = "own-c", .createdAtUnixSeconds = 100 },
                                                   .joining = std::nullopt,
                                                   .fleet = std::nullopt,
                                                   .archivePending = std::nullopt,
                                                   .rejectedBy = std::nullopt,
                                                   .askedJoins = {} };
    auto const identity = NodeIdentity { .id = "n1",
                                         .origin = NodeIdentityOrigin::Recorded,
                                         .publicKey = Testing::TestKeyPair("n1").PublicKey() };
    auto const stateDirectory =
        NodeStateDirectoryChoice { .path = scratch / "state", .origin = StateDirectoryOrigin::Named };
    auto const read = ReloadCandidateReader(
        {},
        ReloadBasis {
            .stateDirectory = stateDirectory,
            .hostNames = NodeHostNames { .fqdn = "box.corp.example", .dnsSuffix = "corp.example", .withheld = {} },
            .formation =
                [&record] {
                    return std::expected<KeptFormation, std::string> { KeptFormation { .record = record,
                                                                                       .remembered = {} } };
                },
            .identity = identity,
        });

    auto const candidate = read(path);
    REQUIRE(candidate.has_value());
    CHECK(candidate->stateDirectory == std::optional { stateDirectory });
    CHECK(Testing::Unwrap(candidate->hostNames).fqdn == "box.corp.example");
    CHECK(Testing::Unwrap(candidate->formation).mode == Cluster::NodeMode::Solitary);
    CHECK(candidate->clusterId == "own-c");
    CHECK(RunsConsensus(*candidate));
    CHECK(ServesScheduler(*candidate));
    CHECK(candidate->nodeId == "n1");
    CHECK(candidate->identityPublicKey == identity.publicKey);

    // And a reload of the running node that read the same file is accepted: nothing the start
    // resolved reads as a field the reload changed.
    NodeReloader reloader { *candidate, path, read, &ValidateNodeReloadable };
    auto const reloaded = reloader.Reload();
    INFO((reloaded.has_value() ? std::string {} : reloaded.error().context));
    CHECK(reloaded.has_value());
}

TEST_CASE("A reload candidate carries the route host the running node derives its endpoints from",
          "[node][reload][endpoint]")
{
    // Read at the reload, never captured: a candidate shaped without the live route host derives an
    // `auto` endpoint the running node does not advertise, and the startup table would judge that.
    Testing::ScratchDirectory const scratch { "node-reload-route" };
    auto const path = WriteFile(scratch.Path(), std::format("cluster_dir: {}\n", (scratch / "state").generic_string()));
    auto const record = Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                                   .own = { .clusterId = "own-c", .createdAtUnixSeconds = 100 },
                                                   .joining = std::nullopt,
                                                   .fleet = std::nullopt,
                                                   .archivePending = std::nullopt,
                                                   .rejectedBy = std::nullopt,
                                                   .askedJoins = {} };
    auto route = std::string { "192.168.7.2" };
    auto const read =
        ReloadCandidateReader({},
                              ReloadBasis {
                                  .stateDirectory = std::nullopt,
                                  .hostNames = NodeHostNames { .fqdn = "box.lan", .dnsSuffix = "lan", .withheld = {} },
                                  .formation =
                                      [&record] {
                                          return std::expected<KeptFormation, std::string> { KeptFormation {
                                              .record = record, .remembered = {} } };
                                      },
                                  .identity = NodeIdentity {},
                                  .routeHost = [&route] { return route; },
                              });

    auto const candidate = read(path);
    REQUIRE(candidate.has_value());
    CHECK(candidate->routeHost == std::optional<std::string> { "192.168.7.2" });

    // And a route lost since is no route, never an engaged empty host.
    route.clear();
    auto const offline = read(path);
    REQUIRE(offline.has_value());
    CHECK_FALSE(offline->routeHost.has_value());
}

TEST_CASE("A reload is shaped by the formation kept at the reload and not by the record the start kept",
          "[node][reload][formation]")
{
    // A reform rewrites the record while the process runs. A candidate shaped by the record the
    // START kept would put the node, in its live configuration, back into the mode it left -- and
    // the worker reads where it registers from that configuration.
    Testing::ScratchDirectory const scratch { "node-reload-reformed" };
    auto const path = WriteFile(scratch.Path(), std::format("cluster_dir: {}\n", (scratch / "state").generic_string()));
    auto kept = Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                           .own = { .clusterId = "own-c", .createdAtUnixSeconds = 100 },
                                           .joining = std::nullopt,
                                           .fleet = std::nullopt,
                                           .archivePending = std::nullopt,
                                           .rejectedBy = std::nullopt,
                                           .askedJoins = {} };
    auto const read = ReloadCandidateReader(
        {},
        ReloadBasis {
            .stateDirectory = NodeStateDirectoryChoice { .path = scratch / "state", .origin = StateDirectoryOrigin::Named },
            .hostNames = NodeHostNames { .fqdn = "box.corp.example", .dnsSuffix = "corp.example", .withheld = {} },
            .formation =
                [&kept] {
                    return std::expected<KeptFormation, std::string> { KeptFormation { .record = kept, .remembered = {} } };
                },
            .identity = NodeIdentity { .id = "n1",
                                       .origin = NodeIdentityOrigin::Recorded,
                                       .publicKey = Testing::TestKeyPair("n1").PublicKey() },
        });

    auto const before = read(path);
    REQUIRE(before.has_value());
    CHECK(Testing::Unwrap(before->formation).mode == Cluster::NodeMode::Solitary);
    CHECK(before->clusterId == "own-c");

    // The node moved: a dissolve left its own cluster for a fleet, and the reform is done.
    kept.mode = Cluster::NodeMode::Voter;
    kept.own.clusterId = "other-c";
    auto const after = read(path);
    REQUIRE(after.has_value());
    CHECK(Testing::Unwrap(after->formation).mode == Cluster::NodeMode::Voter);
    CHECK(after->clusterId == "other-c");
}

TEST_CASE("A reload declines when the formation record cannot be read or is gone", "[node][reload][formation]")
{
    Testing::ScratchDirectory const scratch { "node-reload-unreadable" };
    auto const path = WriteFile(scratch.Path(), std::format("cluster_dir: {}\n", (scratch / "state").generic_string()));
    auto answer = std::expected<KeptFormation, std::string> { std::unexpected {
        std::string { "the formation record was written in format 9" } } };
    auto const read = ReloadCandidateReader(
        {},
        ReloadBasis {
            .stateDirectory = NodeStateDirectoryChoice { .path = scratch / "state", .origin = StateDirectoryOrigin::Named },
            .hostNames = NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = {} },
            .formation = [&answer] { return answer; },
            .identity = NodeIdentity { .id = "n1", .origin = NodeIdentityOrigin::Recorded, .publicKey = std::nullopt },
        });

    auto const unreadable = read(path);
    REQUIRE_FALSE(unreadable.has_value());
    CHECK(unreadable.error().context.contains("format 9"));

    answer = KeptFormation { .record = std::nullopt, .remembered = {} };
    auto const gone = read(path);
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().context.contains("gone"));
}

TEST_CASE("A reload may not widen admission while the running worker verifies no lease", "[node][membership][reload][lease]")
{
    // Review I-2(b). A worker's lease check is chosen ONCE, at startup, and one that verifies
    // nothing is safe only while no machine but this one reaches its compile verbs -- so a reload
    // widening admission past it would open an unauthenticated compile port with every refusal
    // counter reading zero. Keyed on what the running worker BUILT (`LeaseCheckInForce`), never on
    // a flag shape, so a path no shape foresaw cannot reach it unguarded.
    //
    // Asserted on the REFUSAL, by its own words, and on the oracle together: "the reload was
    // declined" and "nothing took effect" are two claims.
    Testing::ScratchDirectory const scratch { "node-reload-unchecked-widen" };
    auto const path = WriteConfig(scratch.Path(), "fleet_open: true\n");

    auto const initial = RunningNode();
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    std::ostringstream sink;
    ConsoleLogger logger { sink, LogLevel::Info, LogTimestamps::No };

    LeaseCheckInForce inForce;
    inForce.Record(BuiltLeaseCheck::Unchecked);
    NodeReloader reloader { initial, path, &Reparse, ReloadCheckWith(inForce) };
    NodeConditions conditions;
    ApplyReloadRequest(&reloader, membership, conditions, logger);

    CHECK_FALSE(Admits(oracle, Stranger));
    CHECK(sink.str().contains("may not widen admission with --fleet-open while this worker compiles WITHOUT verifying"));
    CHECK_FALSE(reloader.Current()->fleetOpen);
}

TEST_CASE("A reload widens admission on a worker that verifies its leases, and on a node that built no worker",
          "[node][membership][reload][lease]")
{
    // The control for the case above: the SAME save, refused only because of what was built. A
    // worker checking every grant against its roster may admit whom it likes, and so may a node
    // that built no lease check at all (`--slots=0`, which serves no compile verb).
    auto const built = GENERATE(BuiltLeaseCheck::Signed, BuiltLeaseCheck::None);
    INFO("built: " << static_cast<int>(built));
    Testing::ScratchDirectory const scratch { "node-reload-checked-widen" };
    auto const path = WriteConfig(scratch.Path(), "fleet_open: true\n");

    auto const initial = RunningNode();
    NodeMembership membership { initial, membershipLog };
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    LeaseCheckInForce inForce;
    inForce.Record(built);
    NodeReloader reloader { initial, path, &Reparse, ReloadCheckWith(inForce) };
    NodeConditions conditions;
    ApplyReloadRequest(&reloader, membership, conditions, logger);

    CHECK(Admits(oracle, Stranger));
}

TEST_CASE("Narrowing is allowed on an unchecked worker, and a widening refusal names every setting first",
          "[node][membership][reload][lease]")
{
    // Asked as a WIDENING rather than as a state: a worker already admitting a remote peer passed
    // its own startup rules, and narrowing is the one edit that makes it safer.
    LeaseCheckInForce inForce;
    inForce.Record(BuiltLeaseCheck::Unchecked);
    auto const check = ReloadCheckWith(inForce);
    auto const admitting = RunningNode(/*open=*/true);
    auto const narrowed = check(admitting, RunningNode());
    INFO((narrowed.has_value() ? std::string {} : narrowed.error().context));
    CHECK(narrowed.has_value());

    // And LAST: a save that both widens and moves an unreloadable setting is told about the
    // setting, rather than about the widening alone -- or the operator fixes one, saves, and only
    // then learns the other was never going to apply.
    auto moved = RunningNode(/*open=*/true);
    moved.slots = 7;
    auto const refused = check(RunningNode(), moved);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context.contains("--slots"));
}

TEST_CASE("Dropping fleet_open closes the node again", "[node][membership][reload][revocation]")
{
    // The narrowing direction: an open node admits every caller there is, so turning it
    // off refuses everybody the roster does not admit. `fleet_open` is a boolean whose
    // key spells the FLAG, so this works
    // because the candidate is built FRESH -- a key that is gone from the file is a
    // flag that is not passed, rather than a value that persists.
    Testing::ScratchDirectory const scratch { "node-reload-close" };
    auto const path = WriteConfig(scratch.Path(), "fleet_open: false\n");

    auto const initial = RunningNode(/*open=*/true);
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    REQUIRE(Admits(oracle, Stranger));

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    NodeConditions conditions;
    ApplyReloadRequest(&reloader, membership, conditions, logger);

    CHECK_FALSE(Admits(oracle, Stranger));
    // And this machine is still admitted, which is the rule that survives every
    // policy: a process on this host already has this host's compiler.
    CHECK(Admits(oracle, "127.0.0.1"));
}

TEST_CASE("A revocation is announced, and a reload that touched nothing is silent", "[node][membership][reload][revocation]")
{
    // The observable half. Turning `--fleet-open` ON announces itself the moment a
    // stranger's build is served; turning it OFF has no such signal, because admission
    // succeeding is the ordinary case -- and it refuses every caller the roster does not
    // admit, a set no list can enumerate. So both directions are said, in words.
    SECTION("closing an open node says what that did")
    {
        auto const said = AdmissionAnnouncement(RunningNode(/*open=*/true), RunningNode());
        REQUIRE(said.has_value());
        CHECK(Testing::Unwrap(said).contains("--fleet-open is off"));
        CHECK(Testing::Unwrap(said).contains("every caller the roster does not admit is now refused"));
    }

    SECTION("opening a node says so too")
    {
        auto const said = AdmissionAnnouncement(RunningNode(), RunningNode(/*open=*/true));
        REQUIRE(said.has_value());
        CHECK(Testing::Unwrap(said).contains("--fleet-open is on"));
        // And the two directions are told apart, or an operator confirming a narrowing
        // reads a widening's line.
        CHECK_FALSE(Testing::Unwrap(said).contains("--fleet-open is off"));
    }

    SECTION("a reload that changed nothing about admission says nothing")
    {
        // A reload is a routine event: a `log_level` change must not narrate an
        // admission policy nobody edited, or the line that MATTERS is the one that
        // gets filtered out.
        auto const before = RunningNode(/*open=*/true);
        auto moved = before;
        moved.logLevel = LogLevel::Debug;
        CHECK_FALSE(AdmissionAnnouncement(before, moved).has_value());
    }
}

TEST_CASE("A reload never revokes what the cluster agreed", "[node][membership][reload]")
{
    // #251 arriving through the door a reload opens. `--fleet-open` and the cluster's
    // agreed keys are two questions with two publishers, and a reload holds the whole
    // truth about exactly one of them. An `Adopt` that rebuilt the composite -- or that
    // republished the key roster -- would discard every machine consensus admitted.
    Testing::ScratchDirectory const scratch { "node-reload-keeps-cluster" };
    auto const path = WriteConfig(scratch.Path(), "");

    auto const initial = RunningNode(/*open=*/true);
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    // A member the cluster admitted by its key, proving it from its own address.
    auto const key = Testing::TestKeyPair("m50").PublicKey();
    auto state = Cluster::ClusterState {};
    Cluster::Apply(state,
                   Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                      .key = "m50",
                                      .value = "10.0.0.50:6676",
                                      .schedulerEndpoint = {},
                                      .publicKey = key });
    membership.PublishCluster(state);
    auto const member = ConnectionFacts { .host = "10.0.0.50", .proven = ProvenIdentity { .id = "m50", .key = key } };
    REQUIRE(Distributed::ExplainConnection(oracle, member).verdict == Distributed::Membership::Member);

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    NodeConditions conditions;
    ApplyReloadRequest(&reloader, membership, conditions, logger);

    // The node closed, and the cluster's member survived it.
    CHECK_FALSE(Admits(oracle, Stranger));
    CHECK(Distributed::ExplainConnection(oracle, member).verdict == Distributed::Membership::Member);
}

TEST_CASE("A reload that gives --advertise clears unqualified-host-name, and one that does not leaves it",
          "[node][reload][conditions][formation][defaults]")
{
    // `--advertise` is reloadable, so whether peers are still told to dial a bare host name
    // is asked again of the configuration an accepted reload puts in force. The candidate
    // carries the names the start resolved, as `main`'s reload lambda hands them on.
    auto const named = [](std::filesystem::path const& path) -> std::expected<NodeConfig, ConfigError> {
        return Reparse(path).transform([](NodeConfig candidate) {
            ApplyHostNames(candidate, NodeHostNames { .fqdn = "laptop", .dnsSuffix = {}, .withheld = {} });
            return candidate;
        });
    };
    Testing::ScratchDirectory const scratch { "node-reload-host-name" };
    auto initial = RunningNode();
    ApplyHostNames(initial, NodeHostNames { .fqdn = "laptop", .dnsSuffix = {}, .withheld = {} });
    NodeMembership membership { initial, membershipLog };
    NullLogger logger;
    NodeConditions conditions;
    EvaluateHostNameCondition(conditions, initial);
    REQUIRE(conditions.StateOf(NodeCondition::UnqualifiedHostName) == Wire::ConditionState::Raised);

    SECTION("a file that still names no --advertise leaves the row raised")
    {
        auto const path = WriteConfig(scratch.Path(), "log_level: info\n");
        NodeReloader reloader { initial, path, named, &ValidateNodeReloadable };
        ApplyReloadRequest(&reloader, membership, conditions, logger);
        CHECK(conditions.StateOf(NodeCondition::UnqualifiedHostName) == Wire::ConditionState::Raised);
    }

    SECTION("a file that names one clears it")
    {
        auto const path = WriteConfig(scratch.Path(), "advertise: 10.0.0.5:6674\n");
        NodeReloader reloader { initial, path, named, &ValidateNodeReloadable };
        ApplyReloadRequest(&reloader, membership, conditions, logger);
        REQUIRE(reloader.Current()->advertise == "10.0.0.5:6674");
        CHECK(conditions.StateOf(NodeCondition::UnqualifiedHostName) == Wire::ConditionState::Clear);
    }
}

TEST_CASE("A reload whose file still lists fleet_member is refused by name, and nothing is applied",
          "[node][membership][reload][admission]")
{
    // The retired key reaching the path a running node reads, rather than the start: a file an
    // operator edited back to its old shape and saved. A key naming no row is refused, so the
    // list is never silently ignored -- and the refusal is the whole-file kind, so the
    // `fleet_open: true` beside it does not slip through either.
    Testing::ScratchDirectory const scratch { "node-reload-fleet-member" };
    auto const path = WriteConfig(scratch.Path(), "fleet_open: true\nfleet_member: 10.0.0.7\n");

    auto const initial = RunningNode();
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    auto const reloaded = reloader.Reload();
    REQUIRE_FALSE(reloaded.has_value());
    CHECK(reloaded.error().code == ConfigErrorCode::UnknownKey);
    CHECK(reloaded.error().field == "fleet_member");

    // Nothing applied: the snapshot is the one the node started with, and a reload request
    // through the production seam leaves the oracle closed.
    CHECK_FALSE(reloader.Current()->fleetOpen);
    NullLogger logger;
    NodeConditions conditions;
    ApplyReloadRequest(&reloader, membership, conditions, logger);
    CHECK_FALSE(Admits(oracle, Stranger));
    CHECK_FALSE(Admits(oracle, "10.0.0.7"));
}
