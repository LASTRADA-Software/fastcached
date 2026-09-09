// SPDX-License-Identifier: Apache-2.0
#include "NodeMembership.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache;

/// `NodeMembership` reports the keyless-widening refusal here; no case asserts on it.
namespace
{
    FastCache::NullLogger membershipLog;
}
using namespace FastCache::Node;
using FastCache::Distributed::Membership;

TEST_CASE("A membership commit does not revoke the hosts an operator listed", "[node][membership]")
{
    // Issue #251, and a WIRING case rather than an oracle one: `AnyOfMembership` and
    // `ClusterMembership` are each asserted where they live. What only this layer can
    // be wrong about is the routing -- `--fleet-member` reaching the list consensus
    // never writes, and `Publish` reaching the other one -- and getting that wrong is
    // what the ticket is.
    //
    // A client machine is not a cluster peer and never will be: a developer's laptop,
    // a CI runner. It reaches this node only because somebody named it. So the first
    // replicated membership commit -- a node joining, a node being forgotten, a
    // settings change, all of them routine -- must not be what takes it away.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetMembers = { "10.0.0.1:6676" };

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.0.0.1") == Membership::Member);

    // Exactly what the observer in `StartConsensusOrExplain` hands over: whatever
    // `ClusterState::Endpoints()` answers, naming nothing the operator typed.
    membership.Publish({ "10.0.0.7:7000" });

    // The listed host is still admitted, and the cluster's peer is admitted without
    // anybody listing it. Both halves of the ticket's acceptance.
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);
    CHECK(membership.Oracle().Classify("10.0.0.7") == Membership::Member);
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);

    // And the limit, which is the state every clustered node passes through: before
    // the first entry naming anybody commits, `Endpoints()` is empty. Read as "the
    // members are: nobody", that empties the node's whole admission policy -- the
    // admission-layer spelling of a mistake consensus already refuses one layer down,
    // where absence from `ClusterState` is not removal.
    membership.Publish({});
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);
    CHECK(membership.Oracle().Classify("10.0.0.7") == Membership::Outsider);
}

TEST_CASE("An open node stays open across a membership commit", "[node][membership]")
{
    // `--fleet-open` says "admit everybody", and a replicated member set narrows
    // nothing an operator has already opened. Asserted rather than assumed, because
    // the oracle an open node hands out is a different object from the one `Publish`
    // writes to -- so a change that made `Publish` swap which oracle is served would
    // silently close a node whose operator had opened it.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    membership.Publish({ "10.0.0.7:7000" });

    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}

TEST_CASE("A forgotten host listed by --fleet-member is STILL admitted, and that is documented",
          "[node][membership][revocation]")
{
    // [#265](https://github.com/LASTRADA-Software/fastcached/issues/265), and this case
    // pins a LIMITATION rather than a fix. Addition is dynamic and removal is not:
    // `AnyOfMembership` answers `Member` when ANY participant says so, and `_listed` is
    // fixed for this process's life, so a host in BOTH lists cannot be revoked while
    // the node runs.
    //
    // **Written down as a test rather than only as prose**, because the case where an
    // operator most wants `--cluster-forget` to bite is a machine they have stopped
    // trusting, and that is precisely the machine most likely to be in both lists. A
    // limitation nothing asserts is one a later change can remove or worsen in silence
    // -- and the direction that matters is *worsen*: a change that made `Publish` write
    // `_listed` would look like it fixed this while discarding every client machine the
    // operator named, which is #251 exactly.
    //
    // The state this asserts is what `docs/tools/fastcache-compile-node.md` states in
    // words. If one moves, the other is wrong.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    // The dangerous arrangement: named by the operator AND agreed by the cluster.
    cfg.fleetMembers = { "10.0.0.1:6676" };

    NodeMembership membership { cfg, membershipLog };
    membership.Publish({ "10.0.0.1:6676", "10.0.0.7:7000" });
    REQUIRE(membership.Oracle().Classify("10.0.0.1") == Membership::Member);

    // The forget: the cluster agrees a member set that no longer names it. This is
    // exactly what the consensus observer hands over after `--cluster-forget` commits.
    membership.Publish({ "10.0.0.7:7000" });

    // **Still a member**, because `--fleet-member` still names it. Not the behaviour an
    // operator expects from a forget, which is why the documentation says so.
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);

    // And the contrast, which is what makes this a statement about the STATIC list
    // rather than about forgets in general: a host the cluster alone admitted IS
    // revoked by the same commit. A case asserting only the line above would pass on a
    // node whose `Publish` did nothing at all.
    membership.Publish({ "10.0.0.7:7000", "10.0.0.8:7000" });
    REQUIRE(membership.Oracle().Classify("10.0.0.8") == Membership::Member);
    membership.Publish({ "10.0.0.7:7000" });
    CHECK(membership.Oracle().Classify("10.0.0.8") == Membership::Outsider);
}

TEST_CASE("Under --fleet-open a forget revokes nothing at all", "[node][membership][revocation]")
{
    // Stated rather than implied, which #265 asks for by name. `--fleet-open` means
    // "admit everybody", so there is no set for a forget to remove anybody from -- and
    // `Publish` deliberately does nothing under it.
    //
    // Correct for the flag, and worth an assertion because it is the one configuration
    // where a forget's failure to bite is not a limitation but the flag working: an
    // operator who wants revocation has to turn `--fleet-open` off, and no amount of
    // work on the revocation path changes that.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;
    cfg.fleetMembers = { "10.0.0.1:6676" };

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    membership.Publish({});
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);
}

