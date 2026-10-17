// SPDX-License-Identifier: Apache-2.0
#include "AnnounceTestFixture.hpp"
#include "CompileCapacity.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Dispatch.hpp>
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
constexpr std::string_view Laptop = "laptop.corp:6677"; // what the worker ADVERTISES
constexpr std::string_view Toolchain = "msvc-19.44";
constexpr std::string_view LaptopMachine = "laptop"; // the worker machine's id, which its grants name

/// The worker's slots: two, so with one compile running it has a slot free ON PAPER, and only the
/// byte budget can say it is full -- the shape #1667 measured, where the scheduler counted slots.
constexpr std::uint32_t Slots = 2;

/// Its `--compile-inflight-max`: small, so one large unit holds most of it.
constexpr std::size_t Budget = 64ULL * 1024ULL * 1024ULL;

/// What the one running compile holds: three quarters of the budget, so the quarter left cannot
/// hold another compile of the size this worker's running one is (the only per-job figure a lease
/// gives the scheduler to divide by).
constexpr std::size_t Held = 48ULL * 1024ULL * 1024ULL;
static_assert(Budget - Held < Held, "the budget left must not hold another compile of the held size");

/// The job every dispatch asks for, under its own key so duplicate suppression stays out.
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
} // namespace

TEST_CASE("A worker whose compiles hold its byte budget is refused at the scheduler, not at its endpoint",
          "[node][fleet][byte-budget]")
{
    // #1667's acceptance, end to end. The worker's heartbeat is production's `AnnounceRound` over a
    // real `CompileCapacity` with a small `--compile-inflight-max` and a large compile in flight, so
    // the budget the scheduler prices is the one the worker's endpoint would spend. The next LEASE is
    // refused at the SCHEDULER -- `no-capacity`, bound by `compile-budget`, never `withdrawn` -- and
    // no COMPILE is sent to the worker, so its endpoint has nothing to refuse `EndpointBusy` and its
    // `worker_jobs_refused_endpoint_busy` counter cannot move: the worker address here is the
    // harness's, and what stands for that counter is that nothing reached it.
    //
    // RED when the ceiling is dropped (`SlotCeilingsFor`'s compile-budget block): two slots and one
    // compile, so the scheduler grants the lease and the compile goes to a worker whose budget is
    // held -- the refusal #1667 measured landing at the endpoint, half the grants of a build.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { Sched });
    fleet.ElectLeader(Sched);
    fleet.SetClusterStateAt(Sched, Testing::FleetHarness::StateOf({ std::string { Sched } }, {}, 1));
    // The worker is a fleet machine: its heartbeat round proves its own key, as production's does over
    // a node proof -- the harness runs no handshake, so the case states the proof.
    fleet.AdmitMachine(std::string { LaptopMachine });
    fleet.AddWorkerAddress(std::string { Laptop },
                           Testing::FleetHarness::CompiledReply(Ask({}), std::string { LaptopMachine }));

    auto const cfg = FastCache::Testing::LearnerRegisteringWith(Node::NodeConfig {}, { std::string { Sched } });
    AtomicMetricsSink workerMetrics;
    NullLogger logger;
    Node::AnnounceTesting::SilentLoadSampler loadSampler;
    Testing::ScriptedHostAddresses machine { { "127.0.0.1" } };
    core::platform::ManualClock clock;
    CachedLocalityOracle const locality { machine, clock };
    Node::CompileCapacity capacity { Slots, Budget, std::chrono::seconds { 1 }, logger };
    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    std::vector<Cc::WorkerRegistrar> registrars;
    registrars.emplace_back(
        std::string { Toolchain }, std::string { Laptop }, Slots, Wire::CodecList {}, Wire::CapacityFields {});
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
    auto const announce = [&] {
        fleet.SetCallerIdentity(ProvenIdentity { .id = std::string { LaptopMachine },
                                                 .key = Testing::TestKeyPair(std::string { LaptopMachine }).PublicKey() });
        auto const accepted = Node::AnnounceRound(round, link, fleet);
        fleet.SetCallerIdentity(std::nullopt); // the launcher's exchanges prove nothing
        return accepted;
    };
    REQUIRE(announce() == 1); // registers; a registration carries no load record

    // One large compile running: its slot taken and its request's bytes held, as the endpoint holds them.
    REQUIRE(capacity.TryTakeSlot() == Node::SlotAdmission::Taken);
    auto held = capacity.TryTakeBytes(Held);
    REQUIRE(held.has_value());
    REQUIRE(announce() == 1); // the heartbeat that carries the budget and what is held of it

    // The scheduler's own view of the machine names the budget as what binds it.
    auto const machines = fleet.MachinesAt(Sched);
    REQUIRE(machines.size() == 1);
    CHECK(Distributed::SlotCeilingsFor(machines[0].capacity, Slots, machines[0].load).binding
          == Distributed::SlotLimit::CompileBudget);

    auto before = fleet.Calls().size();
    auto const refused = Cc::Dispatch(fleet, Ask("k1"));
    REQUIRE(refused.decline == Cc::DeclineCause::NoCapacity);
    CHECK_FALSE(refused.Ran());
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 1);
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::DispatchLeasesWithdrawn) == 0);
    CHECK(fleet.CompileCallsSince(before).empty());

    // The control: the compile finishes, its bytes and its slot come back, and the next heartbeat says
    // so -- a lease is granted and the compile reaches the worker. Without this the refusal above
    // could be a worker the scheduler never offers at all.
    held.reset();
    capacity.ReleaseSlot();
    REQUIRE(announce() == 1);
    before = fleet.Calls().size();
    auto const granted = Cc::Dispatch(fleet, Ask("k2"));
    REQUIRE(granted.Ran());
    CHECK(fleet.CompiledAt(before) == std::vector<std::string> { std::string { Laptop } });
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 1);
}
