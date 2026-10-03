// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "NodeFormation.hpp"
#include "NodeReload.hpp"

#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

/// A node registering with a scheduler on its own machine.
///
/// Loopback deliberately: a REMOTE scheduler plus an admission route -- which a worker's
/// state directory always is -- is what the `--advertise` reachability rows are about,
/// so a fixture that named one would have every reload below refused for a reason none
/// of them is testing.
constexpr std::string_view SelfScheduler = "127.0.0.1:6675";

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
/// The worker these cases are about runs NO consensus -- the kind a reload may not widen
/// admission on without a `--voter-key` -- so the file says so: consensus is on by default,
/// and an empty `listen_raft` is how a file turns it off.
/// @param dir Scratch directory.
/// @param body The keys this case is about; the scheduler, state-directory and consensus lines are added.
/// @return The path written.
[[nodiscard]] std::filesystem::path WriteConfig(std::filesystem::path const& dir, std::string_view body)
{
    return WriteFile(
        dir, std::format("scheduler: {}\ncluster_dir: {}\nlisten_raft: \"\"\n{}", SelfScheduler, StateDirectory, body));
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
    cfg.schedulers = { std::string { SelfScheduler } };
    cfg.clusterDir = StateDirectory;
    cfg.raftListen.clear(); // as `WriteConfig`'s file says
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
    auto const path =
        WriteFile(scratch.Path(),
                  std::format("scheduler: {}\ncluster_dir: {}\n", SelfScheduler, (scratch / "state").generic_string()));
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
            .formation = record,
            .remembered = {},
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

TEST_CASE("A reload may not widen admission on a node that cannot check a lease", "[node][membership][reload][revocation]")
{
    // **The hole this ticket would otherwise open, which is #282 arriving through a
    // new door.** A worker with no roster is legitimate for one shape of node: one no other
    // machine can dial. `StartupPolicyRejection` decides that from the listen flags,
    // which describe nothing under socket activation, and `MakeWorkerLeaseValidator`
    // is the backstop for that -- and it runs once, at startup, against the
    // configuration the process started with. Neither can see a reload that widens.
    //
    // Asserted on the REFUSAL and on the oracle together, because "the reload was
    // declined" and "nothing took effect" are two claims and a reload is all-or-nothing
    // only if both hold.
    Testing::ScratchDirectory const scratch { "node-reload-keyless-widen" };
    auto const path = WriteConfig(scratch.Path(), "fleet_open: true\n");

    auto const initial = RunningNode();
    REQUIRE(initial.voterKeys.empty()); // no roster root (#178)
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();

    std::ostringstream sink;
    ConsoleLogger logger { sink, LogLevel::Info, LogTimestamps::No };

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    NodeConditions conditions;
    ApplyReloadRequest(&reloader, membership, conditions, logger);

    CHECK_FALSE(Admits(oracle, Stranger));
    // **WHICH refusal, not that one happened.** The startup table's lease row and this
    // guard both name `--voter-key` (#178), so matching that string alone passes
    // for either -- and on a loopback-bound node the startup row does not fire, which is
    // exactly the gap this rule exists to cover. `may not widen` is this rule's own
    // words; the case below asserts the other side of the same distinction.
    CHECK(sink.str().contains("may not widen"));
    CHECK_FALSE(reloader.Current()->fleetOpen);
}

TEST_CASE("A node running no worker may widen admission without a key", "[node][membership][reload][revocation]")
{
    // The case above guards the lease check a WORKER chose at startup, and a node started
    // with `--slots=0` chose none: it builds no validator and serves no compile verb, so
    // admitting a remote peer opens no compile port (#206). Refusing it would turn away a
    // keyless scheduling or caching machine for a reason about a worker it does not run.
    //
    // No `scheduler:` line, because a node running no worker registers nowhere.
    Testing::ScratchDirectory const scratch { "node-reload-no-worker-widen" };
    auto const path = WriteFile(scratch.Path(), "slots: 0\nfleet_open: true\n");

    NodeConfig initial;
    initial.slots = 0;
    REQUIRE(initial.voterKeys.empty()); // no roster root (#178)
    REQUIRE_FALSE(RunsWorker(initial));

    auto const candidate = Reparse(path);
    REQUIRE(candidate.has_value());
    // The premise: the widening shape the case above refuses on a worker.
    REQUIRE(AdmitsRemotePeers(*candidate, RosterPresence::Absent));
    REQUIRE_FALSE(AdmitsRemotePeers(initial, RosterPresence::Absent));

    auto const outcome = ValidateNodeReloadable(initial, *candidate);
    INFO((outcome.has_value() ? std::string {} : outcome.error().context));
    CHECK(outcome.has_value());
}

TEST_CASE("A widening refusal names every setting that may not change", "[node][membership][reload][revocation]")
{
    // **The ordering the guard depends on, asserted rather than assumed.**
    //
    // A reload names EVERY unreloadable setting that changed, because a refusal that
    // reports one and stops sends the operator round the same loop per field. The
    // widening guard is a second refusal reachable from the same save, so if it is asked
    // first it answers instead of that whole list -- the operator fixes the widening,
    // saves again, and only then learns that `--slots` was never going to apply either.
    //
    // Asserted on WHICH refusal, never on the fact of one: BOTH orderings refuse this
    // save, so a case checking `has_value()` alone passes under the defect. That is not
    // hypothetical here -- the first version of this case used a peer-list candidate
    // and passed under both orderings, because `StartupPolicyRejection` refuses that one
    // before either check runs. It was the neuter that said so, not the reading.
    Testing::ScratchDirectory const scratch { "node-reload-widen-diagnosis" };
    auto const path = WriteConfig(scratch.Path(), "slots: 7\nfleet_open: true\n");

    auto const initial = RunningNode();
    auto const candidate = Reparse(path);
    REQUIRE(candidate.has_value());

    // The premise, stated so a fixture that stopped exercising the guard says so rather
    // than passing quietly: this save BOTH widens -- which needs the previous
    // configuration to admit nobody remote, or the widening rule cannot fire at all and
    // the case proves nothing -- and moves an unreloadable row.
    REQUIRE_FALSE(AdmitsRemotePeers(initial, RosterPresence::Absent));
    REQUIRE(AdmitsRemotePeers(*candidate, RosterPresence::Absent));
    REQUIRE(candidate->slots != initial.slots);

    auto const outcome = ValidateNodeReloadable(initial, *candidate);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().context.contains("--slots"));
}