/// A cluster state carrying one `fleet-open` value and no members.
/// @param value What the row says, exactly as an operator typed it.
/// @return The state.
[[nodiscard]] static FastCache::Cluster::ClusterState OpenSetTo(std::string value)
{
    FastCache::Cluster::ClusterState state;
    state.settings.push_back({ .name = std::string { FastCache::Cluster::FleetOpenSetting }, .value = std::move(value) });
    return state;
}

TEST_CASE("The cluster's fleet-open row opens a node whose flag did not", "[node][membership]")
{
    // #1112's DISCRIMINATOR: red before, green after. `--cluster-set fleet-open=1`
    // was accepted, replicated, snapshotted and survived restarts while changing no
    // admission decision, because admission read the flag and nothing read the row.
    //
    // What a test asserting the row was ACCEPTED would prove is nothing -- that
    // passes on the broken build, which is the whole complaint. The decision is the
    // subject, so the assertion is a stranger being admitted.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.clusterKeyFile = "/etc/fastcached/cluster.key";
    REQUIRE_FALSE(cfg.fleetOpen);

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);

    membership.PublishCluster(OpenSetTo("1"));

    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}

TEST_CASE("A node with no cluster row keeps its own flag, and an unset row is not a no",
          "[node][membership]")
{
    // The ACCEPT direction, and the case that fails if the fix over-corrects: a fix
    // that made admission read the row and refuse whenever it is absent would pass
    // every refusal case in this file and fail here. `Absence from ClusterState is
    // not removal` is the rule, one layer up from the members it was written for --
    // *nobody has said* and *somebody said no* are different facts, and reading the
    // first as the second closes a node its operator opened with a default nobody
    // chose.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetMembers = { "10.0.0.1:6676" };
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    // A commit that names members and says nothing about openness.
    membership.PublishCluster(FastCache::Cluster::ClusterState {});
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    // And the ordinary policy still works on a closed node: the listed host is
    // admitted and a stranger is not. A fix that read the row and refused everybody
    // is green on any refusal-only assertion and red on this one.
    NodeConfig closed;
    closed.nodeId = "node-b";
    closed.fleetMembers = { "10.0.0.1:6676" };

    NodeMembership shut { closed, membershipLog };
    shut.PublishCluster(FastCache::Cluster::ClusterState {});
    CHECK(shut.Oracle().Classify("10.0.0.1") == Membership::Member);
    CHECK(shut.Oracle().Classify("10.9.9.9") == Membership::Outsider);
}

TEST_CASE("The cluster may CLOSE a node its flag opened", "[node][membership]")
{
    // Narrowing is always safe and always applies, which is what makes the widening
    // guard below an asymmetry rather than a refusal to read the row at all.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    membership.PublishCluster(OpenSetTo("0"));
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);
}

TEST_CASE("A keyless node refuses to be WIDENED by the cluster, and still narrows", "[node][membership]")
{
    // #282 through a door the reload guard cannot watch. `ValidateNodeReloadable`
    // refuses this transition when an OPERATOR acts on the machine; a replicated row
    // does the same with no action on this node at all, so that guard's reasoning
    // applies with more force while none of its code runs.
    //
    // The node stays CLOSED while the cluster says open. That divergence is the
    // point: it fails closed, and it is reported.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetMembers = { "10.0.0.1:6676" };
    REQUIRE(cfg.clusterKeyFile.empty());
    REQUIRE_FALSE(cfg.fleetOpen);

    NodeMembership membership { cfg, membershipLog };
    membership.PublishCluster(OpenSetTo("1"));

    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);

    // Asked as a TRANSITION, so a keyless node its operator already opened is
    // running happily today and stays open -- refusing that would break the nodes
    // this guard exists to protect.
    NodeConfig opened;
    opened.nodeId = "node-b";
    opened.fleetOpen = true;
    REQUIRE(opened.clusterKeyFile.empty());

    NodeMembership already { opened, membershipLog };
    already.PublishCluster(OpenSetTo("1"));
    CHECK(already.Oracle().Classify("10.9.9.9") == Membership::Member);

    // And narrowing still reaches a keyless node: only the widening is refused.
    NodeMembership shut { opened, membershipLog };
    shut.PublishCluster(OpenSetTo("0"));
    CHECK(shut.Oracle().Classify("10.9.9.9") == Membership::Outsider);
}

TEST_CASE("A row value this build cannot read falls back to the flag", "[node][membership]")
{
    // `Validate` refuses such a value on the leader before the append, so reaching
    // this needs a NEWER build with a wider grammar mid rolling upgrade. Guessing
    // `open` would widen admission on a typo; guessing `closed` would override a
    // local flag. Absence is the only answer that does neither.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;
    cfg.clusterKeyFile = "/etc/fastcached/cluster.key";

    NodeMembership membership { cfg, membershipLog };
    membership.PublishCluster(OpenSetTo("yes"));
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}
