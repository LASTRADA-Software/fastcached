// SPDX-License-Identifier: Apache-2.0
//
// Machines admitted by ticket, across a fleet: a launcher on another machine leased capacity on
// its ticket alone, every way a ticket is refused, a learner verifying one against the state it
// applied, and a forgotten machine. Driven through
// `FleetHarness`, because each is decided by two machines -- the one that minted the ticket and
// the one it was presented to -- over the production verifier and the production fold.
//
// **Every case calls from an address nobody listed** (`SetCallerHost`), FIRST: loopback is
// admitted before any other route is asked, so a case calling from the default would pass with
// the ticket route removed. What each case names as its RED is proven by neutering; the
// real-endpoint half -- AUTH's state machine over a socket -- is `FrameEndpoint_test`'s.
#include "NodeConfig.hpp"
#include "NodeMembership.hpp"

#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <tests/FleetHarness.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using FastCache::Testing::FleetHarness;
using FastCache::Testing::TestKeyPair;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The office's scheduler: the fleet's one voter, and its leader.
inline std::string const OfficeScheduler = "office.corp:6674";

/// The PC that compiles: registered with the office as a worker.
inline std::string const Pc = "pc-07.corp:6674";

/// The machine that asks: admitted to the cluster by key, and listed nowhere by address.
inline std::string const Laptop = "laptop";

/// A VPN address nobody listed anywhere, which only a ticket can admit.
inline std::string const VpnAddress = "10.20.0.33";

/// The toolchain every lease here asks for.
inline constexpr std::string_view Toolchain = "gcc-14";

/// Three machines: an office leader, a PC that compiles, a laptop that asks -- the laptop calling
/// from `VpnAddress`, so the address routes refuse it and only its ticket can admit it.
struct Office
{
    FleetHarness fleet;
    NullLogger logger;
    FastCache::Node::NodeMembership membership { FastCache::Node::NodeConfig {}, logger };

    Office()
    {
        fleet.SetCallerHost(VpnAddress);
        fleet.AddScheduler(OfficeScheduler);
        fleet.SetClusterStateAt(OfficeScheduler, FleetHarness::StateOf({ OfficeScheduler }, {}, 1));
        fleet.ElectLeader(OfficeScheduler);
        fleet.AdmitMachine(Laptop);
        fleet.RegisterWorker(OfficeScheduler, Pc, Toolchain);
        fleet.PublishMembershipAt(OfficeScheduler, membership);
    }

    /// A lease asked of the office, presenting @p credential first.
    /// @param credential What the launcher presents; unconfigured presents nothing.
    /// @return What the launcher's own client code made of the answer.
    [[nodiscard]] Cc::CacheOutcome Lease(Cc::Credential const& credential)
    {
        return fleet.Exchange(OfficeScheduler,
                              Wire::EncodeLease(Wire::LeaseRequest {
                                  .fingerprint = std::string { Toolchain }, .key = "k1", .acceptedCodecs = {} }),
                              credential,
                              Cc::ExchangeBudget {});
    }

    /// Let the PC's own presence rounds reach the office, as its proven session does in
    /// production: the harness runs no handshake, so the PC's identity is STATED, and it is live
    /// only because the cluster admitted the PC's key. Nothing about a ticket rides on it.
    void ProvePcRounds()
    {
        fleet.AdmitMachine(Pc);
        fleet.SetCallerIdentity(ProvenIdentity { .id = Pc, .key = TestKeyPair(Pc).PublicKey() });
    }
};

/// How many times each ticket refusal was counted, in enumerator order.
/// @param fleet Whose counters.
/// @return One count per `TicketRefusal`.
[[nodiscard]] std::vector<std::uint64_t> RefusalCounts(FleetHarness& fleet)
{
    auto counts = std::vector<std::uint64_t> {};
    for (auto const reason: Enumerators<Distributed::TicketRefusal>())
        counts.push_back(fleet.Metrics().Read(Distributed::DescribeTicketRefusal(reason).counter));
    return counts;
}

} // namespace

TEST_CASE("A launcher on another machine is leased capacity on its ticket alone", "[fleet][ticket][admission]")
{
    // RED when `CallerContextOf` stops folding the verified machine in: the lease is refused
    // `NotAMember`, since the address is one nobody listed.
    Office office;
    auto const leased = office.Lease(office.fleet.TicketFor(Laptop, OfficeScheduler));
    CHECK(leased.kind == Cc::CacheOutcomeKind::Hit);
    CHECK(office.fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsAccepted) == 1);
}

