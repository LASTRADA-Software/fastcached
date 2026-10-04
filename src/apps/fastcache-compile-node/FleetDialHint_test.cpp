// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"
#include "NodeAnnounce.hpp"
#include "NodeAudience.hpp"
#include "NodeConfig.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CompileCorrelation.hpp>
#include <Dispatch.hpp>
#include <ReachabilityMemo.hpp>
#include <ReachabilityMemoTestSupport.hpp>
#include <TicketCredentials.hpp>
#include <core/platform/Clock.hpp>
#include <tests/FleetHarness.hpp>
#include <tests/LocalityFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;

namespace
{
namespace Wire = CompileCacheWire;

constexpr std::string_view Sched = "sched-a:6676";
constexpr std::string_view Laptop = "laptop.corp:6677";   // what the laptop ADVERTISES
constexpr std::string_view OldHost = "10.8.0.7";          // where the laptop was before the reconnect
constexpr std::string_view NewHost = "10.8.0.42";         // where the VPN put it
constexpr std::string_view NewAddress = "10.8.0.42:6677"; // the hint the scheduler derives from it
constexpr std::string_view Toolchain = "msvc-19.44";
constexpr std::string_view Builder = "builder";      // the machine whose launcher dispatches, by its id
constexpr std::string_view LaptopMachine = "laptop"; // the worker machine's id, which its audience names
constexpr std::chrono::system_clock::time_point Noon { std::chrono::seconds { 1'767'225'600 } };

/// The job every case dispatches, under its own key so duplicate suppression stays out.
/// @param key The object key.
/// @return The request.
[[nodiscard]] Cc::DispatchRequest Ask(std::string_view key)
{
    return Cc::DispatchRequest { .schedulerEndpoint = Sched,
                                 .fingerprint = Toolchain,
                                 .objectKey = key,
                                 .args = {},
                                 .family = Cc::DriverFamily::Gnu,
                                 .preprocessed = "int main() { return 0; }",
                                 .sourceName = "main.cpp",
                                 .compileDir = {},
                                 .compileDirReplacement = {},
                                 .sourceRoot = {},
                                 .sourceRootReplacement = {} };
}

/// The reply an HONEST worker sends for `Ask`'s job: an object, and the correlation the
/// launcher recomputes, signed under the key the grant names (W-4) -- so a case asserting
/// `Ran()` is asserting that the compile was accepted, not merely that some address answered.
/// @param signer Whose identity key signs it: the registered worker's unless a case is about an
///        impostor; empty for a reply nobody signed.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> CompiledReply(std::optional<std::string> const& signer = std::string {
                                                       Testing::FleetHarness::ProvenMachine })
{
    constexpr std::string_view Object = "OBJ";
    auto const request = Ask({});
    auto const correlation =
        Cc::CompileCorrelation(Cc::CorrelatedCompile { .preprocessed = request.preprocessed,
                                                       .args = request.args,
                                                       .fingerprint = request.fingerprint,
                                                       .sourceName = request.sourceName,
                                                       .compileDir = request.compileDir,
                                                       .compileDirReplacement = request.compileDirReplacement,
                                                       .sourceRoot = request.sourceRoot,
                                                       .sourceRootReplacement = request.sourceRootReplacement });
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(Object.size()), Wire::AsBytes(Object));
    if (signer.has_value())
        return Testing::FleetHarness::SignedWorkerReply(enveloped, correlation, *signer);
    return Wire::EncodeReply(Wire::Status::Ok,
                             Wire::EncodeCompileResult(Wire::CompileResult { .exitCode = 0,
                                                                             .object = enveloped,
                                                                             .stdoutText = {},
                                                                             .stderrText = {},
                                                                             .correlation = Wire::AsBytes(correlation),
                                                                             .signature = {} }));
}

/// A leader with the laptop registered under its NAME and last seen at `NewHost`, so every
/// grant carries `NewAddress` as its hint.
/// @param fleet The harness to arrange.
void LaptopMovedTo(Testing::FleetHarness& fleet)
{
    fleet.AddScheduler(std::string { Sched });
    fleet.ElectLeader(Sched);
    auto const id = fleet.RegisterWorkerNamed(Sched, Laptop, Toolchain);
    fleet.HeartbeatFrom(Sched, id, std::string { NewHost }, { std::string { NewHost } });
}

