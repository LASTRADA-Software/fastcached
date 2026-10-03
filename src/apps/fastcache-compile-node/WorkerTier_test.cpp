// SPDX-License-Identifier: Apache-2.0
#include "HostEventInbox.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeIoLoop.hpp"
#include "SchedulerReachability.hpp"
#include "ScratchClaim.hpp"
#include "WorkerTier.hpp"
#include "WorkerTierTestFixture.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SteppedDrainWait.hpp>

using namespace FastCache;
using namespace FastCache::Node;

using WorkerTierFixture = WorkerTierTesting::WorkerTierFixture;
using WorkerTierTesting::Worker;

TEST_CASE("A node running no worker builds no worker tier, and a worker builds one", "[node][worker-tier]")
{
    // #206, #1387. The tier is the one place the question *does this node run a worker*
    // is answered, and on a node running none there is nothing: no pool thread, no
    // capacity sized to zero, no validator, no survey and no heartbeat to register with.
    // A `--scheduler` is named on it deliberately -- the cluster and enrollment commands
    // read it -- and it still builds nothing that could register.
    WorkerTierFixture fixture;

    fixture.cfg.slots = 0;
    fixture.cfg.toolchains.clear();
    // A machine that cannot even be ASKED for its scratch base, which a node running no
    // worker must start on: it never asks.
    fixture.machineThrows = true;
    auto const none = fixture.Start();
    REQUIRE(none.has_value());
    CHECK(none.value() == nullptr);
    CHECK(fixture.machineBuilds == 0);
    CHECK(fixture.discoveryCalls == 0);
    // No scratch root was claimed: the base holds nothing.
    CHECK(std::filesystem::is_empty(fixture.scratch.Path()));

    // The control: the same node running a worker is one, sized from the machine.
    fixture.cfg = Worker();
    fixture.machineThrows = false;
    auto const worker = fixture.Start();
    REQUIRE(worker.has_value());
    REQUIRE(worker.value() != nullptr);
    CHECK(fixture.machineBuilds == 1);
    auto const& tier = *worker.value();
    CHECK(tier.Slots() == Distributed::OfferableSlots(fixture.capacity, std::nullopt));
    CHECK(tier.StartupToolchainCount() == 1);
    // Seeded with what the process was started advertising, which is what the first
    // registration and every lease check before any reload read (#1279). Asserted here
    // because `Start` is where the seam is built and handed to the validator: a tier
    // that came up answering something else would have the fleet dialling one address
    // while this worker verified grants against another.
    CHECK(tier.Advertised().Current() == "127.0.0.1:6674");
    CHECK_FALSE(std::filesystem::is_empty(fixture.scratch.Path()));
}

TEST_CASE("A worker with no scheduler to register with is refused rather than started", "[node][worker-tier]")
{
    // The startup table refuses this configuration, and the tier refuses it again by
    // name: a worker that started without a link would never register, which is the
    // invisible-node failure. One fact, so the link and the worker cannot disagree.
    WorkerTierFixture fixture;
    fixture.cfg.schedulers.clear();

    auto const refused = fixture.Start();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("no --scheduler"));
}

TEST_CASE("A worker answers whether its scratch root can be written into a mapping rule", "[node][worker-tier][conditions]")
{
    // #1364. The worker's half of a debug-prefix-map rule it cannot spell was a startup Warn and
    // nothing else; it is now a LATCHED condition row too, answered where the root is claimed. WHAT
    // DISTINGUISHES: three roots give three different answers -- benign, raised, and not evaluated
    // because there is no root -- and a tier answering any one of them for all three fails two.
    namespace Wire = CompileCacheWire;
    WorkerTierFixture fixture;

    SECTION("an ordinary root is checked and benign")
    {
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::ScratchRootUnmappable) == Wire::ConditionState::Clear);
    }

    SECTION("a root with a space in it is raised, naming the rule and the root")
    {
        fixture.scratchBase = fixture.scratch.Path() / "with space";
        std::filesystem::create_directories(fixture.scratchBase);
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::ScratchRootUnmappable) == Wire::ConditionState::Raised);
        auto const rows = fixture.conditions.Snapshot();
        auto const row =
            std::ranges::find(rows, RowFor(NodeCondition::ScratchRootUnmappable).id, &Wire::NodeConditionFields::id);
        REQUIRE(row != rows.end());
        CHECK(row->detail.contains("-fdebug-prefix-map"));
        CHECK(row->detail.contains("with space"));
        CHECK(row->persistence == "latched");
    }

    SECTION("a worker with nothing to compile claims no root, and says it could not check one")
    {
        fixture.cfg.toolchains.clear();
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::ScratchRootUnmappable) == Wire::ConditionState::NotEvaluated);
    }
}

