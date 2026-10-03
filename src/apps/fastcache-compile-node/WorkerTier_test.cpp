// SPDX-License-Identifier: Apache-2.0
#include "HostEventInbox.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeIoLoop.hpp"
#include "PeerIdentity.hpp"
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
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <Dispatch.hpp>
#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SteppedDrainWait.hpp>
#include <tests/Unwrap.hpp>

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
    CHECK(refused.error().reason.contains("no --scheduler"));
    CHECK(refused.error().cause == NodeRefusalCause::EarlierRule);
}

TEST_CASE("A worker that gave up while serving ends as a failure, so a compiler installed since is found",
          "[node][worker-tier][exit]")
{
    // A survey that found nothing finds the compiler installed since, so the supervisor restarts it.
    // (A fleet other than the one `--cluster-id` named was the refusal here; that flag is gone, and
    // the formation record is the one author of which fleet this node is in.)
    CHECK_FALSE(WorkerEnding(false).has_value());
    CHECK(WorkerEnding(true) == NodeRefusalCause::ToolchainSurvey);
    CHECK(ExitOf(NodeRefusalCause::ToolchainSurvey) == ProcessExit::Failed);
}

TEST_CASE("A worker answers the refused-arguments row the moment it exists", "[node][worker-tier][conditions]")
{
    // The tier builds the report into its own protocol, against the registry `main` shares: a
    // worker that has refused nothing has CHECKED, so the row reads clear -- never undecided,
    // which `Settle` would name as a wiring defect -- and a node with no worker answers nothing,
    // leaving the row to `Settle`'s not-evaluated.
    namespace Wire = CompileCacheWire;
    WorkerTierFixture fixture;

    SECTION("a worker")
    {
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);
    }

    SECTION("no worker")
    {
        fixture.cfg.slots = 0;
        auto const none = fixture.Start();
        REQUIRE(none.has_value());
        REQUIRE(none.value() == nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Undecided);
    }
}

namespace
{

/// A COMPILE frame for @p fingerprint carrying @p args, framed the way the launcher frames them.
/// @param fingerprint The toolchain to claim.
/// @param args The job's arguments.
/// @return The request frame.
[[nodiscard]] std::vector<std::byte> CompileFrameWith(std::string_view fingerprint, std::vector<std::string> const& args)
{
    namespace Wire = CompileCacheWire;
    std::vector<std::byte> argsField;
    for (auto const& arg: args)
    {
        auto const length = static_cast<std::uint32_t>(arg.size());
        for (auto const shift: { 24U, 16U, 8U, 0U })
            argsField.push_back(static_cast<std::byte>((length >> shift) & 0xFFU));
        for (auto const c: arg)
            argsField.push_back(static_cast<std::byte>(c));
    }
    // The framing is the worker's own decoder's to judge, so a list it would read differently
    // fails here rather than as a refusal for some other reason.
    REQUIRE(Cc::DecodeArgs(argsField) == args);

    constexpr std::string_view Source = "int main(){return 0;}";
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(Source.size()), Wire::AsBytes(Source));
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                      .fingerprint = fingerprint,
                                                      .args = argsField,
                                                      .source = enveloped,
                                                      .acceptedCodecs = { Wire::IdentityCodec },
                                                      .sourceName = "a.cpp",
                                                      .compileDir = {},
                                                      .compileDirReplacement = {},
                                                      .sourceRoot = {},
                                                      .sourceRootReplacement = {} });
}

} // namespace

