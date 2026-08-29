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