/// How many RELEASE exchanges went out from @p from on, and to whom.
/// @param fleet The harness whose call log to read.
/// @param from How many calls to skip.
/// @return The endpoint of each RELEASE, in order.
[[nodiscard]] std::vector<std::string> ReleasesSince(Testing::FleetHarness const& fleet, std::size_t from)
{
    auto const releaseOp = static_cast<std::uint8_t>(Wire::Op::Release);
    std::vector<std::string> releases;
    for (auto const& call: std::span { fleet.Calls() }.subspan(from))
        if (call.opRaw == releaseOp)
            releases.push_back(call.endpoint);
    return releases;
}

/// One launcher PROCESS with its reachability memo: load it, dispatch with its exclusions,
/// absorb what happened, save -- the order `TryRemoteCompile` runs in.
/// @param fleet The fleet to dispatch into.
/// @param store The memo launchers share.
/// @param key The object key.
/// @return What the dispatch returned.
[[nodiscard]] Cc::DispatchResult OneLauncher(Testing::FleetHarness& fleet,
                                             Cc::Testing::InMemoryMemoStore& store,
                                             std::string_view key)
{
    auto memo = Cc::ReachabilityMemo::Load(store);
    auto const excluded = memo.Fresh(Cc::MemoKind::WorkerUnreached, Noon);
    auto request = Ask(key);
    request.excludedWorkers = excluded;
    auto const result = Cc::Dispatch(fleet, request);
    memo.Absorb(result, Sched, Noon);
    memo.SaveIfChanged(store);
    return result;
}

/// A launcher's tickets: one per dial, minted for the endpoint that dial names -- the question
/// `Cc::CredentialedExchange` asks production's minter, answered here by the harness minting as
/// @p machine would.
class TicketPerDial final: public Cc::ICredentialFor
{
  public:
    /// @param fleet Where the machine's test key mints.
    /// @param machine Who the tickets speak for.
    TicketPerDial(Testing::FleetHarness& fleet, std::string_view machine):
        _fleet { fleet },
        _machine { machine }
    {
    }

    /// @copydoc Cc::ICredentialFor::Present
    [[nodiscard]] Cc::PresentedCredential Present(std::string_view audience) override
    {
        _audiences.emplace_back(audience);
        ++_nonce; // a node spends each ticket once
        return Cc::PresentedCredential { .credential = _fleet.TicketFor(_machine, std::string { audience }, _nonce),
                                         .missing = std::nullopt };
    }

    /// @return Every audience a ticket was minted for, in order.
    [[nodiscard]] std::vector<std::string> const& Audiences() const noexcept
    {
        return _audiences;
    }

  private:
    Testing::FleetHarness& _fleet;
    std::string _machine;
    std::uint8_t _nonce { 0 };
    std::vector<std::string> _audiences;
};

/// The laptop moved, its name dead, a ticket-presenting launcher on `Builder` admitted to the
/// cluster -- and the laptop's address verifying the tickets presented there over @p audience.
/// @param fleet The harness to arrange.
/// @param audience What the laptop answers to; borrowed.
void TicketedLaptopMovedTo(Testing::FleetHarness& fleet, Distributed::IAudience const& audience)
{
    LaptopMovedTo(fleet);
    fleet.SetClusterStateAt(Sched, Testing::FleetHarness::StateOf({ std::string { Sched } }, {}, 1));
    fleet.AdmitMachine(std::string { Builder });
    fleet.SetWorkerUnreachable(Laptop, true);
    fleet.AddWorkerAddress(std::string { NewAddress }, CompiledReply());
    fleet.VerifyTicketsAtWorker(std::string { NewAddress }, audience, Sched);
}

/// A load sampler that reports nothing: what a heartbeat SAYS about load decides nothing here.
class SilentLoadSampler final: public IHostLoadSampler
{
  public:
    [[nodiscard]] HostLoad Sample() override
    {
        return HostLoad {};
    }
};
} // namespace