TEST_CASE("A worker tier reports a refused argument through its OWN protocol", "[node][worker-tier][conditions]")
{
    // The wiring, at the door a client knocks on: the tier's responder, over the protocol the tier
    // built, into the registry the tier was handed. A report the tier constructs and never hands
    // its protocol reads `clear` forever and passes every case that builds a protocol by hand.
    namespace Wire = CompileCacheWire;
    WorkerTierFixture fixture;
    // A PINNED identity, so the survey serves it without spawning anything: the tier cannot judge
    // a job's arguments until it knows which driver the job names.
    fixture.cfg.toolchains = { "deadbeef=/opt/none/g++" };
    // The heartbeat's rounds dial the scheduler once the survey lands. Spare replies, because a dial
    // past the script would FAIL on the heartbeat's thread, where no assertion may run.
    auto const ok = Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {});
    fixture.heartbeatReplies = { ok, ok, ok, ok, ok, ok };

    auto const started = fixture.Start();
    REQUIRE(started.has_value());
    auto const& tier = started.value();
    REQUIRE(tier != nullptr);
    core::platform::ManualClock statusClock;
    SchedulerReachability reachability { statusClock, nullptr };
    auto heartbeat = tier->Launch(statusClock, reachability);
    REQUIRE(Testing::WaitUntil(
        "the heartbeat's survey to serve the pinned toolchain",
        [&tier] { return !tier->CompilerFor("deadbeef").empty(); },
        [&tier] { return std::format("compiler for deadbeef: '{}'", tier->CompilerFor("deadbeef")); }));

    // Driven ON the node's reactor, the shape `FrameEndpoint::ServeConnection` has: the compile hops
    // to the pool and its reply is posted back to this loop, which `blockOn` turns on this thread
    // until the answer is in -- so nothing here needs a reactor thread the fixture never starts.
    // The reply's slot is released before the answer is handed over, as the endpoint releases it
    // once the reply is written.
    auto const bytes = fixture.io.Reactor().blockOn(
        [](CompileResponder* responder, std::vector<std::byte> request) -> core::async::Task<std::vector<std::byte>> {
            auto answer = co_await responder->Answer(request, PeerIdentity { .host = "127.0.0.1" });
            answer.hold.reset();
            co_return std::move(answer.bytes);
        }(&tier->Responder(), CompileFrameWith("deadbeef", { "-O2", "-fanalyzer" })));
    auto const decoded = Wire::DecodeErrorPayload(std::span<std::byte const> { bytes }.subspan(Wire::ReplyHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Testing::Unwrap(decoded).first == Wire::ErrorCode::WorkerRejectedArgument);

    CHECK(fixture.conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
    auto const rows = fixture.conditions.Snapshot();
    auto const row =
        std::ranges::find(rows, RowFor(NodeCondition::RefusedCompileArguments).id, &Wire::NodeConditionFields::id);
    REQUIRE(row != rows.end());
    CHECK(row->detail.contains("-fanalyzer"));

    // As `main` does before anything is destroyed: every compile drained while the reactor turns.
    tier->StopAndDrain();
}

TEST_CASE("A reloaded allowlist re-judges the refused-arguments row", "[node][worker-tier][conditions]")
{
    // `AdoptAllowlist` is what the heartbeat runs on every reload: the runner and the report must
    // move together, or the operator's fix lands on the runner and the row keeps saying the
    // argument is refused -- or the row clears over one that still is.
    namespace Wire = CompileCacheWire;
    NodeConditions conditions;
    NullLogger logger;
    auto runner = Cc::MakeProcessRunner();
    FastCache::Testing::ScratchDirectory scratch { "fc-adopt-allowlist" };
    Cc::CompileJobRunner jobs { *runner, scratch.Path(), { { "gcc", "/opt/none/g++" } }, Cc::ToolchainSurvey::Completed() };
    std::vector<std::string> inForce;
    RefusedArgumentsReport report { conditions, logger, inForce };
    report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fanalyzer", Cc::Flavor::Gcc));
    REQUIRE(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);

    AdoptAllowlist(jobs, report, logger, inForce, { "-fanalyzer" });

    CHECK(inForce == std::vector<std::string> { "-fanalyzer" });
    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);

    // An unchanged candidate is no reload at all, and changes nothing the row says.
    report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fno-such", Cc::Flavor::Gcc));
    AdoptAllowlist(jobs, report, logger, inForce, { "-fanalyzer" });
    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
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

TEST_CASE("A toolchain label a scheduler would refuse is withheld from the registration, and said so",
          "[node][worker-tier][label]")
{
    // The label is display only, and a scheduler refuses the WHOLE registration over one that is not
    // text or is longer than it records -- so the worker registers without it. Silently, the fleet page
    // would show no name for a compiler this node serves and nothing would say why; so one Warn per
    // toolchain, naming the compiler and the reason. The at-bound row is the control: sent, and quiet.
    struct Case
    {
        std::string_view why;    ///< What the case is about.
        std::string label;       ///< What the probe called the compiler.
        std::string sent;        ///< What the registration carries.
        std::string_view reason; ///< What the Warn says, or empty for none.
    };
    auto const atBound = std::string(CompileCacheWire::MaxToolchainLabelBytes, 'x');
    auto const cases = std::array {
        Case { .why = "not text", .label = "g++ \xff 14.2.0", .sent = "", .reason = "it is not valid UTF-8" },
        Case { .why = "over the bound", .label = atBound + "x", .sent = "", .reason = "a scheduler records at most" },
        Case { .why = "at the bound", .label = atBound, .sent = atBound, .reason = "" },
    };

    for (auto const& [why, label, sent, reason]: cases)
    {
        INFO(why);
        CapturingLogger logger;
        auto const registered = RegisteredToolchainLabel(
            ServedToolchain { .compiler = "/opt/cross/bin/g++", .label = label, .witness = std::nullopt }, logger);
        CHECK(registered == sent);

        // CHECK rather than REQUIRE on the count, so a case that goes wrong does not hide the ones
        // after it.
        auto const records = logger.Snapshot();
        CHECK(records.size() == (reason.empty() ? 0U : 1U));
        for (auto const& record: records)
        {
            CHECK(record.level == LogLevel::Warn);
            CHECK(record.message.contains("/opt/cross/bin/g++"));
            CHECK(record.message.contains(reason));
        }
    }
}

TEST_CASE("RegistrarsFor asks RegisteredToolchainLabel rather than sending a label unchecked", "[node][worker-tier][label]")
{
    // The case above proves what `RegisteredToolchainLabel` decides; nothing proves `RegistrarsFor`
    // actually asks it -- it is the only caller, and the survey that would exercise it for real runs
    // on the heartbeat thread this suite does not spin up. `RegistrarsFor` is a pure query, so this
    // calls it directly rather than through a seam of its own.
    WorkerTierFixture fixture;
    auto const tier = fixture.Start();
    REQUIRE(tier.has_value());
    REQUIRE(tier.value() != nullptr);
    fixture.logger.Clear(); // Startup may itself have warned; only what RegistrarsFor logs matters here.

    auto const served = std::map<std::string, ServedToolchain> {
        { "fp-cross",
          ServedToolchain { .compiler = "/opt/cross/bin/g++", .label = "g++ \xff 14.2.0", .witness = std::nullopt } },
    };
    auto const registrars = tier.value()->RegistrarsFor(served);
    CHECK(registrars.size() == 1);

    auto const records = fixture.logger.Snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records.front().level == LogLevel::Warn);
    CHECK(records.front().message.contains("/opt/cross/bin/g++"));
    CHECK(records.front().message.contains("it is not valid UTF-8"));
}
