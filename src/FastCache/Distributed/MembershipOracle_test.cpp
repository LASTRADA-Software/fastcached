// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/MembershipWire.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;

namespace
{

/// The fold a node composes -- this machine and a key roster -- over real oracles.
using Fold = Testing::RosterFold;

/// The identity a machine establishes under its own test key.
using Testing::IdentityOf;

} // namespace

TEST_CASE("No address admits a machine that is not this one", "[distributed][membership][admission]")
{
    // The address route is gone: a machine whose key the roster holds is still a stranger when its
    // connection established nothing, wherever it dials from.
    Fold const fold { { "pc-07" } };
    auto const decision = Distributed::ExplainConnection(fold.admitted, ConnectionFacts { .host = "10.0.0.7" });
    CHECK(decision.verdict == Distributed::Membership::Outsider);
    CHECK(decision.decidedBy.Empty());
}

TEST_CASE("This machine is admitted as itself, and says so", "[distributed][membership][admission]")
{
    // The rule that makes an unconfigured node useful and still closed to the network: a process
    // on this host already has this host's CPU. Every spelling a kernel reports for a local peer --
    // the whole 127/8, IPv6 loopback and the IPv4-mapped form a dual-stack listener reports.
    Fold const fold { {} };
    for (auto const* host: { "127.0.0.1", "::1", "::ffff:127.0.0.1", "127.0.0.2" })
    {
        INFO(host);
        auto const decision = Distributed::ExplainConnection(fold.admitted, ConnectionFacts { .host = host });
        CHECK(decision.verdict == Distributed::Membership::Member);
        CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::Loopback));
    }

    // Not local, and the near-misses are the point: a prefix test on "1" or on "::ffff:" alone would
    // admit the network. `localhost` is whatever a resolver says, and no kernel reports it as a peer.
    // An empty host is a peer this machine cannot name, which must not be handed its CPU.
    for (auto const* host: { "128.0.0.1", "::ffff:10.0.0.1", "10.127.0.1", "localhost", "" })
    {
        INFO(host);
        auto const decision = Distributed::ExplainConnection(fold.admitted, ConnectionFacts { .host = host });
        CHECK(decision.verdict == Distributed::Membership::Outsider);
        CHECK(decision.decidedBy.Empty());
    }
}

TEST_CASE("A proof and a ticket each admit a live key, and the fold names which", "[distributed][membership][admission]")
{
    Fold const fold { { "pc-07" } };
    auto const byTicket = Distributed::ExplainConnection(
        fold.admitted, ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = IdentityOf("pc-07") });
    CHECK(byTicket.verdict == Distributed::Membership::Member);
    CHECK(byTicket.decidedBy.Has(Distributed::MembershipParticipant::MachineTicket));
    CHECK_FALSE(byTicket.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));

    auto const byProof =
        Distributed::ExplainConnection(fold.admitted, ConnectionFacts { .host = "10.0.0.7", .proven = IdentityOf("pc-07") });
    CHECK(byProof.verdict == Distributed::Membership::Member);
    CHECK(byProof.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));
    CHECK_FALSE(byProof.decidedBy.Has(Distributed::MembershipParticipant::MachineTicket));

    // Both routes are right at once, so both are named -- the tie arm of the fold.
    auto const both = Distributed::ExplainConnection(
        fold.admitted,
        ConnectionFacts { .host = "10.0.0.7", .proven = IdentityOf("pc-07"), .authenticatedMachine = IdentityOf("pc-07") });
    CHECK(both.verdict == Distributed::Membership::Member);
    CHECK(both.decidedBy.Count() == 2);

    // A key the roster does not hold for that id admits nothing, by either evidence.
    auto stranger = IdentityOf("pc-07");
    stranger.key = Testing::TestKeyPair("somebody-else").PublicKey();
    CHECK(Distributed::ExplainConnection(fold.admitted,
                                         ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = stranger })
              .verdict
          == Distributed::Membership::Outsider);
}