TEST_CASE("A worker hears the host's events from the moment it exists, and stops hearing them when it goes",
          "[node][worker-tier][host-events]")
{
    WorkerTierFixture fixture;
    {
        auto const started = fixture.Start();
        REQUIRE(started.has_value());
        REQUIRE(started.value() != nullptr);
        auto& tier = *started.value();
        CHECK(fixture.hostEvents.SubscriberCount() == 1);

        // A resume ends the heartbeat's wait at once.
        fixture.hostEvents.Fire(HostEvent::Resumed);
        std::stop_source stop;
        CHECK(tier.Capacity().WaitForHeartbeat(stop.get_token(), false, std::chrono::seconds { 30 })
              == HeartbeatWake::HostEvent);

        // A suspend never holds the machine past its budget: no heartbeat is running to settle this
        // one, and the machine is handed back when the budget is spent -- by the tier's own inbox,
        // through its parts.
        fixture.hostEvents.Fire(HostEvent::Suspending);
        CHECK(fixture.suspendWait.Elapsed() >= SuspendWithdrawBudget);
        CHECK(fixture.suspendWait.Elapsed() < SuspendWithdrawBudget + std::chrono::milliseconds { 20 });
        // Windows' allowance for a suspend notification, independent of the budget constant.
        CHECK(fixture.suspendWait.Elapsed() < std::chrono::seconds { 2 });
    }
    CHECK(fixture.hostEvents.SubscriberCount() == 0);
}

TEST_CASE("A network change or a resume is handed to the heartbeat: the delivering thread dials nobody and waits for "
          "nothing",
          "[node][worker-tier][host-events]")
{
    // The sink contract (`IHostEventSink`): the thread that delivers is the SCM's handler or the
    // network watcher's, and it must return promptly. So the round a network change asks for runs on
    // the heartbeat's thread -- which no case here launches -- and never on the one that delivered.
    WorkerTierFixture fixture;
    auto const started = fixture.Start();
    REQUIRE(started.has_value());
    REQUIRE(started.value() != nullptr);
    auto& tier = *started.value();

    fixture.hostEvents.Fire(HostEvent::NetworkChanged);
    fixture.hostEvents.Fire(HostEvent::Resumed);

    // The fixture's dialer has an empty script, so a dial would already have FAILED; this names it.
    CHECK(fixture.Dialer().Dialed().empty());
    CHECK(fixture.suspendWait.Elapsed() == std::chrono::milliseconds { 0 });
    // Handed off rather than dropped: the heartbeat's next wait ends at once.
    std::stop_source stop;
    CHECK(tier.Capacity().WaitForHeartbeat(stop.get_token(), false, std::chrono::seconds { 30 })
          == HeartbeatWake::HostEvent);
}

namespace
{

/// The heartbeat's side of the host events, over the real capacity and inbox: what `AwaitNextRound`
/// is handed in production, with a withdrawal that only counts.
struct HeartbeatWaitRig
{
    NullLogger logger;
    CompileCapacity capacity { 1, WorkerMaxRequestBytes, std::chrono::seconds { 5 }, logger };
    std::size_t withdrawals = 0;
    std::optional<HeartbeatWake> woke;
    std::stop_source stop;
    /// Plays the heartbeat thread INSIDE a suspend's wait, as production's does from its own thread:
    /// the first poll runs one `AwaitNextRound`, with @p interval.
    std::chrono::milliseconds interval { 20 };
    /// Whether posting an event wakes the heartbeat, as production's does. Off, the wake a case
    /// wants to arrive LATE is played by `lateWake`.
    bool postWakes = true;
    /// Whether the withdrawal wakes the heartbeat once more: the posting thread's `_wake()` landing
    /// after the heartbeat already TOOK the suspend it announces.
    bool lateWake = false;
    Testing::SteppedDrainWait wait { [this] {
        if (!woke.has_value())
            woke = Await(interval);
    } };
    HostEventInbox inbox { [this] {
                              if (postWakes)
                                  capacity.WakeHeartbeat();
                          },
                           wait };

