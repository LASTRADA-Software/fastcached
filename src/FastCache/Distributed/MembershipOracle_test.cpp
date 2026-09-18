// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>

using namespace FastCache::Distributed;
using FastCache::Testing::FixedMembership;

TEST_CASE("An empty cluster admits nobody but this machine", "[distributed][membership]")
{
    // The direction a mistake has to fail in. A node that has not yet discovered a
    // peer -- or whose discovery is misconfigured, or whose key is wrong -- must not
    // silently become an open scheduler: that failure is invisible from both ends,
    // because the fleet keeps working and simply serves strangers too.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers };

    CHECK(cluster.Size() == 0);
    CHECK(cluster.Classify("10.0.0.9") == Membership::Outsider);
    CHECK(cluster.Classify("") == Membership::Outsider);

    // This machine is the one exception, and it is unconditional -- see the case
    // below for why an unconfigured node still has to serve its own builds.
    CHECK(cluster.Classify("127.0.0.1") == Membership::Member);
}

TEST_CASE("A member is admitted by host, whatever port it dials from", "[distributed][membership]")
{
    // The whole reason the constructor takes endpoints and stores hosts. Discovery
    // admits a peer at a (node, endpoint) pair, but a peer *connecting* to the
    // scheduler comes from an ephemeral source port -- so an endpoint-keyed set would
    // refuse every legitimate member while looking entirely correct, and the fleet
    // would silently never distribute anything.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "10.0.0.1:7000", "10.0.0.2:7000" } };

    CHECK(cluster.Classify("10.0.0.1") == Membership::Member);
    CHECK(cluster.Classify("10.0.0.2") == Membership::Member);
    CHECK(cluster.Classify("10.0.0.3") == Membership::Outsider);

    // Matched whole, not by prefix. A prefix test would admit `10.0.0.10` on the
    // strength of `10.0.0.1`, which is a different machine.
    CHECK(cluster.Classify("10.0.0.10") == Membership::Outsider);

    // And an endpoint is NOT an identity here. Querying in the vocabulary the set was
    // built from is the mistake the constructor exists to make impossible, so it is
    // pinned: this must not accidentally start working.
    CHECK(cluster.Classify("10.0.0.1:7000") == Membership::Outsider);
}

TEST_CASE("An IPv6 endpoint keeps its address rather than its last colon group", "[distributed][membership]")
{
    // `rfind(':')` on `[::1]:7000` splits at the wrong colon and yields a host of
    // `[::1]` or worse -- the exact defect `Core/HostPort` exists to hold in one
    // place. Reaching for it here rather than splitting locally is what keeps this
    // from being a second author of that rule.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "[::1]:7000", "[fe80::1]:7000" } };

    CHECK(cluster.Classify("::1") == Membership::Member);
    CHECK(cluster.Classify("fe80::1") == Membership::Member);
    CHECK(cluster.Classify("fe80::2") == Membership::Outsider);
}

TEST_CASE("An endpoint with no port is kept whole", "[distributed][membership]")
{
    // A member the set cannot represent must not silently stop being one. Dropping it
    // would be the same silent-refusal failure the host/endpoint collapse exists to
    // prevent, arriving by a different route.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "10.0.0.1" } };

    CHECK(cluster.Size() == 1);
    CHECK(cluster.Classify("10.0.0.1") == Membership::Member);
}

TEST_CASE("Both IPv6 spellings of a member survive publication", "[distributed][membership]")
{
    // The two shapes a hand-rolled `SplitHostPort` + fallback got wrong in opposite
    // directions, each producing a stored "host" no kernel ever reports as a peer
    // address -- so a listed member silently stopped being one while `Size()` still
    // counted it.
    SECTION("an unbracketed literal is not cut at its last colon")
    {
        // Split at the last colon this is `2001:db8:`, a plausible-looking wrong
        // answer rather than a failure.
        ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "2001:db8::1" } };

        CHECK(cluster.Size() == 1);
        CHECK(cluster.Classify("2001:db8::1") == Membership::Member);
        CHECK(cluster.Classify("2001:db8::2") == Membership::Outsider);
    }

    SECTION("a bracketed literal with no port loses its brackets")
    {
        ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "[2001:db8::1]" } };

        CHECK(cluster.Classify("2001:db8::1") == Membership::Member);
    }

    SECTION("and the ordinary bracketed endpoint still works")
    {
        ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "[2001:db8::1]:7000" } };

        CHECK(cluster.Classify("2001:db8::1") == Membership::Member);
    }
}