TEST_CASE("After a VPN reconnect the compile reaches the laptop at its NEW address", "[node][fleet][dialhint]")
{
    Testing::FleetHarness fleet;
    Cc::Testing::InMemoryMemoStore store;
    LaptopMovedTo(fleet);
    // The name still resolves to the OLD address, which nothing answers any more...
    fleet.SetWorkerUnreachable(Laptop, true);
    // ...while the laptop answers at the address the scheduler saw it heartbeat from.
    fleet.AddWorkerAddress(std::string { NewAddress }, CompiledReply());

    auto const before = fleet.Calls().size();
    auto const result = OneLauncher(fleet, store, "k1");
    REQUIRE(result.Ran());
    CHECK(result.dialledEndpoint == NewAddress);
    CHECK(result.workerEndpoint == Laptop);
    // Exactly one COMPILE, at the new address: the dead name was never dialled.
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { NewAddress } });
    // One release, to the scheduler that issued the lease, and the key is free again.
    CHECK(ReleasesSince(fleet, before) == std::vector<std::string> { std::string { Sched } });
    CHECK_FALSE(fleet.IsInFlight(Sched, "k1"));
    // Nothing was unreachable that was ever dialled: the memo learns nothing.
    CHECK(Cc::ReachabilityMemo::Load(store).Fresh(Cc::MemoKind::WorkerUnreached, Noon).empty());
}

TEST_CASE("An impostor at the hint address answers with a valid-looking object, and it is refused and never stored",
          "[node][fleet][dialhint][reply-seal]")
{
    // W-4: between the heartbeat and this compile the laptop's VPN address went to a machine that
    // is NOT a fleet worker. It accepts the job and returns a well-formed, correctly correlated
    // object -- which used to be cached here and written through to the fleet's shared tier. The
    // grant names the laptop's key, so a reply signed by anybody else, or by nobody, is refused.
    for (auto const& [what, signer]:
         { std::pair { "signed by another machine's key", std::optional<std::string> { "impostor" } },
           std::pair { "carrying no signature", std::optional<std::string> {} } })
    {
        INFO(what);
        Testing::FleetHarness fleet;
        Cc::Testing::InMemoryMemoStore store;
        LaptopMovedTo(fleet);
        fleet.SetWorkerUnreachable(Laptop, true);
        fleet.AddWorkerAddress(std::string { NewAddress }, CompiledReply(signer));

        auto const before = fleet.Calls().size();
        auto const result = OneLauncher(fleet, store, "k-impostor");
        // Refused by name, and its object is not used. This is the seam "nothing stored" is
        // decided at: `Dispatch` itself sends no STORE either way, and the launcher stores a
        // DISPATCHED object only from an outcome that `Ran()` -- a refused one compiles locally
        // and stores that. So the next three assertions are what keep the impostor's object out
        // of every tier, and the ones a neutered check turns red.
        CHECK(result.status == Cc::DispatchStatus::Unauthenticated);
        CHECK_FALSE(result.Ran());
        CHECK(result.object.empty());
        CHECK(result.detail.contains("not signed by the worker the grant named"));
        // It did reach the impostor -- the refusal is the reply's, not the dial's -- and the name
        // was not dialled after it: a second dial after a machine answered could compile twice.
        CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { NewAddress } });
        // Released like every other way out of a granted lease.
        CHECK(ReleasesSince(fleet, before) == std::vector<std::string> { std::string { Sched } });
        // Counted: the launcher's tally records it under a state of its own.
        auto const recorded = Cc::RecordingFor(result.status, Cc::DeclineCause {});
        CHECK(recorded.outcome == Cc::DispatchOutcome::Unauthenticated);
        CHECK(Cc::ToStringView(recorded.outcome) == "UNAUTHENTICATED");
    }
}

TEST_CASE("A grant names the key the worker proved at registration, and an honest reply under it is used",
          "[node][fleet][dialhint][reply-seal]")
{
    // The control for the case above, through the SCHEDULER's own grant: the key it names is the
    // one the worker's REGISTER connection proved, so the same worker's signed reply is accepted.
    Testing::FleetHarness fleet;
    Cc::Testing::InMemoryMemoStore store;
    LaptopMovedTo(fleet);
    fleet.SetWorkerUnreachable(Laptop, true);
    fleet.AddWorkerAddress(std::string { NewAddress }, CompiledReply());

    auto const result = OneLauncher(fleet, store, "k-honest");
    REQUIRE(result.Ran());
    CHECK(result.object == std::vector<std::byte> { std::byte { 'O' }, std::byte { 'B' }, std::byte { 'J' } });
}

