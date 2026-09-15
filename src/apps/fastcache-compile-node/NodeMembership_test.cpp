// SPDX-License-Identifier: Apache-2.0
#include "NodeMembership.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache;

/// `NodeMembership` reports an unreadable `fleet-open` row here; no case asserts on it.
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

TEST_CASE("A node with no cluster row keeps its own flag, and an unset row is not a no", "[node][membership]")
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
    // Narrowing is always safe and always applies.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    membership.PublishCluster(OpenSetTo("0"));
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);
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

TEST_CASE("A cluster forget outranks the host an operator listed", "[node][membership][forget]")
{
    // #1309, and the direction that fails OPEN: a client is removed by editing
    // `--fleet-member` on every other machine in the fleet, and missing one leaves it
    // serving a retired host indefinitely, with admission succeeding being the ordinary
    // case and nothing to report.
    //
    // A WIRING case, as the #251 one above is: the oracles are asserted where they
    // live. What only this layer can be wrong about is whether the tombstones reach the
    // participants at all -- and `_forgotten` being published from `PublishCluster` and
    // never from `Adopt` is exactly the kind of routing that compiles either way.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetMembers = { "10.0.0.1:6676", "10.0.0.2:6676" };

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.0.0.1") == Membership::Member);

    FastCache::Cluster::ClusterState state;
    state.forgotten = { "10.0.0.1" };
    membership.PublishCluster(state);

    // The listed host is now refused, and refused AS FORGOTTEN -- not as an outsider,
    // which is what the old `any_of` fold answered and what makes the refusal
    // indistinguishable from a stranger's on every surface.
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Forgotten);

    // The control, and it is what makes the line above mean anything: the other listed
    // host is untouched. A `PublishCluster` that had emptied the operator's list would
    // pass the assertion above and fail this one.
    CHECK(membership.Oracle().Classify("10.0.0.2") == Membership::Member);

    // And a host nobody listed is still a stranger rather than a forget: the two
    // refusals must stay distinguishable in both directions.
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);
}

TEST_CASE("A re-admit is what lifts a forget, and a reload is not", "[node][membership][forget]")
{
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetMembers = { "10.0.0.1:6676" };

    NodeMembership membership { cfg, membershipLog };

    FastCache::Cluster::ClusterState state;
    state.forgotten = { "10.0.0.1" };
    membership.PublishCluster(state);
    REQUIRE(membership.Oracle().Classify("10.0.0.1") == Membership::Forgotten);

    // A reload must NOT lift it. `Adopt` writes `--fleet-member`'s list and only that,
    // and a host named there is exactly the host a forget is about -- so an
    // implementation that rebuilt the participants from the configuration would erase
    // every tombstone agreed since startup, on the first SIGHUP, and the fleet would
    // start serving the decommissioned machine again with nothing to say why.
    membership.Adopt(cfg);
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Forgotten);

    // What does lift it is the cluster agreeing to admit the host again, which reaches
    // here as a committed state without the tombstone.
    state.forgotten = {};
    membership.PublishCluster(state);
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);
}

TEST_CASE("A forget reaches an open node too", "[node][membership][forget]")
{
    // The decision `--fleet-open` forces, and the one door a forget could have been
    // left out of. The flag says "I have not enumerated who may use this fleet" -- a
    // blanket over hosts nobody named. A forget names one. Letting the blanket win
    // would make a local flag resurrect a machine the cluster positively removed, on
    // exactly the node nobody has reconfigured yet.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };

    // The control first, because the whole case rests on this node being open at all: a
    // fixture that had failed to open would report `Forgotten` below for the ordinary
    // reason and prove nothing.
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    FastCache::Cluster::ClusterState state;
    state.forgotten = { "10.0.0.1" };
    membership.PublishCluster(state);

    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Forgotten);

    // And the blanket still covers everybody else, so the forget narrowed exactly one
    // host rather than closing the node.
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}