TEST_CASE("A dual-stack listener's mapped spelling still names a listed member", "[distributed][membership]")
{
    // The failure `IsLoopbackHost` was already taught to avoid, arriving at the list
    // instead of at the loopback branch: a node bound to `::` is dual-stack, so an
    // IPv4 peer reaches `Classify` as `::ffff:10.0.0.1`. Compared raw against the
    // list, EVERY member is refused while this machine's own clients are still
    // admitted -- a fleet that looks configured, serves its own box, and distributes
    // nothing.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "10.0.0.1:7000" } };

    CHECK(cluster.Classify("::ffff:10.0.0.1") == Membership::Member);
    CHECK(cluster.Classify("10.0.0.1") == Membership::Member);

    // Folded, never widened: the mapped form of a stranger is still a stranger, and
    // the prefix rule survives the fold.
    CHECK(cluster.Classify("::ffff:10.0.0.2") == Membership::Outsider);
    CHECK(cluster.Classify("::ffff:10.0.0.10") == Membership::Outsider);

    // The other side of the fold, for a set published in the mapped form. Bracketed,
    // because that is the only spelling a mapped address with a port can have: bare,
    // `::ffff:10.0.0.1:7000` is indistinguishable from an IPv6 literal and is
    // deliberately kept whole rather than cut at a guessed colon.
    ClusterMembership const mapped { MembershipParticipant::ClusterMembers, { "[::ffff:10.0.0.1]:7000" } };
    CHECK(mapped.Classify("10.0.0.1") == Membership::Member);
}

TEST_CASE("An empty member entry does not admit a peer this machine cannot name", "[distributed][membership]")
{
    // Two unanswerable questions are not a match. Under a raw string compare they
    // were: an endpoint that published as nothing stored an empty host, and the empty
    // host is exactly what `FormatPeerAddress` answers for a peer whose `getpeername`
    // failed -- so the one caller that must never be admitted matched.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "", "10.0.0.1:7000" } };

    CHECK(cluster.Classify("") == Membership::Outsider);
    CHECK(cluster.Classify("10.0.0.1") == Membership::Member);
}

TEST_CASE("Membership can be republished while the scheduler runs", "[distributed][membership]")
{
    // The carve-out to configuration-at-construction, and the reason for it:
    // membership is precisely what changes while this object lives. Rebuilding the
    // oracle per join would mean handing a new one to a running server.
    ClusterMembership cluster { MembershipParticipant::ClusterMembers, { "10.0.0.1:7000" } };
    REQUIRE(cluster.Classify("10.0.0.1") == Membership::Member);

    cluster.Publish({ "10.0.0.2:7000" });

    // A peer that left is refused from the next request, not from the next restart.
    CHECK(cluster.Classify("10.0.0.1") == Membership::Outsider);
    CHECK(cluster.Classify("10.0.0.2") == Membership::Member);
    CHECK(cluster.Size() == 1);
}

TEST_CASE("An open deployment admits everyone, and says so by name", "[distributed][membership]")
{
    // The right answer for one machine, or a fleet whose reachability is its
    // boundary -- but never a default. "No policy" and "a policy that admits
    // everybody" have to be the same explicit decision, which is why this is a
    // named type somebody constructs rather than an unset field.
    OpenMembership const open;

    CHECK(open.Classify("10.0.0.9:7100") == Membership::Member);
    CHECK(open.Classify("") == Membership::Member);
}

