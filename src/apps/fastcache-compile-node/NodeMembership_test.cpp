// SPDX-License-Identifier: Apache-2.0
#include "NodeMembership.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache;
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

    NodeMembership membership { cfg };
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

    NodeMembership membership { cfg };
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

    NodeMembership membership { cfg };
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

    NodeMembership membership { cfg };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    membership.Publish({});
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);
}