TEST_CASE("A stale hint that lands on ANOTHER worker is refused there, and the name compiles", "[node][fleet][dialhint]")
{
    Testing::FleetHarness fleet;
    LaptopMovedTo(fleet);
    // Between the heartbeat and this compile, the address went to another fleet worker,
    // whose lease check compares the token's endpoint with ITS OWN name.
    fleet.AddWorkerAddress(
        std::string { NewAddress },
        Wire::EncodeErrorReply(Wire::ErrorCode::LeaseEndpointMismatch, "this lease names laptop.corp:6677"));
    fleet.AddWorkerAddress(std::string { Laptop }, CompiledReply());

    auto const before = fleet.Calls().size();
    auto const result = Cc::Dispatch(fleet, Ask("k2"));
    auto const compiles = fleet.CompileCallsSince(before);
    REQUIRE(compiles.size() == 2);
    CHECK(compiles[0].endpoint == NewAddress);
    CHECK(compiles[0].kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(compiles[0].code == Wire::ErrorCode::LeaseEndpointMismatch);
    CHECK(compiles[1].endpoint == Laptop);
    CHECK(compiles[1].kind == Cc::CacheOutcomeKind::Hit);
    REQUIRE(result.Ran());
    CHECK(result.dialledEndpoint == Laptop);
    CHECK(result.workerEndpoint == Laptop);
    // Two dials, one lease: released once, to its issuer.
    CHECK(ReleasesSince(fleet, before) == std::vector<std::string> { std::string { Sched } });
    CHECK_FALSE(fleet.IsInFlight(Sched, "k2"));
}

TEST_CASE("A dead hint with a live name teaches the memo nothing", "[node][fleet][dialhint][exclusion]")
{
    // The hint reached nothing and the name compiled: a stale ADDRESS, not a dead machine,
    // so the next launcher must still be granted the laptop.
    Testing::FleetHarness fleet;
    Cc::Testing::InMemoryMemoStore store;
    LaptopMovedTo(fleet);
    fleet.SetWorkerUnreachable(NewAddress, true);
    fleet.AddWorkerAddress(std::string { Laptop }, CompiledReply());

    auto const before = fleet.Calls().size();
    auto const result = OneLauncher(fleet, store, "k3");
    REQUIRE(result.Ran());
    CHECK(result.dialledEndpoint == Laptop);
    CHECK(result.unreachedWorker.empty());
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { NewAddress }, std::string { Laptop } });
    CHECK(ReleasesSince(fleet, before) == std::vector<std::string> { std::string { Sched } });
    CHECK(Cc::ReachabilityMemo::Load(store).Fresh(Cc::MemoKind::WorkerUnreached, Noon).empty());

    // And the next launcher is granted the laptop again.
    auto const next = fleet.Calls().size();
    CHECK(OneLauncher(fleet, store, "k4").Ran());
    CHECK_FALSE(fleet.CompiledAt(next).empty());
}

TEST_CASE("A dead hint AND a dead name exclude the laptop by its name", "[node][fleet][dialhint][exclusion]")
{
    Testing::FleetHarness fleet;
    Cc::Testing::InMemoryMemoStore store;
    LaptopMovedTo(fleet);
    fleet.SetWorkerUnreachable(NewAddress, true);
    fleet.SetWorkerUnreachable(Laptop, true);

    auto const before = fleet.Calls().size();
    auto const result = OneLauncher(fleet, store, "k5");
    CHECK(result.status == Cc::DispatchStatus::Unavailable);
    CHECK(result.unreachedWorker == Laptop);
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { NewAddress }, std::string { Laptop } });
    CHECK(ReleasesSince(fleet, before) == std::vector<std::string> { std::string { Sched } });
    CHECK_FALSE(fleet.IsInFlight(Sched, "k5"));
    // The memo names the MACHINE, by what the scheduler's registry keys on -- never the hint.
    auto const excluded = Cc::ReachabilityMemo::Load(store).Fresh(Cc::MemoKind::WorkerUnreached, Noon);
    CHECK(excluded == std::vector<std::string> { std::string { Laptop } });
}