TEST_CASE("A revoked key is refused from every address, loopback and --fleet-open included",
          "[distributed][membership][admission][forget]")
{
    // A forgotten machine's tickets are refused, as its proofs are: `KeyTombstone` outranks this
    // machine's own loopback and the open policy.
    Fold const fold { {}, { "gone" } };
    Distributed::OpenMembership const open;
    Distributed::AnyOfMembership const openly { { &fold.loopback, &open, &fold.keys } };
    for (auto const* host: { "10.0.0.7", "127.0.0.1" })
        for (auto const* oracle: { static_cast<Distributed::IMembershipOracle const*>(&fold.admitted),
                                   static_cast<Distributed::IMembershipOracle const*>(&openly) })
            for (auto const& facts: { ConnectionFacts { .host = host, .authenticatedMachine = IdentityOf("gone") },
                                      ConnectionFacts { .host = host, .proven = IdentityOf("gone") } })
            {
                INFO(host << (facts.proven.has_value() ? " by proof" : " by ticket"));
                auto const decision = Distributed::ExplainConnection(*oracle, facts);
                CHECK(decision.verdict == Distributed::Membership::Forgotten);
                CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone));
            }
}

TEST_CASE("A revoked key's verdict says how the key was shown, and only a proof may be told",
          "[distributed][membership][admission][forget]")
{
    // What a gate may SAY to a forgotten machine rides with the verdict: a proof shows possession, a
    // ticket shows bytes anybody may have captured. Each way a connection shows a revoked key, and
    // the fold of two of them, over loopback and the open policy as well.
    Fold const fold { {}, { "gone" } };
    Distributed::OpenMembership const open;
    Distributed::AnyOfMembership const openly { { &fold.loopback, &open, &fold.keys } };
    struct Row
    {
        char const* what;
        ConnectionFacts facts;
        bool byProof;
        bool byTicket;
    };
    auto const rows = std::to_array<Row>({
        { .what = "a proof",
          .facts = { .host = "10.0.0.7", .proven = IdentityOf("gone") },
          .byProof = true,
          .byTicket = false },
        { .what = "a ticket AUTH accepted before the forget",
          .facts = { .host = "10.0.0.7", .authenticatedMachine = IdentityOf("gone") },
          .byProof = false,
          .byTicket = true },
        { .what = "a ticket AUTH refused as revoked",
          .facts = { .host = "10.0.0.7", .revokedMachine = RevokedKeyEvidence { IdentityOf("gone") } },
          .byProof = false,
          .byTicket = true },
        { .what = "a proof and a ticket on one connection",
          .facts = { .host = "10.0.0.7",
                     .proven = IdentityOf("gone"),
                     .revokedMachine = RevokedKeyEvidence { IdentityOf("gone") } },
          .byProof = true,
          .byTicket = true },
    });
    for (auto const* oracle: { static_cast<Distributed::IMembershipOracle const*>(&fold.admitted),
                               static_cast<Distributed::IMembershipOracle const*>(&openly) })
        for (auto const& row: rows)
        {
            INFO(row.what);
            auto const decision = Distributed::ExplainConnection(*oracle, row.facts);
            REQUIRE(decision.verdict == Distributed::Membership::Forgotten);
            CHECK(decision.revokedBy.Has(Distributed::KeyEvidence::SessionProof) == row.byProof);
            CHECK(decision.revokedBy.Has(Distributed::KeyEvidence::MachineTicket) == row.byTicket);
            CHECK(Distributed::RevocationIsProven(decision) == row.byProof);
        }

    // A verdict that is not `Forgotten` is never "proven", whatever evidence it carries, and a
    // `Forgotten` that names no evidence says nothing: both directions fail toward silence.
    auto admitted = Distributed::DecidedBy(Distributed::Membership::Member, Distributed::MembershipParticipant::OpenPolicy);
    admitted.revokedBy.Add(Distributed::KeyEvidence::SessionProof);
    CHECK_FALSE(Distributed::RevocationIsProven(admitted));
    CHECK_FALSE(Distributed::RevocationIsProven(
        Distributed::DecidedBy(Distributed::Membership::Forgotten, Distributed::MembershipParticipant::KeyTombstone)));
}

