// SPDX-License-Identifier: Apache-2.0
#include "NodeMembership.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;

/// `NodeMembership` reports an unreadable `fleet-open` row here; no case asserts on it.
namespace
{
FastCache::NullLogger membershipLog;
}
using namespace FastCache::Node;
using FastCache::Distributed::Membership;

namespace
{
/// The identity @p id proves on a connection: its test key.
/// @param id The machine.
/// @return What the connection proved.
[[nodiscard]] FastCache::ProvenIdentity Proving(std::string const& id)
{
    return FastCache::ProvenIdentity { .id = id, .key = FastCache::Testing::TestKeyPair(id).PublicKey() };
}

/// The verdict @p membership reaches about one connection: an address, and what it proved.
/// @param membership The node's oracle.
/// @param host Where the connection came from.
/// @param proven What it proved, if anything.
/// @return The folded verdict, as every surface's gate reaches it.
[[nodiscard]] Membership VerdictOf(NodeMembership const& membership,
                                   std::string const& host,
                                   std::optional<FastCache::ProvenIdentity> const& proven)
{
    return FastCache::Distributed::ExplainConnection(membership.Oracle(),
                                                     FastCache::ConnectionFacts { .host = host, .proven = proven })
        .verdict;
}

/// Apply a committed `--cluster-forget=<id>` to @p state, as consensus would.
/// @param state The state.
/// @param id The machine forgotten.
void Forget(FastCache::Cluster::ClusterState& state, std::string const& id)
{
    FastCache::Cluster::Apply(state,
                              FastCache::Cluster::Command { .kind = FastCache::Cluster::CommandKind::Forget,
                                                            .key = id,
                                                            .value = {},
                                                            .schedulerEndpoint = {},
                                                            .publicKey = std::nullopt,
                                                            .role = std::nullopt });
}

/// Apply a committed `--cluster-admit=<id>=<endpoint>@<key>` to @p state, as consensus would.
/// @param state The state.
/// @param id The machine.
/// @param endpoint Its consensus endpoint.
/// @param key The key it is admitted under.
void Admit(FastCache::Cluster::ClusterState& state,
           std::string const& id,
           std::string const& endpoint,
           FastCache::Ed25519PublicKey const& key)
{
    FastCache::Cluster::Apply(state,
                              FastCache::Cluster::Command { .kind = FastCache::Cluster::CommandKind::AddMember,
                                                            .key = id,
                                                            .value = endpoint,
                                                            .schedulerEndpoint = {},
                                                            .publicKey = key,
                                                            .role = std::nullopt });
}
} // namespace

TEST_CASE("An open node stays open across a membership commit", "[node][membership]")
{
    // `--fleet-open` says "admit everybody", and a committed member set narrows nothing an
    // operator has already opened. Asserted rather than assumed, because the open fold and the
    // closed one are different objects -- so a commit that swapped which one is served would
    // silently close a node whose operator had opened it.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    auto state = FastCache::Cluster::ClusterState {};
    Admit(state, "m1", "10.0.0.7:7000", Proving("m1").key);
    membership.PublishCluster(state);

    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}

TEST_CASE("Under --fleet-open a member set narrows nothing, and a revoked key is still refused",
          "[node][membership][revocation]")
{
    // `--fleet-open` means "admit everybody by address", so an empty committed state removes
    // nobody. What it does NOT mean is "admit a machine the cluster forgot": a forget revokes the
    // machine's KEY, and a revoked key outranks the flag (#1309, #178), so the one configuration
    // where a forget's failure to bite used to be the flag working is not one any more.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    membership.PublishCluster(FastCache::Cluster::ClusterState {});
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
    CHECK(membership.Oracle().Classify("10.0.0.1") == Membership::Member);

    auto state = FastCache::Cluster::ClusterState {};
    Admit(state, "gone", "10.0.0.1:6676", Proving("gone").key);
    Forget(state, "gone");
    membership.PublishCluster(state);
    CHECK(VerdictOf(membership, "10.0.0.1", Proving("gone")) == Membership::Forgotten);
    CHECK(VerdictOf(membership, "10.0.0.1", std::nullopt) == Membership::Member);
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
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    // A commit that names members and says nothing about openness.
    membership.PublishCluster(FastCache::Cluster::ClusterState {});
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    // And the ordinary policy still works on a closed node: this machine is admitted
    // and a stranger is not. A fix that read the row and refused everybody is green on
    // any refusal-only assertion and red on this one.
    NodeConfig closed;
    closed.nodeId = "node-b";

    NodeMembership shut { closed, membershipLog };
    shut.PublishCluster(FastCache::Cluster::ClusterState {});
    CHECK(shut.Oracle().Classify("127.0.0.1") == Membership::Member);
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

    NodeMembership membership { cfg, membershipLog };
    membership.PublishCluster(OpenSetTo("yes"));
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}