TEST_CASE("With no ticket the same machine is refused, by the same scheduler, as a stranger", "[fleet][ticket][admission]")
{
    // The control for the case above: the same address and the same scheduler, and nothing
    // presented. `NotAMember` is a refusal the scheduler deliberately does not count
    // (`UncountedRefusals`), so what moves nothing here is asserted as moving nothing.
    Office office;
    auto const refused = office.Lease(Cc::Credential {});
    CHECK(refused.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(refused.code == Wire::ErrorCode::NotAMember);
    CHECK(office.fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsAccepted) == 0);
    CHECK(RefusalCounts(office.fleet) == std::vector<std::uint64_t>(EnumeratorCount<Distributed::TicketRefusal>, 0));
}

TEST_CASE("Every ticket refusal is named on the wire and counted apart, across the fleet", "[fleet][ticket][admission]")
{
    struct Row
    {
        char const* what;
        std::function<Cc::Credential(Office&)> ticket;
        Distributed::TicketRefusal reason;
    };
    auto const rows = std::vector<Row> {
        { .what = "bytes that are not a ticket",
          .ticket =
              [](Office&) {
                  return Cc::Credential { .kind = Wire::AuthKind::MachineTicket,
                                          .username = {},
                                          .secret = SecureString { "not a ticket" } };
              },
          .reason = Distributed::TicketRefusal::Malformed },
        { .what = "another machine's key",
          .ticket = [](Office& o) { return FleetHarness::ForgeAs(o.fleet.TicketFor(Laptop, OfficeScheduler), "impostor"); },
          .reason = Distributed::TicketRefusal::Forged },
        { .what = "a machine never admitted",
          .ticket = [](Office& o) { return o.fleet.TicketFor("stranger", OfficeScheduler); },
          .reason = Distributed::TicketRefusal::UnknownMachine },
        { .what = "another node's audience",
          .ticket = [](Office& o) { return o.fleet.TicketFor(Laptop, Pc); },
          .reason = Distributed::TicketRefusal::WrongAudience },
        { .what = "a forgotten machine",
          .ticket =
              [](Office& o) {
                  o.fleet.ForgetMachine(Laptop);
                  return o.fleet.TicketFor(Laptop, OfficeScheduler);
              },
          .reason = Distributed::TicketRefusal::Revoked },
        { .what = "an expired ticket",
          .ticket =
              [](Office& o) {
                  auto ticket = o.fleet.TicketFor(Laptop, OfficeScheduler);
                  o.fleet.Step(Distributed::MachineTicketLifetime + Distributed::LeaseTokenClockSkewSlack
                               + std::chrono::seconds { 1 });
                  return ticket;
              },
          .reason = Distributed::TicketRefusal::Expired },
        { .what = "a ticket presented twice",
          .ticket =
              [](Office& o) {
                  auto ticket = o.fleet.TicketFor(Laptop, OfficeScheduler);
                  REQUIRE(o.Lease(ticket).kind == Cc::CacheOutcomeKind::Hit);
                  return ticket;
              },
          .reason = Distributed::TicketRefusal::Replayed },
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        Office office;
        auto const credential = row.ticket(office);
        auto const refused = office.Lease(credential);
        CHECK(refused.kind == Cc::CacheOutcomeKind::Rejected);
        CHECK(refused.code == Wire::ErrorCode::TicketRefused);
        // The words the row travels with: its name, or -- for the three that would tell a
        // stranger which ids the roster holds -- the one not-admitted message.
        CHECK(refused.message == Distributed::TicketRefusalMessage(Distributed::DescribeTicketRefusal(row.reason)));
        // Its own counter, once, and no other refusal's.
        auto expected = std::vector<std::uint64_t>(EnumeratorCount<Distributed::TicketRefusal>, 0);
        expected[static_cast<std::size_t>(row.reason)] = 1;
        CHECK(RefusalCounts(office.fleet) == expected);
    }
}