TEST_CASE("A ticket admits a caller and never stands in for a proof", "[distributed][membership][admission]")
{
    Fold const fold { { "pc-07" } };
    auto const caller = Distributed::CallerContextOf(
        fold.admitted, ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = IdentityOf("pc-07") });
    CHECK(caller.membership == Distributed::Membership::Member);
    CHECK(caller.peerId == "10.0.0.7");
    // So `Register` over a ticket is refused `NodeIdentityRequired`.
    CHECK_FALSE(caller.provenNodeId.has_value());

    auto const proven =
        Distributed::CallerContextOf(fold.admitted, ConnectionFacts { .host = "10.0.0.7", .proven = IdentityOf("pc-07") });
    CHECK(proven.provenNodeId == std::optional<std::string> { "pc-07" });

    // The shape where the ATTRIBUTION is the only guard: a connection that proved a key the roster
    // does not hold AND presented a ticket for one it does. The ticket admits it -- and must not
    // lend its standing to the proof, or the unknown id becomes `provenNodeId` and passes
    // `ProvenNodeOnly`. The case above cannot show this: with no proof on the connection there is
    // no id to engage, however a ticket were attributed.
    auto const mixed = ConnectionFacts { .host = "10.0.0.7",
                                         .proven = IdentityOf("stranger"),
                                         .authenticatedMachine = IdentityOf("pc-07") };
    auto const decision = Distributed::ExplainConnection(fold.admitted, mixed);
    CHECK(decision.verdict == Distributed::Membership::Member);
    CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::MachineTicket));
    CHECK_FALSE(decision.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));
    CHECK_FALSE(Distributed::CallerContextOf(fold.admitted, mixed).provenNodeId.has_value());
}

TEST_CASE("Only a proof or a ticket rests on a machine's key", "[distributed][membership][admission]")
{
    Fold const fold { { "pc-07" } };
    Distributed::OpenMembership const open;
    Distributed::AnyOfMembership const openly { { &fold.loopback, &open, &fold.keys } };
    auto const rests = [](Distributed::IMembershipOracle const& oracle, ConnectionFacts const& facts) {
        return Distributed::RestsOnMachineKey(Distributed::ExplainConnection(oracle, facts));
    };
    CHECK(rests(fold.admitted, ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = IdentityOf("pc-07") }));
    CHECK(rests(fold.admitted, ConnectionFacts { .host = "10.0.0.7", .proven = IdentityOf("pc-07") }));
    CHECK_FALSE(rests(fold.admitted, ConnectionFacts { .host = "127.0.0.1" }));
    CHECK_FALSE(rests(openly, ConnectionFacts { .host = "10.0.0.7" }));
}

TEST_CASE("The reserved participant can be named by no decision", "[distributed][membership]")
{
    auto set = Distributed::MembershipParticipantSet {};
    set.Add(Distributed::MembershipParticipant::Reserved);
    CHECK(set.Empty());
    // A participant that is `Reserved` has no opinion: an admission nobody claims would be the
    // confident wrong signal from the other side, so it fails closed rather than admitting.
    auto const unclaimed =
        Distributed::DecidedBy(Distributed::Membership::Member, Distributed::MembershipParticipant::Reserved);
    CHECK(unclaimed.verdict == Distributed::Membership::Outsider);
    CHECK(unclaimed.decidedBy.Empty());
    CHECK(static_cast<std::uint8_t>(Distributed::MembershipParticipant::Reserved) == 0);
    CHECK(Distributed::MembershipWireRoutes[0].route == Distributed::MembershipParticipant::Reserved);
    CHECK(Distributed::MembershipWireRoutes[0].bit == 0);

    // The control: a live participant is recorded, so the empty set above is about `Reserved`.
    CHECK_FALSE(set.Add(Distributed::MembershipParticipant::Loopback).Empty());
    CHECK(Distributed::DecidedBy(Distributed::Membership::Member, Distributed::MembershipParticipant::Loopback).verdict
          == Distributed::Membership::Member);
}