TEST_CASE("Admission is a union, so one route publishing does not revoke another", "[distributed][membership]")
{
    // Issue #251. Two lists answering two different questions -- who may spend this
    // node's CPU (clients included: a developer's laptop, a CI runner, machines that
    // never join consensus and never should) and who is in the cluster (peers only)
    // -- were one list, so the first replicated membership commit discarded
    // everything an operator had listed. And agreeing something is routine.
    ClusterMembership listed { MembershipParticipant::FleetMemberList, { "10.0.0.1:7000" } };
    ClusterMembership agreed { MembershipParticipant::ClusterMembers };
    AnyOfMembership const admitted { { &listed, &agreed } };

    CHECK(admitted.Classify("10.0.0.1") == Membership::Member);
    CHECK(admitted.Classify("10.0.0.2") == Membership::Outsider);

    agreed.Publish({ "10.0.0.2:7000" });

    // Each route is still replaced wholesale by whoever owns it, and neither
    // publisher speaks for the other.
    CHECK(admitted.Classify("10.0.0.1") == Membership::Member);
    CHECK(admitted.Classify("10.0.0.2") == Membership::Member);

    // Adding a route must not add an admission.
    CHECK(admitted.Classify("10.0.0.9") == Membership::Outsider);

    // The same rule at its limit: a route that publishes an empty set -- which is
    // what a clustered node sees before the first entry naming anybody commits --
    // takes nothing away from the others. That is the admission-layer spelling of a
    // rule consensus already applies: absence from `ClusterState` is not removal.
    agreed.Publish({});
    CHECK(admitted.Classify("10.0.0.1") == Membership::Member);
    CHECK(admitted.Classify("10.0.0.2") == Membership::Outsider);
}

TEST_CASE("A composite with no participants refuses everybody", "[distributed][membership]")
{
    // The direction this default has to fail in, and the same one every other default
    // in this file takes: a node whose routes have not been wired must not become an
    // open scheduler. `OpenMembership` is how "admit everybody" is said out loud.
    AnyOfMembership const admitted { {} };

    CHECK(admitted.Classify("10.0.0.1") == Membership::Outsider);

    // Not even loopback, because a composite has no policy of its own -- the
    // this-machine rule belongs to the participants that have one, and inventing it
    // here would make an unwired composite quietly useful instead of visibly wrong.
    CHECK(admitted.Classify("127.0.0.1") == Membership::Outsider);
}

TEST_CASE("A forget outranks a listing, whichever participant said it", "[distributed][membership][forget]")
{
    // #1309. The composite folds on `PrecedenceOf` rather than admitting on any_of: the host
    // an operator has just forgotten in the cluster is exactly the one still named by
    // `--fleet-member` on a node nobody has reconfigured, so a forget has to WIN. What this
    // case distinguishes is the fold from the old `any_of(... == Member)`, which answered
    // `Member` here and flattened the forget to `Outsider` when it stood alone.
    FixedMembership const forgets { Membership::Forgotten, MembershipParticipant::ClientTombstone };
    ClusterMembership listed { MembershipParticipant::FleetMemberList, { "10.0.0.1:7000" } };

    SECTION("the forget is asked first")
    {
        AnyOfMembership const admitted { { &forgets, &listed } };
        CHECK(admitted.Classify("10.0.0.1") == Membership::Forgotten);
    }

    SECTION("the listing is asked first")
    {
        AnyOfMembership const admitted { { &listed, &forgets } };
        CHECK(admitted.Classify("10.0.0.1") == Membership::Forgotten);
    }

    SECTION("a forget alone reaches the surface as itself, not as an outsider")
    {
        AnyOfMembership const admitted { { &forgets } };
        CHECK(admitted.Classify("10.0.0.9") == Membership::Forgotten);
    }

    SECTION("and nothing else is disturbed: a listed host with no forget is still a member")
    {
        // Any route will do: this fake's whole job is to have NO opinion, and `DecidedBy`
        // discards the participant given alongside `Outsider`, so the set comes back empty
        // whatever is named. Asserted below rather than assumed.
        FixedMembership const nobody { Membership::Outsider, MembershipParticipant::ClusterMembers };
        REQUIRE(nobody.Explain("10.0.0.9").decidedBy.Empty());
        AnyOfMembership const admitted { { &nobody, &listed } };
        CHECK(admitted.Classify("10.0.0.1") == Membership::Member);
        CHECK(admitted.Classify("10.0.0.2") == Membership::Outsider);
    }
}