TEST_CASE("A cluster forget outranks the cluster's own member set, by the key it revoked", "[node][membership][forget]")
{
    // #1309, and the direction that fails OPEN: missing one node leaves it serving a retired
    // machine indefinitely, with admission succeeding being the ordinary case and nothing to
    // report.
    //
    // A WIRING case: the oracles are asserted where they live. What only this layer can be
    // wrong about is whether the revocation reaches the participants at all -- and the key
    // roster being published from `PublishCluster` and never from `Adopt` is exactly the kind
    // of routing that compiles either way.
    NodeConfig cfg;
    cfg.nodeId = "node-a";

    NodeMembership membership { cfg, membershipLog };
    auto state = FastCache::Cluster::ClusterState {};
    Admit(state, "m1", "10.0.0.1:6676", Proving("m1").key);
    Admit(state, "m2", "10.0.0.2:6676", Proving("m2").key);
    membership.PublishCluster(state);
    REQUIRE(VerdictOf(membership, "10.0.0.1", Proving("m1")) == Membership::Member);

    Forget(state, "m1");
    membership.PublishCluster(state);

    // The machine is now refused, and refused AS FORGOTTEN -- not as an outsider, which is
    // what makes the refusal indistinguishable from a stranger's on every surface -- and from
    // any address, since what was revoked is its key.
    CHECK(VerdictOf(membership, "10.0.0.1", Proving("m1")) == Membership::Forgotten);
    CHECK(VerdictOf(membership, "10.7.7.7", Proving("m1")) == Membership::Forgotten);

    // The control, and it is what makes the line above mean anything: the other member is
    // untouched. A `PublishCluster` that had emptied the roster would pass the assertion
    // above and fail this one.
    CHECK(VerdictOf(membership, "10.0.0.2", Proving("m2")) == Membership::Member);

    // And the ADDRESS the forgotten machine was admitted at is no forget: no host is recorded,
    // so a caller from there proving nothing is a stranger, refused as one.
    CHECK(VerdictOf(membership, "10.0.0.1", std::nullopt) == Membership::Outsider);
    CHECK(VerdictOf(membership, "10.9.9.9", std::nullopt) == Membership::Outsider);
}

TEST_CASE("A re-admit under a new key admits the new key only, and a reload lifts nothing", "[node][membership][forget]")
{
    NodeConfig cfg;
    cfg.nodeId = "node-a";

    NodeMembership membership { cfg, membershipLog };

    auto state = FastCache::Cluster::ClusterState {};
    Admit(state, "m1", "10.0.0.1:6676", Proving("m1").key);
    Forget(state, "m1");
    membership.PublishCluster(state);
    REQUIRE(VerdictOf(membership, "10.0.0.1", Proving("m1")) == Membership::Forgotten);

    // A reload must NOT lift it. `Adopt` settles `--fleet-open` and only that, so an
    // implementation that rebuilt the participants from the configuration would erase every
    // revocation agreed since startup, on the first SIGHUP, and the fleet would start serving
    // the decommissioned machine again with nothing to say why.
    membership.Adopt(cfg);
    CHECK(VerdictOf(membership, "10.0.0.1", Proving("m1")) == Membership::Forgotten);

    // What brings the machine back is the cluster admitting it again under a NEW key -- the
    // revoked one is never admitted again -- which reaches here as a committed state. The new
    // key is a member; the old one stays forgotten, whatever id it now claims.
    auto const reminted = Proving("m1-reminted").key;
    Admit(state, "m1", "10.0.0.1:6676", reminted);
    membership.PublishCluster(state);
    CHECK(VerdictOf(membership, "10.0.0.1", FastCache::ProvenIdentity { .id = "m1", .key = reminted })
          == Membership::Member);
    CHECK(VerdictOf(membership, "10.0.0.1", Proving("m1")) == Membership::Forgotten);
}