TEST_CASE("Narrowing is allowed on a keyless node, which is the direction that closes it",
          "[node][membership][reload][revocation]")
{
    // The control for the case above, and the reason it asks about a WIDENING rather
    // than about a state. A rosterless worker that already admits remote peers passed its
    // own startup rules and is running today; refusing its reloads would refuse the
    // one edit that makes it safer, which is a guard that punishes the remedy.
    Testing::ScratchDirectory const scratch { "node-reload-keyless-narrow" };
    auto const path = WriteConfig(scratch.Path(), "");

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
                                      .publicKey = key,
                                      .role = std::nullopt });
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

TEST_CASE("A node that binds its own network-facing port is closed by the reload guard too",
          "[node][membership][reload][revocation]")
{
    // **What decides how severe the widening hole is, pinned rather than reasoned.**
    //
    // The guard above was written as a backstop for a node whose configuration does not
    // describe its own port -- socket activation, where the unit chose the address and
    // `--listen-node` keeps whatever was typed. Until #178 PR 6 a node that binds its OWN
    // network-facing port never reached it: `StartupPolicyRejection` is re-run on the candidate
    // by `ValidateNodeReloadable`, and a startup row refused any worker other machines could
    // reach with no roster to check against. That row went when every worker came to keep a
    // state directory -- a node naming a scheduler must hold an identity -- because a directory
    // may hold a roster no configuration can see, so the table cannot refuse it.
    //
    // So the guard is now what closes this for EVERY worker, and this case pins that: the
    // widening is refused, and refused by the guard's own words. A change that let it through
    // would open an unauthenticated compile port with every refusal counter reading zero.
    //
    // `--advertise` is named because otherwise the reachability rows answer first -- an
    // admission route plus a wildcard advertise is what they are about, and the case
    // would then pass for a reason that has nothing to do with keys.
    Testing::ScratchDirectory const scratch { "node-reload-self-bound" };
    auto const path = WriteConfig(scratch.Path(),
                                  "listen_node: 0.0.0.0:6674\n"
                                  "advertise: worker-01.internal:6674\n"
                                  "fleet_open: true\n");

    auto initial = RunningNode();
    initial.nodeListen = "0.0.0.0:6674";
    initial.advertise = "worker-01.internal:6674";
    REQUIRE(initial.voterKeys.empty()); // no roster root the configuration can see (#178)

    auto const candidate = Reparse(path);
    REQUIRE(candidate.has_value());
    // The premise: this really is the widening shape, on a port that really does face
    // the network. Without both, the case would pass having exercised nothing.
    REQUIRE(AdmitsRemotePeers(*candidate, RosterPresence::Absent));
    REQUIRE_FALSE(AdmitsRemotePeers(initial, RosterPresence::Absent));
    REQUIRE(candidate->nodeListen == "0.0.0.0:6674");
    // And the startup table lets the candidate through, which is what leaves the guard as the
    // only thing standing: it names a state directory, which might hold a roster.
    REQUIRE_FALSE(StartupPolicyRejection(*candidate).has_value());

    auto const outcome = ValidateNodeReloadable(initial, *candidate);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().context.contains("may not widen"));

    // The control: a worker that names the voters has a roster root the configuration CAN see,
    // so the same widening is taken.
    auto anchored = initial;
    anchored.voterKeys = { Testing::TestKeyPair("scheduler").PublicKey() };
    auto anchoredCandidate = *candidate;
    anchoredCandidate.voterKeys = anchored.voterKeys;
    CHECK(ValidateNodeReloadable(anchored, anchoredCandidate).has_value());
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