TEST_CASE("A connection proving a key the record no longer holds is no proven node, wherever it is admitted from",
          "[distributed][membership][admission]")
{
    // The re-key shape. A connection proved m1's key; the cluster then re-admitted m1 under a NEW
    // key, which replaces the whole record without revoking the old key. The connection is still
    // admitted where an address admits it -- this machine, or a node that is open -- but the key it
    // proved is nobody's now, so it must not stand as m1 for the verbs a machine joins the fleet
    // with. With no ticket anywhere, only the attribution keeps `provenNodeId` disengaged.
    Fold fold { { "m1" } };
    Distributed::OpenMembership const open;
    Distributed::AnyOfMembership const openly { { &fold.loopback, &open, &fold.keys } };
    auto const onThisMachine = ConnectionFacts { .host = "127.0.0.1", .proven = IdentityOf("m1") };
    auto const remote = ConnectionFacts { .host = "10.0.0.7", .proven = IdentityOf("m1") };

    // Before the re-key the proof stands, which is what makes the answer below about the re-key.
    REQUIRE(Distributed::CallerContextOf(fold.admitted, onThisMachine).provenNodeId == std::optional<std::string> { "m1" });

    std::map<std::string, Ed25519PublicKey, std::less<>> rekeyed;
    rekeyed.emplace("m1", Testing::TestKeyPair("m1-rekeyed").PublicKey());
    fold.keys.Publish(std::move(rekeyed), {});

    for (auto const& [oracle, facts, route]:
         { std::tuple { static_cast<Distributed::IMembershipOracle const*>(&fold.admitted),
                        onThisMachine,
                        Distributed::MembershipParticipant::Loopback },
           std::tuple { static_cast<Distributed::IMembershipOracle const*>(&openly),
                        remote,
                        Distributed::MembershipParticipant::OpenPolicy } })
    {
        INFO(facts.host);
        auto const decision = Distributed::ExplainConnection(*oracle, facts);
        CHECK(decision.verdict == Distributed::Membership::Member);
        CHECK(decision.decidedBy.Has(route));
        CHECK_FALSE(decision.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));
        CHECK_FALSE(Distributed::CallerContextOf(*oracle, facts).provenNodeId.has_value());
    }
}

TEST_CASE("A membership fake refuses a label no production route could carry", "[distributed][membership]")
{
    // Loopback admits this machine and nobody else, so a fake that labels a remote host
    // `Loopback` models an admission production cannot produce -- a fake more permissive than the
    // real thing, whose cases pass while describing a route that does not exist.
    CHECK_THROWS_AS(
        (Testing::ListedMembership { { "127.0.0.1", "10.0.0.7" }, Distributed::MembershipParticipant::Loopback }),
        std::invalid_argument);
    CHECK_THROWS_AS(
        (Testing::FixedMembership { Distributed::Membership::Member, Distributed::MembershipParticipant::Loopback }),
        std::invalid_argument);
    CHECK_THROWS_AS((Testing::ListedMembership { { "10.0.0.7" }, Distributed::MembershipParticipant::Reserved }),
                    std::invalid_argument);

    // The controls: this machine's spellings under `Loopback`, a remote host under the one route
    // that admits one by address, and loopback's honest opinion of a remote host -- none.
    CHECK_NOTHROW((Testing::ListedMembership { { "127.0.0.1", "::1" }, Distributed::MembershipParticipant::Loopback }));
    CHECK_NOTHROW((Testing::ListedMembership { { "10.0.0.7" }, Distributed::MembershipParticipant::OpenPolicy }));
    CHECK_NOTHROW(
        (Testing::FixedMembership { Distributed::Membership::Outsider, Distributed::MembershipParticipant::Loopback }));
}

TEST_CASE("A key revoked while a ticketed connection is open refuses its next verb", "[distributed][membership][admission]")
{
    Fold fold { { "pc-07" } };
    auto const facts = ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = IdentityOf("pc-07") };
    REQUIRE(Distributed::ExplainConnection(fold.admitted, facts).verdict == Distributed::Membership::Member);
    std::map<std::string, Ed25519PublicKey, std::less<>> none;
    fold.keys.Publish(std::move(none), { Testing::TestKeyPair("pc-07").PublicKey() });
    CHECK(Distributed::ExplainConnection(fold.admitted, facts).verdict == Distributed::Membership::Forgotten);
}

TEST_CASE("An open deployment admits everyone, and says so by name", "[distributed][membership]")
{
    // The right answer for one machine, or a fleet whose reachability is its boundary -- but never
    // a default. "No policy" and "a policy that admits everybody" have to be the same explicit
    // decision, which is why this is a named type somebody constructs rather than an unset field.
    Distributed::OpenMembership const open;

    CHECK(open.Classify("10.0.0.9:7100") == Distributed::Membership::Member);
    CHECK(open.Classify("") == Distributed::Membership::Member);
    CHECK(open.Explain("10.0.0.9").decidedBy.Has(Distributed::MembershipParticipant::OpenPolicy));
}