TEST_CASE("A forget reaches an open node too", "[node][membership][forget]")
{
    // The decision `--fleet-open` forces, and the one door a forget could have been left out
    // of. The flag says "I have not enumerated who may use this fleet" -- a blanket over
    // machines nobody named. A forget names one, by revoking its key. Letting the blanket win
    // would make a local flag resurrect a machine the cluster positively removed, on exactly
    // the node nobody has reconfigured yet.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;

    NodeMembership membership { cfg, membershipLog };

    // The control first, because the whole case rests on this node being open at all: a
    // fixture that had failed to open would report a refusal below for the ordinary reason and
    // prove nothing.
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    auto state = FastCache::Cluster::ClusterState {};
    Admit(state, "gone", "10.0.0.1:6676", Proving("gone").key);
    Forget(state, "gone");
    membership.PublishCluster(state);

    CHECK(VerdictOf(membership, "10.0.0.1", Proving("gone")) == Membership::Forgotten);

    // And the blanket still covers everybody else -- a stranger, and a machine proving a key
    // nobody revoked -- so the forget narrowed exactly one machine rather than closing the node.
    CHECK(VerdictOf(membership, "10.9.9.9", std::nullopt) == Membership::Member);
    CHECK(VerdictOf(membership, "10.0.0.1", Proving("other")) == Membership::Member);
}

TEST_CASE("A reload that turns --fleet-open off refuses a stranger from the next request", "[node][membership]")
{
    // The one admission setting a reload moves, and the direction that fails OPEN: a node
    // an operator has just closed that went on serving strangers would report nothing,
    // because admission succeeding is the ordinary case.
    NodeConfig open;
    open.nodeId = "node-a";
    open.fleetOpen = true;

    NodeMembership membership { open, membershipLog };
    REQUIRE(membership.Oracle().Classify("10.9.9.9") == Membership::Member);

    auto closed = open;
    closed.fleetOpen = false;
    membership.Adopt(closed);
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Outsider);
    // This machine is still admitted, so the narrowing closed the network and not the node.
    CHECK(membership.Oracle().Classify("127.0.0.1") == Membership::Member);

    // And back: the widening applies too.
    membership.Adopt(open);
    CHECK(membership.Oracle().Classify("10.9.9.9") == Membership::Member);
}

TEST_CASE("A rosterless node that runs no consensus admits no other machine, by address or by key",
          "[node][membership][admission]")
{
    // What makes `Absent` the honest answer at `WorkerLease` and `NodeRoster`: there a worker
    // that holds no roster builds `UncheckedLeaseValidator` whenever `AdmitsRemotePeers(cfg,
    // Absent)` says no other machine is admitted, and that is safe only while THIS oracle agrees
    // -- no remote address and no proven key gets through it. A no-consensus node is never handed
    // a `ClusterState`, so its key roster stays empty and a proof has nothing to be checked
    // against, and neither has a ticket. If a later route (a new participant) admitted a remote
    // caller here without also moving `AdmitsRemotePeers`, the unchecked port would reopen
    // silently: this case is what goes red first.
    NodeConfig cfg;
    cfg.schedulers = { "127.0.0.1:6675" };
    cfg.clusterDir = "cluster";
    cfg.nodeListen = "0.0.0.0:6674";
    REQUIRE_FALSE(RunsConsensus(cfg));
    REQUIRE(cfg.voterKeys.empty());
    // The premise the call sites act on.
    REQUIRE_FALSE(AdmitsRemotePeers(cfg, RosterPresence::Absent));

    NodeMembership membership { cfg, membershipLog };
    auto const& oracle = membership.Oracle();
    auto const proven = std::optional { ProvenIdentity { .id = "n9", .key = Testing::TestKeyPair("n9").PublicKey() } };

    // A remote address with nothing established, the same address proving a well-formed key, and
    // the same address presenting a verified ticket for it.
    CHECK(Distributed::ExplainConnection(oracle, ConnectionFacts { .host = "10.0.0.7" }).verdict != Membership::Member);
    CHECK(Distributed::ExplainConnection(oracle, ConnectionFacts { .host = "10.0.0.7", .proven = proven }).verdict
          != Membership::Member);
    CHECK(Distributed::ExplainConnection(oracle, ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = proven })
              .verdict
          != Membership::Member);

    // This machine is still admitted, so the refusal above is about the caller and not a node
    // that refuses everybody.
    CHECK(Distributed::ExplainConnection(oracle, ConnectionFacts { .host = "127.0.0.1" }).verdict == Membership::Member);

    // And the control that makes the key half able to fail: the same key IS admitted, by proof and
    // by ticket, once a roster names it -- which only a node running consensus is ever handed.
    auto state = FastCache::Cluster::ClusterState {};
    state.principals.push_back(FastCache::Cluster::ClusterPrincipal { .id = "n9", .publicKey = proven->key });
    membership.PublishCluster(state);
    CHECK(Distributed::ExplainConnection(oracle, ConnectionFacts { .host = "10.0.0.7", .proven = proven }).verdict
          == Membership::Member);
    CHECK(Distributed::ExplainConnection(oracle, ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = proven })
              .verdict
          == Membership::Member);
}