    /// @param within The interval this wait may run out.
    /// @return What ended it.
    [[nodiscard]] HeartbeatWake Await(std::chrono::milliseconds within)
    {
        return AwaitNextRound(stop.get_token(), capacity, inbox, false, within, [this] {
            ++withdrawals;
            if (lateWake)
                capacity.WakeHeartbeat();
        });
    }
};

/// Long enough that a wait which ran it out did not end for the reason under test.
constexpr auto LongInterval = std::chrono::seconds { 30 };

} // namespace

TEST_CASE("A suspend is withdrawn and then waited past: no round runs for it", "[node][worker-tier][host-events]")
{
    // Announcing right after withdrawing would re-register a machine that is about to sleep.
    HeartbeatWaitRig rig;

    rig.inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(rig.withdrawals == 1);
    // The short interval ran out: the wait went on after the withdrawal rather than ending for it.
    CHECK(rig.woke == std::optional { HeartbeatWake::Elapsed });
    // And the suspend was let go as soon as the withdrawal was made, not at its budget.
    CHECK(rig.inbox.LastSuspendWait() == std::optional { DrainResult::Drained });
    CHECK(rig.wait.Sleeps() == 1);
}

TEST_CASE("A resume posted before a suspend is superseded: no round runs after the withdrawal",
          "[node][worker-tier][host-events]")
{
    // Deliveries are serialised, so a resume still pending when a suspend is posted is older than
    // it; a round for it would re-register a machine about to sleep.
    HeartbeatWaitRig rig;

    rig.inbox.OnHostEvent(HostEvent::Resumed);
    rig.inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(rig.withdrawals == 1);
    // The short interval ran out: nothing was owed after the withdrawal.
    CHECK(rig.woke == std::optional { HeartbeatWake::Elapsed });
    CHECK_FALSE(rig.inbox.HasPending());
}

TEST_CASE("A resume posted after a suspend still runs the round", "[node][worker-tier][host-events]")
{
    HeartbeatWaitRig rig;
    rig.inbox.OnHostEvent(HostEvent::Suspending);
    REQUIRE(rig.withdrawals == 1);

    rig.inbox.OnHostEvent(HostEvent::Resumed);

    CHECK(rig.Await(LongInterval) == HeartbeatWake::HostEvent);
    CHECK(rig.withdrawals == 1);
    CHECK_FALSE(rig.inbox.HasPending());
}

TEST_CASE("A wake consumed after its suspend was taken does not run a round", "[node][worker-tier][host-events]")
{
    // A suspend can be posted as the wait's interval runs out: the heartbeat TAKES it on that
    // wake, and the posting thread's `_wake()` lands afterwards. That wake announces a suspend
    // already withdrawn, so nothing is owed -- a round for it would register the machine about to
    // sleep, the one thing the withdrawal was for.
    HeartbeatWaitRig rig;
    rig.postWakes = false;
    rig.lateWake = true;

    rig.inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(rig.withdrawals == 1);
    CHECK(rig.inbox.LastSuspendWait() == std::optional { DrainResult::Drained });
    CHECK_FALSE(rig.inbox.HasPending());
    // The interval ran out after the withdrawal, rather than the stale wake ending the wait.
    CHECK(rig.woke == std::optional { HeartbeatWake::Elapsed });
}