TEST_CASE("A learner verifies a ticket against the state it applied", "[fleet][ticket][roster]")
{
    // A worker verifying a ticket where it compiles, against the trust root it holds. That root
    // was a pure worker's certified roster until T24 retired the pure worker; it is now the state a
    // LEARNER applied. The learner asks its own verifier, so this stays GREEN with
    // `CallerContextOf`'s ticket fold removed -- the asymmetry with the lease case above is the
    // evidence that the fold, not the verifier, is what admits at a scheduler.
    //
    // RED with `NodeRoster::Applied` neutered (the learner applies nothing, so the laptop is
    // unknown to it), and with `StateLeaseRoster::Adopt` keeping no revoked key (the forgotten
    // laptop then reads as unknown rather than revoked).
    Office office;
    office.fleet.AdmitMachine(Pc);
    FleetHarness::LearnerWorker learner { office.fleet, Pc, OfficeScheduler };
    learner.Apply(); // the committed state, which admits the laptop

    auto const accepted = learner.CheckTicket(office.fleet.TicketFor(Laptop, Pc));
    REQUIRE(accepted.has_value());
    CHECK(accepted->id == Laptop);
    auto const elsewhere = learner.CheckTicket(office.fleet.TicketFor(Laptop, OfficeScheduler, 2));
    REQUIRE_FALSE(elsewhere.has_value());
    CHECK(elsewhere.error() == Distributed::TicketRefusal::WrongAudience);

    office.fleet.ForgetMachine(Laptop);
    learner.Apply(); // the forget is applied -- no restart
    auto const forgotten = learner.CheckTicket(office.fleet.TicketFor(Laptop, Pc, 3));
    REQUIRE_FALSE(forgotten.has_value());
    CHECK(forgotten.error() == Distributed::TicketRefusal::Revoked);
}

TEST_CASE("A learner's verdict on a ticket follows the state it applied, never a roster's lapse", "[fleet][ticket][roster]")
{
    // The pure worker's version of this case asked a roster that LAPSES: asleep past its
    // certification, the worker refused every ticket as no-roster until a voter endorsed again. A
    // learner's trust root is the state it applied, which carries no certificate to lapse -- what
    // it can be is BEHIND, and a machine admitted since it last applied is one it does not know.
    //
    // RED with `NodeRoster::Applied` neutered (the second machine stays unknown after the apply),
    // and with `NodeRoster::Build`'s consensus branch removed (the learner then holds a certified
    // roster's trust, of which it has none, and refuses every ticket as no-roster).
    Office office;
    office.fleet.AdmitMachine(Pc);
    FleetHarness::LearnerWorker learner { office.fleet, Pc, OfficeScheduler };
    learner.Apply();
    REQUIRE(learner.CheckTicket(office.fleet.TicketFor(Laptop, Pc)).has_value());

    // A second machine admitted after the learner last applied: behind, not lapsed.
    std::string const desktop = "desktop";
    office.fleet.AdmitMachine(desktop);
    auto const behind = learner.CheckTicket(office.fleet.TicketFor(desktop, Pc));
    REQUIRE_FALSE(behind.has_value());
    CHECK(behind.error() == Distributed::TicketRefusal::UnknownMachine);
    learner.Apply();
    CHECK(learner.CheckTicket(office.fleet.TicketFor(desktop, Pc, 2)).has_value());

    // Asleep past what a certified roster's endorsement and its slack would have allowed: the
    // state it applied is still the state it applied.
    office.fleet.Step(Cluster::RosterEndorsementLifetime + std::chrono::hours { 1 });
    CHECK(learner.CheckTicket(office.fleet.TicketFor(Laptop, Pc, 4)).has_value());
}

TEST_CASE("A forgotten machine's ticket is refused by the scheduler from any address with no restart",
          "[fleet][ticket][admission]")
{
    // RED when the verifier stops recognising a revoked key's signature as the forgotten machine's:
    // the ticket is still refused, but as an UNKNOWN machine -- which sends an operator to admit one
    // that was removed on purpose -- and the revoked counter stays at zero.
    Office office;
    REQUIRE(office.Lease(office.fleet.TicketFor(Laptop, OfficeScheduler)).kind == Cc::CacheOutcomeKind::Hit);
    office.fleet.ForgetMachine(Laptop);

    struct Attempt
    {
        std::string host;
        std::uint8_t nonce;
    };
    for (auto const& [host, nonce]:
         { Attempt { .host = VpnAddress, .nonce = 2 }, Attempt { .host = "10.20.0.34", .nonce = 3 } })
    {
        INFO(host);
        office.fleet.SetCallerHost(host);
        auto const refused = office.Lease(office.fleet.TicketFor(Laptop, OfficeScheduler, nonce));
        CHECK(refused.kind == Cc::CacheOutcomeKind::Rejected);
        CHECK(refused.code == Wire::ErrorCode::TicketRefused);
    }
    CHECK(office.fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsRefusedRevoked) == 2);
    CHECK(office.fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsAccepted) == 1);
}