TEST_CASE("An admission names every route that admitted, not the first one", "[distributed][membership][explain]")
{
    // #1471's first acceptance clause. The operator question behind it is not "is this host
    // served" -- which the verdict already answered -- but "what do I have to change to stop it
    // being served", and when two routes admit, the answer is BOTH of them.
    //
    // This case is why `decidedBy` is a set. The first version of this change folded with a
    // strict `>` and kept the first winner, so a host on both lists reported `FleetMemberList`
    // alone; an operator following that removes it from `--fleet-member` and finds it still
    // served by the cluster, which is the scenario the ticket opens with. The fix had reproduced
    // the defect.
    ClusterMembership const listed { MembershipParticipant::FleetMemberList, { "10.0.0.1:7000", "10.0.0.3:7000" } };
    ClusterMembership const agreed { MembershipParticipant::ClusterMembers, { "10.0.0.2:7000", "10.0.0.3:7000" } };

    SECTION("a host only `--fleet-member` names reports that list")
    {
        // A CONTROL for the both-routes section: without a case per route, a fold that
        // attributed every admission to one fixed route would pass it.
        AnyOfMembership const admitted { { &listed, &agreed } };
        auto const decision = admitted.Explain("10.0.0.1");

        CHECK(decision.verdict == Membership::Member);
        CHECK(decision.decidedBy.Count() == 1);
        CHECK(decision.decidedBy.Has(MembershipParticipant::FleetMemberList));
        CHECK_FALSE(decision.decidedBy.Has(MembershipParticipant::ClusterMembers));
    }

    SECTION("a host only the cluster agreed reports the committed set")
    {
        // The other half of the control, and the direction the ticket's scenario is about: the
        // host is NOT in `--fleet-member`, so an operator editing that list changes nothing.
        AnyOfMembership const admitted { { &listed, &agreed } };
        auto const decision = admitted.Explain("10.0.0.2");

        CHECK(decision.verdict == Membership::Member);
        CHECK(decision.decidedBy.Count() == 1);
        CHECK(decision.decidedBy.Has(MembershipParticipant::ClusterMembers));
        CHECK_FALSE(decision.decidedBy.Has(MembershipParticipant::FleetMemberList));
    }

    SECTION("a host BOTH routes admit reports both, because both are true")
    {
        // The section that fails under the single-winner fold.
        AnyOfMembership const admitted { { &listed, &agreed } };
        auto const decision = admitted.Explain("10.0.0.3");

        CHECK(decision.verdict == Membership::Member);
        CHECK(decision.decidedBy.Count() == 2);
        CHECK(decision.decidedBy.Has(MembershipParticipant::FleetMemberList));
        CHECK(decision.decidedBy.Has(MembershipParticipant::ClusterMembers));
    }

    SECTION("and the answer does not depend on the order the routes are folded in")
    {
        // Order dependence WAS the defect's signature, so this asserts EQUALITY of the two
        // answers rather than that each contains both routes: an implementation that unions in
        // one order and replaces in the other satisfies "contains both" in one direction only,
        // and a case asking each direction separately would pass on whichever it happened to
        // check first.
        AnyOfMembership const listedFirst { { &listed, &agreed } };
        AnyOfMembership const agreedFirst { { &agreed, &listed } };

        CHECK(listedFirst.Explain("10.0.0.3") == agreedFirst.Explain("10.0.0.3"));
        CHECK(listedFirst.Explain("10.0.0.3").decidedBy.Count() == 2);
    }

    SECTION("a host no route names is refused by ABSENCE, attributed to nobody")
    {
        // `Outsider` with an EMPTY set, which is a different fact from any route having refused
        // it: both lists were asked and neither mentioned the host. The assertion that stops
        // `DecidedBy` being loosened into naming the author of a silence.
        AnyOfMembership const admitted { { &listed, &agreed } };
        auto const decision = admitted.Explain("10.9.9.9");

        CHECK(decision.verdict == Membership::Outsider);
        CHECK(decision.decidedBy.Empty());
        CHECK(decision.decidedBy.Count() == 0);
    }

    SECTION("a forget REPLACES the listing rather than joining it")
    {
        // The REPLACE arm, and the guard on the tie arm: a tombstone outranks every admission
        // route, so the set names the tombstone ALONE even though `--fleet-member` is still
        // admitting the host. Asserting the listing is ABSENT is the whole point -- a tie arm
        // widened into an unconditional union would tell an operator that `--fleet-member`
        // refused a host it was in fact still serving.
        ForgottenMembership const forgotten { { "10.0.0.3" } };
        AnyOfMembership const admitted { { &listed, &agreed, &forgotten } };
        auto const decision = admitted.Explain("10.0.0.3");

        CHECK(decision.verdict == Membership::Forgotten);
        CHECK(decision.decidedBy.Count() == 1);
        CHECK(decision.decidedBy.Has(MembershipParticipant::ClientTombstone));
        CHECK_FALSE(decision.decidedBy.Has(MembershipParticipant::FleetMemberList));
        CHECK_FALSE(decision.decidedBy.Has(MembershipParticipant::ClusterMembers));
    }
}