TEST_CASE("A composite with no participants refuses everybody", "[distributed][membership]")
{
    // The direction this default has to fail in: a node whose routes have not been wired must not
    // become an open scheduler. `OpenMembership` is how "admit everybody" is said out loud.
    Distributed::AnyOfMembership const admitted { {} };

    CHECK(admitted.Classify("10.0.0.1") == Distributed::Membership::Outsider);

    // Not even loopback, because a composite has no policy of its own -- the this-machine rule
    // belongs to `LoopbackMembership`, and inventing it here would make an unwired composite
    // quietly useful instead of visibly wrong.
    CHECK(admitted.Classify("127.0.0.1") == Distributed::Membership::Outsider);
    CHECK(Distributed::ExplainConnection(admitted, ConnectionFacts { .host = "10.0.0.7", .proven = IdentityOf("pc-07") })
              .verdict
          == Distributed::Membership::Outsider);
}

TEST_CASE("A scheduler refuses a non-member through the oracle", "[distributed][membership][scheduler]")
{
    // The two halves joined: the oracle answers who, `SchedulerService` decides what. Asserted
    // together because each is correct in isolation and the wiring between them is what a caller
    // actually depends on.
    Fold const fold { { "pc-07" } };
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    core::platform::ManualWallClock wallClock;
    auto const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, schedulerLogger, signer, {} };
    service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);

    auto const ask = [&](ConnectionFacts facts) {
        return service.Lease(Distributed::CallerContextOf(fold.admitted, std::move(facts)),
                             CompileCacheWire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    };

    // A machine with a live key reaches the fleet and is refused only for want of a worker, which
    // is the fleet's own answer rather than the policy's.
    CHECK(ask(ConnectionFacts { .host = "10.0.0.7", .authenticatedMachine = IdentityOf("pc-07") }).error
          == CompileCacheWire::ErrorCode::NoWorker);

    // A non-loopback caller that established nothing never gets that far.
    CHECK(ask(ConnectionFacts { .host = "10.0.0.9" }).error == CompileCacheWire::ErrorCode::NotAMember);
}

TEST_CASE("A key roster forgets a revoked key and has no opinion about an address", "[distributed][membership][forget]")
{
    // The one participant that answers `Forgotten`, and it must admit nobody by ADDRESS: it is
    // composed into every node's participants, so an opinion escaping from `Explain` would widen or
    // narrow admission by where a caller dials from.
    auto keyOf = [](std::uint8_t fill) {
        auto key = Ed25519PublicKey {};
        key.fill(static_cast<std::byte>(fill));
        return key;
    };
    Distributed::KeyRosterMembership keys;
    keys.Publish({ { "n1", keyOf(0x11) }, { "n2", keyOf(0x22) } }, { keyOf(0x22), keyOf(0x33) });

    // A revoked key is the removed machine, asked FIRST: `n2`'s key is revoked though a record
    // still names it, and a revoked key is refused whatever id it now claims, by either evidence.
    for (auto const evidence: { Distributed::KeyEvidence::SessionProof, Distributed::KeyEvidence::MachineTicket })
    {
        auto const stale = keys.ExplainKey(ProvenIdentity { .id = "n2", .key = keyOf(0x22) }, evidence);
        CHECK(stale.verdict == Distributed::Membership::Forgotten);
        CHECK(stale.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone));
        CHECK(keys.ExplainKey(ProvenIdentity { .id = "n9", .key = keyOf(0x33) }, evidence).verdict
              == Distributed::Membership::Forgotten);
    }

    // The control: the live key of its own id is a member, attributed by HOW it was shown -- and a
    // key the roster does not hold for that id is no opinion at all, silence rather than a refusal.
    auto const proved =
        keys.ExplainKey(ProvenIdentity { .id = "n1", .key = keyOf(0x11) }, Distributed::KeyEvidence::SessionProof);
    CHECK(proved.verdict == Distributed::Membership::Member);
    CHECK(proved.decidedBy.Has(Distributed::MembershipParticipant::ProvenIdentity));
    auto const ticketed =
        keys.ExplainKey(ProvenIdentity { .id = "n1", .key = keyOf(0x11) }, Distributed::KeyEvidence::MachineTicket);
    CHECK(ticketed.verdict == Distributed::Membership::Member);
    CHECK(ticketed.decidedBy.Has(Distributed::MembershipParticipant::MachineTicket));
    CHECK(keys.ExplainKey(ProvenIdentity { .id = "n1", .key = keyOf(0x44) }, Distributed::KeyEvidence::SessionProof)
              .decidedBy.Empty());

    // No address is anything to it.
    CHECK(keys.Explain("10.0.0.2").decidedBy.Empty());
    CHECK(keys.Explain("10.0.0.2").verdict == Distributed::Membership::Outsider);
}
