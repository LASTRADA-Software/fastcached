// SPDX-License-Identifier: Apache-2.0
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

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

// **The removal direction is the test** (#405).
//
// `--fleet-member` and `--fleet-open` are the settings a fleet actually edits, and
// their two directions are not symmetric. A member ADDED to the file and not yet
// admitted fails CLOSED: the machine is refused until somebody restarts the node,
// which is annoying, self-healing and visible from the machine being refused. A member
// REMOVED from the file and still admitted fails OPEN: a machine the operator has just
// revoked keeps being served, and nothing reports it, because admission succeeding is
// the ordinary case.
//
// So the addition case is here only to stop a change that satisfies the easy half from
// passing, and every other case is about narrowing.

using namespace FastCache;

/// `NodeMembership` reports the keyless-widening refusal here; no case asserts on it.
namespace
{
    FastCache::NullLogger membershipLog;
}
using namespace FastCache::Node;

namespace
{

/// A node registering with a scheduler on its own machine.
///
/// Loopback deliberately: a REMOTE scheduler plus a membership policy is what the
/// three `--advertise` reachability rows are about, so a fixture that named one would
/// have every reload below refused for a reason none of them is testing.
constexpr std::string_view SelfScheduler = "127.0.0.1:6675";

/// A peer that is not this machine. `ClusterMembership` answers `Member` for the whole
/// of `127.0.0.0/8` before it reads the list, so a loopback caller could never show
/// that a list decides anything.
constexpr std::string_view Revoked = "10.0.0.7";
constexpr std::string_view Stranger = "10.0.0.99";

/// Write @p body to a configuration file this case owns.
/// @param dir Scratch directory.
/// @param body The keys this case is about; the scheduler line is added.
/// @return The path written.
[[nodiscard]] std::filesystem::path WriteConfig(std::filesystem::path const& dir, std::string_view body)
{
    std::filesystem::create_directories(dir);
    auto const path = dir / "node.yaml";
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out << std::format("scheduler: {}\n{}", SelfScheduler, body);
    return path;
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
/// @param members What `--fleet-member` named at startup.
/// @param open Whether `--fleet-open` was given.
/// @return The live configuration.
[[nodiscard]] NodeConfig RunningNode(std::vector<std::string> members = {}, bool open = false)
{
    NodeConfig cfg;
    cfg.scheduler = std::string { SelfScheduler };
    cfg.fleetMembers = std::move(members);
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

TEST_CASE("A member removed from the file is refused after the reload", "[node][membership][reload][revocation]")
{
    // **The acceptance clause, and the direction nothing would otherwise report.**
    //
    // Asserted on the CLASSIFICATION rather than on the reload's outcome or on the
    // snapshot's field. A reload that is accepted and publishes a configuration the
    // running oracle never hears about is exactly the defect: `reloader.Current()`
    // would agree with the file perfectly while the compile port went on serving the
    // revoked machine.
    Testing::ScratchDirectory const scratch { "node-reload-revoke" };
    auto const path = WriteConfig(scratch.Path(), "fleet_member: 10.0.0.8\n");

    auto const initial = RunningNode({ std::string { Revoked }, "10.0.0.8" });
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    REQUIRE(Admits(oracle, Revoked));

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ApplyReloadRequest(&reloader, membership, logger);

    CHECK_FALSE(Admits(oracle, Revoked));
    // The control, and it is not decoration: an `Adopt` that published an EMPTY list
    // would satisfy the assertion above while revoking the whole fleet, which is a
    // different defect with the same green.
    CHECK(Admits(oracle, "10.0.0.8"));
}

TEST_CASE("A member added to the file is admitted after the reload", "[node][membership][reload]")
{
    // The easy half, asserted separately and on purpose: a change that only ever
    // EXTENDS the list -- which is what an implementation reaching for
    // `ClusterMembership`'s additive publisher by mistake would produce -- passes this
    // and fails the case above. Neither case alone says the list was replaced.
    //
    // It starts from a node that ALREADY admits a remote peer, and that is not
    // incidental: a keyless node going from admitting nobody to admitting somebody is
    // refused by the guard two cases down, so a fixture that widened from nothing
    // would fail here for a reason this case is not about. Adding a SECOND host to a
    // policy that already reaches the network changes nothing about what this node can
    // verify.
    Testing::ScratchDirectory const scratch { "node-reload-admit" };
    auto const path = WriteConfig(scratch.Path(), std::format("fleet_member: 10.0.0.8\nfleet_member: {}\n", Revoked));

    auto const initial = RunningNode({ "10.0.0.8" });
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    REQUIRE_FALSE(Admits(oracle, Revoked));
    REQUIRE(Admits(oracle, "10.0.0.8"));

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ApplyReloadRequest(&reloader, membership, logger);

    CHECK(Admits(oracle, Revoked));
    CHECK(Admits(oracle, "10.0.0.8"));
}

TEST_CASE("Dropping fleet_open closes the node again", "[node][membership][reload][revocation]")
{
    // The narrowing direction of the OTHER flag, and it needs no list at all: an open
    // node admits every caller there is, so turning it off revokes everybody nobody
    // listed. `fleet_open` is a boolean whose key spells the FLAG, so this works
    // because the candidate is built FRESH -- a key that is gone from the file is a
    // flag that is not passed, rather than a value that persists.
    Testing::ScratchDirectory const scratch { "node-reload-close" };
    auto const path = WriteConfig(scratch.Path(), "fleet_open: false\n");

    auto const initial = RunningNode({}, /*open=*/true);
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    REQUIRE(Admits(oracle, Stranger));

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ApplyReloadRequest(&reloader, membership, logger);

    CHECK_FALSE(Admits(oracle, Stranger));
    // And this machine is still admitted, which is the rule that survives every
    // policy: a process on this host already has this host's compiler.
    CHECK(Admits(oracle, "127.0.0.1"));
}

TEST_CASE("A reload may not widen admission on a node that cannot check a lease", "[node][membership][reload][revocation]")
{
    // **The hole this ticket would otherwise open, which is #282 arriving through a
    // new door.** A keyless worker is legitimate for one shape of node: one no other
    // machine can dial. `StartupPolicyRejection` decides that from the listen flags,
    // which describe nothing under socket activation, and `MakeWorkerLeaseValidator`
    // is the backstop for that -- and it runs once, at startup, against the
    // configuration the process started with. Neither can see a reload that widens.
    //
    // Asserted on the REFUSAL and on the oracle together, because "the reload was
    // declined" and "nothing took effect" are two claims and a reload is all-or-nothing
    // only if both hold.
    Testing::ScratchDirectory const scratch { "node-reload-keyless-widen" };
    auto const path = WriteConfig(scratch.Path(), std::format("fleet_member: {}\n", Revoked));

    auto const initial = RunningNode();
    REQUIRE(initial.clusterKeyFile.empty());
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();

    std::ostringstream sink;
    ConsoleLogger logger { sink, LogLevel::Info, LogTimestamps::No };

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ApplyReloadRequest(&reloader, membership, logger);

    CHECK_FALSE(Admits(oracle, Revoked));
    // **WHICH refusal, not that one happened.** The startup table's cluster-key row and
    // this guard both name `--cluster-key-file`, so matching that string alone passes
    // for either -- and on a loopback-bound node the startup row does not fire, which is
    // exactly the gap this rule exists to cover. `may not widen` is this rule's own
    // words; the case below asserts the other side of the same distinction.
    CHECK(sink.str().contains("may not widen"));
    CHECK(reloader.Current()->fleetMembers.empty());
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
    // hypothetical here -- the first version of this case used a `--raft-peer` candidate
    // and passed under both orderings, because `StartupPolicyRejection` refuses that one
    // before either check runs. It was the neuter that said so, not the reading.
    Testing::ScratchDirectory const scratch { "node-reload-widen-diagnosis" };
    auto const path = WriteConfig(scratch.Path(), std::format("slots: 7\nfleet_member: {}\n", Revoked));

    auto const initial = RunningNode();
    auto const candidate = Reparse(path);
    REQUIRE(candidate.has_value());

    // The premise, stated so a fixture that stopped exercising the guard says so rather
    // than passing quietly: this save BOTH widens -- which needs the previous
    // configuration to admit nobody remote, or the widening rule cannot fire at all and
    // the case proves nothing -- and moves an unreloadable row.
    REQUIRE_FALSE(AdmitsRemotePeers(initial));
    REQUIRE(AdmitsRemotePeers(*candidate));
    REQUIRE(candidate->slots != initial.slots);

    auto const outcome = ValidateNodeReloadable(initial, *candidate);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().context.contains("--slots"));
}

TEST_CASE("Narrowing is allowed on a keyless node, which is the direction that closes it",
          "[node][membership][reload][revocation]")
{
    // The control for the case above, and the reason it asks about a WIDENING rather
    // than about a state. A keyless worker that already admits remote peers passed its
    // own startup rules and is running today; refusing its reloads would refuse the
    // one edit that makes it safer, which is a guard that punishes the remedy.
    Testing::ScratchDirectory const scratch { "node-reload-keyless-narrow" };
    auto const path = WriteConfig(scratch.Path(), "");

    auto const initial = RunningNode({ std::string { Revoked } });
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    REQUIRE(Admits(oracle, Revoked));

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ApplyReloadRequest(&reloader, membership, logger);

    CHECK_FALSE(Admits(oracle, Revoked));
}

TEST_CASE("A revocation is announced, and a reload that touched nothing is silent", "[node][membership][reload][revocation]")
{
    // The observable half. An operator who ADDS a member finds out it worked the
    // moment that machine's build is distributed; one who REVOKES has no such signal,
    // because admission succeeding is the ordinary case. So the revoked hosts are
    // named individually -- what a reader is checking is the list they meant to
    // shorten.
    auto const before = RunningNode({ std::string { Revoked }, "10.0.0.8" });

    SECTION("a dropped host is named")
    {
        auto const after = RunningNode({ "10.0.0.8" });
        auto const said = AdmissionAnnouncement(before, after);
        REQUIRE(said.has_value());
        auto const line = Testing::Unwrap(said);
        CHECK(line.contains(Revoked));
        // And the one still listed is NOT named as dropped, or the line an operator
        // reads to confirm a revocation reports two and means one.
        CHECK_FALSE(line.contains("no longer admitted: 10.0.0.7, 10.0.0.8"));
    }

    SECTION("closing an open node says what that did")
    {
        auto const openBefore = RunningNode({}, /*open=*/true);
        auto const said = AdmissionAnnouncement(openBefore, RunningNode());
        REQUIRE(said.has_value());
        // No list can enumerate "everybody who was not on a list", so this case says
        // what happened instead of naming nobody and reading as a no-op.
        CHECK(Testing::Unwrap(said).contains("--fleet-open is off"));
    }

    SECTION("a reload that changed neither flag says nothing")
    {
        // A reload is a routine event: a `log_level` change must not narrate an
        // admission policy nobody edited, or the line that MATTERS is the one that
        // gets filtered out.
        auto moved = before;
        moved.logLevel = LogLevel::Debug;
        CHECK_FALSE(AdmissionAnnouncement(before, moved).has_value());
    }

    SECTION("reordering the list is not a revocation")
    {
        auto const reordered = RunningNode({ "10.0.0.8", std::string { Revoked } });
        auto const said = AdmissionAnnouncement(before, reordered);
        // Either answer is legitimate -- a reorder is a change to the value and saying
        // so is not wrong -- but neither may claim a host was revoked.
        if (said.has_value())
            CHECK_FALSE(Testing::Unwrap(said).contains("no longer admitted"));
    }
}

TEST_CASE("A reload never revokes what the cluster agreed", "[node][membership][reload]")
{
    // #251 arriving through the door this ticket opens. `--fleet-member` and the
    // cluster's agreed set are two questions with two publishers, and a reload holds
    // the whole truth about exactly one of them. An `Adopt` that wrote the composite
    // -- or that published into `_cluster` -- would discard every host consensus
    // admitted, which is the defect that made the two lists separate in the first
    // place, reached from the other side.
    Testing::ScratchDirectory const scratch { "node-reload-keeps-cluster" };
    auto const path = WriteConfig(scratch.Path(), "");

    auto const initial = RunningNode({ std::string { Revoked } });
    NodeMembership membership { initial, membershipLog };
    // Bound ONCE, as a surface does at startup.
    auto const& oracle = membership.Oracle();
    NullLogger logger;

    membership.Publish({ "10.0.0.50:6676" });
    REQUIRE(Admits(oracle, "10.0.0.50"));

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ApplyReloadRequest(&reloader, membership, logger);

    // The operator's list emptied, and the cluster's survived it.
    CHECK_FALSE(Admits(oracle, Revoked));
    CHECK(Admits(oracle, "10.0.0.50"));
}

TEST_CASE("A node that binds its own network-facing port is closed by the STARTUP rule",
          "[node][membership][reload][revocation]")
{
    // **What decides how severe the widening hole is, pinned rather than reasoned.**
    //
    // The guard above is a backstop for a node whose configuration does not describe
    // its own port -- which is socket activation, where the unit chose the address and
    // `--listen-node` keeps whatever was typed. The obvious worry is that a node which
    // binds its own network-facing port is exposed too, and it is NOT: `StartupPolicyRejection`
    // is re-run on the candidate by `ValidateNodeReloadable`, `CompilePortFacesTheNetwork`
    // tells the truth for a self-binding node, and the pre-existing cluster-key row
    // refuses the widening before the backstop is ever consulted.
    //
    // So this case is what makes "socket activation is a necessary condition" a measured
    // claim instead of a reading, and it is asserted on WHICH rule answered -- both
    // messages name `--cluster-key-file`, so matching that alone cannot tell them apart.
    // It also guards a dependency across lanes: the severity stated in #405's commit
    // body is only true while that startup row keeps catching this, so a change to it
    // fails here rather than silently widening what this ticket left open.
    //
    // `--advertise` is named because otherwise the reachability rows answer first -- a
    // membership policy plus a wildcard advertise is what they are about, and the case
    // would then pass for a reason that has nothing to do with keys.
    Testing::ScratchDirectory const scratch { "node-reload-self-bound" };
    auto const path = WriteConfig(scratch.Path(),
                                  std::format("listen_node: 0.0.0.0:6674\n"
                                              "advertise: worker-01.internal:6674\n"
                                              "fleet_member: {}\n",
                                              Revoked));

    auto initial = RunningNode();
    initial.nodeListen = "0.0.0.0:6674";
    initial.advertise = "worker-01.internal:6674";
    REQUIRE(initial.clusterKeyFile.empty());

    auto const candidate = Reparse(path);
    REQUIRE(candidate.has_value());
    // The premise: this really is the widening shape, on a port that really does face
    // the network. Without both, the case would pass having exercised nothing.
    REQUIRE(AdmitsRemotePeers(*candidate));
    REQUIRE_FALSE(AdmitsRemotePeers(initial));
    // The port is stated rather than asked about: `CompilePortFacesTheNetwork` is
    // private to `NodeConfig.cpp`, and a copy of it here would be a second reader of
    // the predicate under test. The refusal below is the evidence that it answered
    // true, because that startup row cannot fire on any other reading of this bind.
    REQUIRE(candidate->nodeListen == "0.0.0.0:6674");

    auto const outcome = ValidateNodeReloadable(initial, *candidate);
    REQUIRE_FALSE(outcome.has_value());
    // The STARTUP row's own words, and NOT the reload guard's, which says "may not
    // widen". Which one answers is the whole finding.
    CHECK(outcome.error().context.contains("needs --cluster-key-file"));
    CHECK_FALSE(outcome.error().context.contains("may not widen"));
}