TEST_CASE("A compile dialled at a grant's hint presents a ticket the worker takes as its own",
          "[node][fleet][dialhint][ticket]")
{
    // The launcher mints each ticket for the endpoint it DIALS, so a compile sent to a hint names
    // the hint's ADDRESS -- never the name the worker advertises -- and the worker spends it only
    // because its audience answers to an address its locality oracle calls this machine
    // (`AudienceMatches`). Production's `NodeAudience` throughout: the laptop's advertised name,
    // its host name, its port, and the one address it answers on now.
    //
    // RED when `AudienceMatches` stops asking the locality oracle: the hint's ticket is refused
    // `wrong-audience` and the build compiles locally. The control below stays green under that.
    Testing::FleetHarness fleet;
    Node::AnnouncedEndpoint const advertised { Laptop };
    Testing::ThisMachineIs const locality { NewHost };
    Node::NodeAudience const audience { advertised, "laptop", { 6677 }, locality };
    TicketedLaptopMovedTo(fleet, audience);

    TicketPerDial tickets { fleet, Builder };
    Cc::CredentialedExchange credentialed { fleet, tickets };
    auto const before = fleet.Calls().size();
    auto const result = Cc::Dispatch(credentialed, Ask("k5"));
    REQUIRE(result.Ran());
    CHECK(result.dialledEndpoint == NewAddress);
    CHECK(result.workerEndpoint == Laptop);
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { NewAddress } });
    // The lease and its release at the scheduler, the compile at the hint -- and the hint's ticket
    // named the address, which is the half that needed the locality oracle.
    CHECK(tickets.Audiences()
          == std::vector<std::string> { std::string { Sched }, std::string { NewAddress }, std::string { Sched } });
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsAccepted) == 3);
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsRefusedWrongAudience) == 0);
}

TEST_CASE("A worker whose addresses do not include the hint refuses its ticket as another machine's",
          "[node][fleet][dialhint][ticket]")
{
    // The control for the case above, and what proves the worker's check bites in this harness: the
    // same fleet and the same dial, with a locality oracle that knows none of the laptop's
    // addresses. The ticket names an endpoint the laptop cannot call its own, so the compile is
    // refused there -- and something RAN, so the name is not dialled afterwards.
    Testing::FleetHarness fleet;
    Node::AnnouncedEndpoint const advertised { Laptop };
    Testing::ThisMachineIs const locality {};
    Node::NodeAudience const audience { advertised, "laptop", { 6677 }, locality };
    TicketedLaptopMovedTo(fleet, audience);

    TicketPerDial tickets { fleet, Builder };
    Cc::CredentialedExchange credentialed { fleet, tickets };
    auto const before = fleet.Calls().size();
    auto const result = Cc::Dispatch(credentialed, Ask("k6"));
    CHECK_FALSE(result.Ran());
    auto const compiles = fleet.CompileCallsSince(before);
    REQUIRE(compiles.size() == 1);
    CHECK(compiles[0].endpoint == NewAddress);
    CHECK(compiles[0].kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(compiles[0].code == Wire::ErrorCode::TicketRefused);
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsRefusedWrongAudience) == 1);
    CHECK_FALSE(fleet.IsInFlight(Sched, "k6"));
}

