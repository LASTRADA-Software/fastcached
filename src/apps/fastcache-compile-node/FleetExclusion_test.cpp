// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <Dispatch.hpp>
#include <ReachabilityMemo.hpp>
#include <ReachabilityMemoTestSupport.hpp>
#include <tests/FleetHarness.hpp>

using namespace FastCache;

namespace
{
constexpr std::string_view Sched = "sched-a:6676";
constexpr std::string_view Laptop = "laptop.corp:6677";
constexpr std::string_view Desk = "desk.corp:6677";
constexpr std::string_view Toolchain = "msvc-19.44";
constexpr std::chrono::system_clock::time_point Noon { std::chrono::seconds { 1'767'225'600 } };

/// One launcher PROCESS: load the shared memo, dispatch with its exclusions, absorb, save.
///
/// The order `TryRemoteCompile` runs in, with the store standing in for the memo FILE two
/// launcher processes share -- so what one launcher learned reaches the next only through
/// the store, never through an object both of them hold.
/// @param fleet The fleet to dispatch into.
/// @param store The memo both launchers read and write.
/// @param key The object key; a new one per launcher, so duplicate suppression stays out.
/// @param now The launcher's wall clock.
/// @return What the dispatch returned.
[[nodiscard]] Cc::DispatchResult OneLauncher(Testing::FleetHarness& fleet,
                                             Cc::Testing::InMemoryMemoStore& store,
                                             std::string_view key,
                                             std::chrono::system_clock::time_point now)
{
    auto memo = Cc::ReachabilityMemo::Load(store);
    auto const excluded = memo.Fresh(Cc::MemoKind::WorkerUnreached, now);
    auto const result = Cc::Dispatch(fleet,
                                     Cc::DispatchRequest { .schedulerEndpoint = Sched,
                                                           .fingerprint = Toolchain,
                                                           .objectKey = key,
                                                           .args = {},
                                                           .family = Cc::DriverFamily::Gnu,
                                                           .preprocessed = "int main() { return 0; }",
                                                           .sourceName = "main.cpp",
                                                           .compileDir = {},
                                                           .compileDirReplacement = {},
                                                           .sourceRoot = {},
                                                           .sourceRootReplacement = {},
                                                           .excludedWorkers = excluded });
    memo.Absorb(result, Sched, now);
    memo.SaveIfChanged(store);
    return result;
}

/// A COMPILE frame; what it asks for does not matter to a harness worker address.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> AnyCompile()
{
    return CompileCacheWire::EncodeCompile(CompileCacheWire::CompileRequest { .leaseToken = "t",
                                                                              .fingerprint = Toolchain,
                                                                              .args = {},
                                                                              .source = {},
                                                                              .acceptedCodecs = {},
                                                                              .sourceName = "t.cpp",
                                                                              .compileDir = {},
                                                                              .compileDirReplacement = {},
                                                                              .sourceRoot = {},
                                                                              .sourceRootReplacement = {} });
}
} // namespace

TEST_CASE("A worker one launcher could not reach is excluded by the next, until it expires", "[node][fleet][exclusion]")
{
    Testing::FleetHarness fleet;
    Cc::Testing::InMemoryMemoStore store;
    fleet.AddScheduler(std::string { Sched });
    fleet.ElectLeader(Sched);
    // Two slots, so once the laptop is offered again it is the scheduler's first choice
    // over the one-slot desk: which machine the last launcher lands on is then a fact
    // about the exclusion list and not about the registry's iteration order.
    fleet.RegisterWorker(Sched, Laptop, Toolchain, 2);
    fleet.SetWorkerUnreachable(Laptop, true);

    // Launcher 1 is granted the laptop, cannot reach it, and remembers that.
    auto const beforeFirst = fleet.Calls().size();
    CHECK(OneLauncher(fleet, store, "k1", Noon).unreachedWorker == Laptop);
    CHECK(fleet.CompiledAt(beforeFirst) == std::vector<std::string> { std::string { Laptop } });

    // Launcher 2 excludes it. The laptop is the only worker, so the lease is refused
    // no-worker -- and counted as ALL-EXCLUDED, never as a toolchain nobody serves.
    auto const beforeSecond = fleet.Calls().size();
    auto const second = OneLauncher(fleet, store, "k2", Noon + std::chrono::seconds { 5 });
    CHECK(second.status == Cc::DispatchStatus::Declined);
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::DispatchLeasesAllExcluded) == 1);
    CHECK(fleet.Metrics().Read(IMetricsSink::Counter::DispatchLeasesNoWorker) == 0);
    CHECK(fleet.CompiledAt(beforeSecond).empty());

    // A second worker joins: launcher 3 is granted the DESK, and no compile goes near the laptop.
    fleet.RegisterWorker(Sched, Desk, Toolchain);
    auto const beforeThird = fleet.Calls().size();
    (void) OneLauncher(fleet, store, "k3", Noon + std::chrono::seconds { 10 });
    CHECK(fleet.CompiledAt(beforeThird) == std::vector<std::string> { std::string { Desk } });

    // A minute after it was noted the entry has expired: the laptop is offered again,
    // it answers now, and the scheduler prefers it.
    fleet.SetWorkerUnreachable(Laptop, false);
    auto const later = Noon + std::chrono::seconds { 70 };
    CHECK(Cc::ReachabilityMemo::Load(store).Fresh(Cc::MemoKind::WorkerUnreached, later).empty());
    auto const beforeFourth = fleet.Calls().size();
    CHECK(OneLauncher(fleet, store, "k4", later).unreachedWorker.empty());
    CHECK(fleet.CompiledAt(beforeFourth) == std::vector<std::string> { std::string { Laptop } });
}

TEST_CASE("A worker address the harness adds answers with its own reply until it is taken down", "[node][fleet][exclusion]")
{
    // The harness half the dial-hint cases stand on: an address a client may DIAL, with a
    // reply of its own, distinguishable from the shared worker reply and from nothing at all.
    constexpr std::string_view Hint = "10.0.0.7:6677";
    constexpr std::string_view Plain = "10.0.0.8:6677";
    Testing::FleetHarness fleet;
    fleet.AddWorkerAddress(std::string { Hint },
                           CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::WorkerSpawnFailed, "hint"));
    fleet.AddWorkerAddress(std::string { Plain });

    auto const own = fleet.Exchange(Hint, AnyCompile(), {}, {});
    REQUIRE(own.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(own.code == CompileCacheWire::ErrorCode::WorkerSpawnFailed);

    // No reply of its own: the shared one, which refuses `NoCapacity` by default.
    auto const shared = fleet.Exchange(Plain, AnyCompile(), {}, {});
    REQUIRE(shared.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(shared.code == CompileCacheWire::ErrorCode::NoCapacity);

    // Taken down, the address reaches nothing -- which is `Unreached`, never a refusal.
    fleet.SetWorkerUnreachable(Hint, true);
    auto const down = fleet.Exchange(Hint, AnyCompile(), {}, {});
    CHECK(down.kind == Cc::CacheOutcomeKind::Transport);
    CHECK(down.transportFailure == Cc::TransportFailure::Unreached);

    // And back up, it answers again.
    fleet.SetWorkerUnreachable(Hint, false);
    CHECK(fleet.Exchange(Hint, AnyCompile(), {}, {}).code == CompileCacheWire::ErrorCode::WorkerSpawnFailed);
}