TEST_CASE("A learner's verified ticket admits it once consensus publishes the cluster, and a forget refuses it",
          "[node][membership][ticket]")
{
    // What makes a PURE worker able to verify a ticket: it runs consensus as a learner, so the
    // cluster reaches it through `PublishCluster` like any member's, and the key roster a ticket
    // is folded against is filled with members of EITHER seat. The case above shows the other
    // half -- a node handed no cluster admits nobody by ticket -- so a learner seat is the whole
    // difference between the two, and this pins that the seat does not decide admission.
    NodeConfig cfg;
    cfg.nodeId = "worker";
    NodeMembership membership { cfg, membershipLog };
    auto const machine = Proving("pc-07");
    auto const presenting = [&membership, &machine](std::string const& host) {
        return Distributed::ExplainConnection(membership.Oracle(),
                                              ConnectionFacts { .host = host, .authenticatedMachine = machine })
            .verdict;
    };

    // Before any cluster reaches it, a verified ticket admits nobody: fails CLOSED.
    CHECK(presenting("10.0.0.7") != Membership::Member);

    auto state = FastCache::Cluster::ClusterState {};
    FastCache::Cluster::Apply(state,
                              FastCache::Cluster::Command { .kind = FastCache::Cluster::CommandKind::AddLearner,
                                                            .key = machine.id,
                                                            .value = "10.0.0.7:6676",
                                                            .schedulerEndpoint = {},
                                                            .publicKey = machine.key,
                                                            .role = std::nullopt });
    REQUIRE(state.members.size() == 1);
    REQUIRE(state.members.front().seat == FastCache::Cluster::MemberSeat::Learner);
    membership.PublishCluster(state);

    // A learner member: admitted by its ticket, from whatever address it dialled from.
    CHECK(presenting("10.0.0.7") == Membership::Member);
    CHECK(presenting("10.9.9.9") == Membership::Member);

    // And forgotten, refused as forgotten rather than as a stranger.
    Forget(state, machine.id);
    membership.PublishCluster(state);
    CHECK(presenting("10.0.0.7") == Membership::Forgotten);
}

TEST_CASE("A revoked ticket's evidence refuses on an open node, whatever the key roster still says",
          "[node][membership][ticket][forget]")
{
    // The evidence a ticket refused for a revoked key leaves on its connection answers `Forgotten`
    // and nothing else. Asked of an open node whose key roster has NOT yet heard of the forget --
    // it still holds pc-07 live, since the lease roster that refused the ticket and this roster are
    // published at different moments -- it must still refuse: a roster that lags must not turn a
    // forget into an admission.
    NodeConfig cfg;
    cfg.nodeId = "node-a";
    cfg.fleetOpen = true;
    NodeMembership membership { cfg, membershipLog };
    auto state = FastCache::Cluster::ClusterState {};
    Admit(state, "pc-07", "10.0.0.7:6676", Proving("pc-07").key);
    membership.PublishCluster(state);

    auto const evidence = RevokedKeyEvidence { Proving("pc-07") };
    auto const decision = Distributed::ExplainConnection(membership.Oracle(),
                                                         ConnectionFacts { .host = "10.0.0.7", .revokedMachine = evidence });
    CHECK(decision.verdict == Membership::Forgotten);
    CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone));
    CHECK_FALSE(decision.decidedBy.Has(Distributed::MembershipParticipant::OpenPolicy));

    // Loopback and a live ticket for the same key do not lift it: `Forgotten` outranks both.
    CHECK(Distributed::ExplainConnection(
              membership.Oracle(),
              ConnectionFacts { .host = "127.0.0.1", .authenticatedMachine = Proving("pc-07"), .revokedMachine = evidence })
              .verdict
          == Membership::Forgotten);

    // The control, which says the EVIDENCE decided: the same host presenting nothing is admitted by
    // the open policy -- by design, an anonymous connection is admitted on an open fleet.
    CHECK(VerdictOf(membership, "10.0.0.7", std::nullopt) == Membership::Member);
}