TEST_CASE("A heartbeat reports what the audience accepts, so a hint names a new address only once the audience knows it",
          "[node][fleet][dialhint][ticket]")
{
    // The laptop's heartbeat as `main` builds it -- production's round over the ONE locality oracle
    // its ticket audience answers from -- announcing into the fleet. The VPN reconnects: the
    // scheduler sees the rounds arrive from the new address at once, but the oracle knows it only
    // at its refresh. Until then the report still names the old set, so the scheduler vetoes the
    // hint (`NotAReportedInterface`) and the compile goes to the NAME; after it, the hint names the
    // new address and its ticket is accepted there, because the audience reads the same set.
    //
    // RED when the report is a second, independent acquisition (hand the round a `CachedLocalityOracle`
    // of its own, interval zero, over `machine`): the first compile goes to the new address and the
    // audience, still on the old set, refuses its ticket `wrong-audience` -- a refusal is final, so
    // that build compiles locally.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { Sched });
    fleet.ElectLeader(Sched);
    fleet.SetClusterStateAt(Sched, Testing::FleetHarness::StateOf({ std::string { Sched } }, {}, 1));
    fleet.AdmitMachine(std::string { Builder });
    // The laptop is a fleet machine too: its presence round proves its own key, as production's
    // does over a node proof -- the harness runs no handshake, so the case states the proof.
    fleet.AdmitMachine(std::string { LaptopMachine });

    Testing::ScriptedHostAddresses machine { { std::string { OldHost } } };
    core::platform::ManualClock clock;
    CachedLocalityOracle const locality { machine, clock };
    Node::AnnouncedEndpoint const advertised { Laptop };
    Node::NodeAudience const audience { advertised, "laptop", { 6677 }, locality };
    // Signed as the laptop: its presence round proves the laptop's key, so that is the key its grants
    // name and the key its replies must be signed under (W-4).
    fleet.AddWorkerAddress(std::string { Laptop }, CompiledReply(std::string { LaptopMachine }));
    fleet.AddWorkerAddress(std::string { NewAddress }, CompiledReply(std::string { LaptopMachine }));
    fleet.VerifyTicketsAtWorker(std::string { Laptop }, audience, Sched);
    fleet.VerifyTicketsAtWorker(std::string { NewAddress }, audience, Sched);

    auto const cfg = FastCache::Testing::LearnerRegisteringWith(Node::NodeConfig {}, { std::string { Sched } });
    AtomicMetricsSink workerMetrics;
    NullLogger logger;
    SilentLoadSampler loadSampler;
    Node::CompileCapacity capacity { /*slots=*/1, /*byteBudget=*/1024ULL, std::chrono::seconds { 1 }, logger };
    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    std::vector<Cc::WorkerRegistrar> registrars;
    registrars.emplace_back(
        std::string { Toolchain }, std::string { Laptop }, 1U, Wire::CodecList {}, Wire::CapacityFields {});
    std::vector<Cc::WorkerRegistrar> withdrawals;
    std::atomic<bool> addressCapNoticed { false };
    Node::SchedulerReachability reachability { clock };
    auto link = Testing::Unwrap(Node::SchedulerLink::For(Node::SchedulersOf(cfg, Node::AsConfigured)));
    Node::HeartbeatRound const round { .cfg = cfg,
                                       .registrars = registrars,
                                       .withdrawals = withdrawals,
                                       .capacity = capacity,
                                       .loadSampler = loadSampler,
                                       .locality = locality,
                                       .addressCapNoticed = addressCapNoticed,
                                       .cacheTier = nullptr,
                                       .metrics = workerMetrics,
                                       // The harness runs no handshake: `announce` states the proof.
                                       .prover = nullptr,
                                       .lease = lease,
                                       .logger = logger,
                                       .reachability = reachability };

    // The reconnect: the machine answers at the new address, and its rounds arrive from there.
    machine.Publish({ std::string { NewHost } });
    fleet.SetCallerHost(std::string { NewHost });
    auto const announce = [&] {
        fleet.SetCallerIdentity(ProvenIdentity { .id = std::string { LaptopMachine },
                                                 .key = Testing::TestKeyPair(std::string { LaptopMachine }).PublicKey() });
        auto const registered = Node::AnnounceRound(round, link, fleet);
        fleet.SetCallerIdentity(std::nullopt); // the launcher's exchanges prove nothing; its ticket speaks
        return registered;
    };
    REQUIRE(announce() == 1); // registers, reporting the oracle's set

    TicketPerDial tickets { fleet, Builder };
    Cc::CredentialedExchange credentialed { fleet, tickets };

    // Before the refresh: no hint names the new address, and the name compiles.
    auto before = fleet.Calls().size();
    auto const unrefreshed = Cc::Dispatch(credentialed, Ask("k7"));
    REQUIRE(unrefreshed.Ran());
    CHECK(unrefreshed.dialledEndpoint == Laptop);
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { Laptop } });

    // The oracle's interval passes; the next heartbeat reports the new address, and a hinted
    // compile's ticket is accepted there.
    clock.advance(CachedLocalityOracle::DefaultRefreshInterval);
    REQUIRE(announce() == 1);
    before = fleet.Calls().size();
    auto const refreshed = Cc::Dispatch(credentialed, Ask("k8"));
    REQUIRE(refreshed.Ran());
    CHECK(refreshed.dialledEndpoint == NewAddress);
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { NewAddress } });
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::NodeTicketsRefusedWrongAudience) == 0);
}