TEST_CASE("A resume or a network change alone runs the round now and withdraws nothing", "[node][worker-tier][host-events]")
{
    HeartbeatWaitRig rig;

    rig.inbox.OnHostEvent(HostEvent::NetworkChanged);

    CHECK(rig.Await(LongInterval) == HeartbeatWake::HostEvent);
    CHECK(rig.withdrawals == 0);
    CHECK_FALSE(rig.inbox.HasPending());
}

TEST_CASE("A stop ends the heartbeat's wait and withdraws nothing", "[node][worker-tier][host-events]")
{
    HeartbeatWaitRig rig;
    rig.inbox.OnHostEvent(HostEvent::Resumed);
    rig.stop.request_stop();

    CHECK(rig.Await(LongInterval) == HeartbeatWake::Stopped);
    CHECK(rig.withdrawals == 0);
}

TEST_CASE("A running heartbeat withdraws for a suspend and lets it go: the tier's own loop is wired",
          "[node][worker-tier][host-events]")
{
    // Every case above drives `AwaitNextRound` beside the tier; this one runs the tier's OWN heartbeat
    // thread, so a loop that went back to a bare wait -- or a wait that never withdrew -- leaves every
    // suspend to run out its budget with nothing withdrawn, and fails here.
    WorkerTierFixture fixture;
    // Nothing can be spawned, so the survey serves nothing and the heartbeat keeps running; its
    // rounds then dial the scheduler with no toolchain to announce. Spare replies, because a dial
    // past the script would FAIL on the heartbeat's thread, where no assertion may run.
    fixture.compilersCannotRun = true;
    auto const ok = CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok, std::vector<std::byte> {});
    fixture.heartbeatReplies = { ok, ok, ok };
    // If the suspend polls at all, its first poll waits -- on the case's thread, bounded by the
    // test's own hang guard and not by the suspend's budget -- until the heartbeat thread has
    // withdrawn. The budget runs on the stepped clock, so no real deadline races that thread. It may
    // not poll: a heartbeat that withdraws and settles before the suspend's first look ends it with
    // no poll, and then the withdrawal has happened already.
    auto const withdrewLine = [&fixture] {
        auto const records = fixture.logger.Snapshot();
        return std::ranges::count_if(records, [](CapturingLogger::Record const& record) {
            return record.message.contains("this machine is going to sleep: withdrew 0 registration(s)");
        });
    };
    std::optional<Testing::WaitOutcome> withdrawn;
    fixture.suspendPoll = [&withdrawn, &withdrewLine] {
        if (!withdrawn.has_value())
            withdrawn = Testing::WaitUntilOutcome(
                "the heartbeat thread to withdraw for the suspend",
                [&withdrewLine] { return withdrewLine() > 0; },
                [&withdrewLine] { return std::format("{} withdrawal line(s)", withdrewLine()); });
    };
    auto const started = fixture.Start();
    REQUIRE(started.has_value());
    REQUIRE(started.value() != nullptr);
    auto& tier = *started.value();
    SchedulerReachability reachability { fixture.clock, nullptr };

    {
        auto const heartbeat = tier.Launch(fixture.clock, reachability);
        // The first round has run once `node-status` has a registration reading; the heartbeat is
        // then in its wait, or about to be -- either way, where a suspend reaches it.
        REQUIRE(Testing::WaitUntil(
            "the heartbeat's first round",
            [&tier] { return tier.Runtime().Registration().has_value(); },
            [&tier] { return std::format("toolchain state {}", static_cast<int>(tier.Runtime().Toolchains().state)); }));

        fixture.hostEvents.Fire(HostEvent::Suspending);

        // The verdict is the withdrawal line, which a wired loop has written by the time the suspend
        // returns in EVERY order: settled at the first look (written before the settle), or polled
        // (the hook waited for it). A loop that never withdraws never settles, so the suspend polls,
        // the hook's wait runs out, and it says so by name.
        CHECK((!withdrawn.has_value() || Testing::Reached(*withdrawn)));
        CHECK(withdrewLine() == 1);
    }
    // One dial, the first round's: a withdrawal with nothing registered dials nobody.
    CHECK(fixture.Dialer().Dialed().size() == 1);
}