TEST_CASE("A scheduler refuses a non-member through the oracle", "[distributed][membership][scheduler]")
{
    // The two halves joined: the oracle answers who, `SchedulerService` decides
    // what. Asserted together because each is correct in isolation and the wiring
    // between them is what a caller actually depends on.
    ClusterMembership const cluster { MembershipParticipant::ClusterMembers, { "10.0.0.1:7000" } };
    FastCache::ManualClock clock;
    FastCache::AtomicMetricsSink metrics;
    FastCache::NullLogger schedulerLogger;
    FastCache::ManualWallClock wallClock;
    auto const signer = FastCache::Testing::TestLeaseSigner();
    SchedulerService service { clock, wallClock, metrics, schedulerLogger, signer, {} };
    service.SetRole(SchedulerRole::Leader, {}, StandaloneSchedulerTerm);

    auto const ask = [&](std::string_view peer) {
        return service.Lease(
            CallerContext { .membership = cluster.Classify(peer), .peerId = std::string { peer } },
            FastCache::CompileCacheWire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    };

    // A member reaches the fleet and is refused only for want of a worker, which is
    // the fleet's own answer rather than the policy's.
    CHECK(ask("10.0.0.1").error == FastCache::CompileCacheWire::ErrorCode::NoWorker);

    // A stranger never gets that far.
    CHECK(ask("10.0.0.9").error == FastCache::CompileCacheWire::ErrorCode::NotAMember);
}

TEST_CASE("This machine is a member of its own fleet, whatever the list says", "[distributed][membership]")
{
    // The rule that makes an unconfigured node useful and still closed to the
    // network. Anti-leeching exists to stop OTHER machines spending capacity they do
    // not contribute; a process on this host already has this host's CPU, and the
    // `fastcache-cc` a developer runs against their own node is the whole reason the
    // node is there.
    //
    // Without it, a node whose operator had listed only their peers would refuse
    // their own builds — a fleet that looks configured and serves nobody locally,
    // and which nothing would report.
    ClusterMembership const remoteOnly { MembershipParticipant::ClusterMembers, { "10.0.0.1:7000", "10.0.0.2:7000" } };

    CHECK(remoteOnly.Classify("127.0.0.1") == Membership::Member);
    CHECK(remoteOnly.Classify("::1") == Membership::Member);
    CHECK(remoteOnly.Classify("10.0.0.1") == Membership::Member);
    CHECK(remoteOnly.Classify("10.9.9.9") == Membership::Outsider);

    // And an empty list is still closed to everybody but this machine, which is what
    // makes "no configuration" a safe state rather than an open one.
    ClusterMembership const unconfigured { MembershipParticipant::ClusterMembers, {} };
    CHECK(unconfigured.Classify("127.0.0.1") == Membership::Member);
    CHECK(unconfigured.Classify("10.0.0.1") == Membership::Outsider);
}

TEST_CASE("Every spelling a kernel reports for a local peer is local", "[distributed][membership]")
{
    // The whole 127/8, IPv6 loopback, and the IPv4-mapped form a dual-stack listener
    // reports for an IPv4 client. Missing that last one is the subtle failure: a node
    // bound to `::` would classify every local client as a stranger and refuse its
    // own machine, on some hosts and not others.
    ClusterMembership const membership { MembershipParticipant::ClusterMembers, {} };

    CHECK(membership.Classify("127.0.0.1") == Membership::Member);
    CHECK(membership.Classify("127.0.0.53") == Membership::Member);
    CHECK(membership.Classify("::ffff:127.0.0.1") == Membership::Member);

    // Not local, and the near-misses are the point: a prefix test on "127." alone
    // would be right, but one on "1" or on "::ffff:" alone would admit the network.
    CHECK(membership.Classify("128.0.0.1") == Membership::Outsider);
    CHECK(membership.Classify("::ffff:10.0.0.1") == Membership::Outsider);
    CHECK(membership.Classify("10.127.0.1") == Membership::Outsider);

    // `localhost` is deliberately NOT local here: it is whatever a resolver says it
    // is, and a resolver is not something a security decision may depend on. No
    // kernel reports it as a peer address either, so nothing legitimate is lost.
    CHECK(membership.Classify("localhost") == Membership::Outsider);
}

TEST_CASE("A peer this machine cannot name is refused", "[distributed][membership]")
{
    // The direction an unidentifiable caller has to fail in. An empty host is what
    // `FormatPeerAddress` answers for a peer whose family it does not know or whose
    // `getpeername` failed, and handing this machine's CPU to something it cannot
    // name is the one outcome that must not be possible.
    ClusterMembership const membership { MembershipParticipant::ClusterMembers, { "10.0.0.1:7000" } };

    CHECK(membership.Classify("") == Membership::Outsider);
}

TEST_CASE("A forgotten set answers about forgotten hosts and nothing else", "[distributed][membership][forget]")
{
    // The real oracle rather than the fake above, which could say `Forgotten` about
    // anybody and therefore proved only the fold. What only this can show is that the
    // set answers the RIGHT hosts -- and, twice as important, that it admits nobody: it
    // is composed into every node's participants, so a `Member` escaping from here would
    // widen admission on a node whose operator listed nothing at all.
    ForgottenMembership const forgotten { { "10.0.0.7", "ci-runner-3.example" } };

    CHECK(forgotten.Classify("10.0.0.7") == Membership::Forgotten);
    CHECK(forgotten.Classify("ci-runner-3.example") == Membership::Forgotten);

    // A host it has nothing to say about is `Outsider`, which is `PrecedenceOf` 0 and
    // therefore silence in the fold -- not an opinion that this host is a stranger.
    CHECK(forgotten.Classify("10.0.0.8") == Membership::Outsider);
    CHECK(forgotten.Classify("") == Membership::Outsider);

    // Whole-host, the same rule the member sets keep: `10.0.0.7` must not forget
    // `10.0.0.70`, and the dual-stack spelling of a forgotten host is the same machine.
    CHECK(forgotten.Classify("10.0.0.70") == Membership::Outsider);
    CHECK(forgotten.Classify("::ffff:10.0.0.7") == Membership::Forgotten);
}

TEST_CASE("This machine is never forgotten, whatever the set holds", "[distributed][membership][forget]")
{
    // The guard `ForgottenVerdicts` exists for, asserted against a set that should be
    // impossible: `Cluster::Validate` refuses a loopback host to `ForgetClient`, so no
    // such entry can be committed today. That is a reason this is never REACHED, not a
    // reason to leave it out -- the consequence is that a node stops serving the local
    // builds that are the entire reason it is installed, on every surface at once, with
    // admission having succeeded for years and nothing to report the change.
    //
    // It also distinguishes this class from a `ClusterMembership` with its verdict
    // swapped, which is the cheap-looking implementation: that one answers `Member` for
    // loopback, and remapping its match verdict would have remapped the loopback arm too.
    ForgottenMembership const forgotten { { "127.0.0.1", "::1", "10.0.0.7" } };

    CHECK(forgotten.Classify("127.0.0.1") == Membership::Outsider);
    CHECK(forgotten.Classify("127.0.0.53") == Membership::Outsider);
    CHECK(forgotten.Classify("::1") == Membership::Outsider);

    // The control, and it is load-bearing: without it a class that answered `Outsider`
    // for every caller would pass the three checks above.
    CHECK(forgotten.Classify("10.0.0.7") == Membership::Forgotten);
}

TEST_CASE("A forget is published wholesale, and a re-admit is its removal", "[distributed][membership][forget]")
{
    ForgottenMembership forgotten { { "10.0.0.7" } };
    REQUIRE(forgotten.Classify("10.0.0.7") == Membership::Forgotten);

    // What a re-admit looks like from here: the tombstone leaves the replicated state,
    // and the next publish carries a set without it. There is no "un-forget" call,
    // because the cluster's committed set is the whole truth about this question.
    forgotten.Publish({});
    CHECK(forgotten.Classify("10.0.0.7") == Membership::Outsider);
    CHECK(forgotten.Size() == 0);
}
