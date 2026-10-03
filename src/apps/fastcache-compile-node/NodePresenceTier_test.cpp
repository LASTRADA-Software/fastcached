// SPDX-License-Identifier: Apache-2.0
//
// What a machine with no worker owes the fleet
// ([#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440)).
//
// The half worth testing is `AnnounceMachineOnce`, and the reason it is a free function over a
// record is this file: the loop around it acquires a thread, a real host sampler and a real
// dialler, none of which a case can drive, while what decides anything is *what gets said* and
// *whether the history cursor moves*.
//
// **The cursor is the property with a direction that gets skipped.** Advancing it on a round
// that landed is the case everybody writes; leaving it alone on a round that was REFUSED is the
// one that matters, because a fleet re-electing is exactly when every endpoint refuses, and it
// is also exactly the history the handover exists to carry across an election. So the two cases
// below differ in ONE thing -- the scheduler's reply -- and a round that always advanced, or
// never did, fails exactly one of them. Neither alone tests anything.
#include "EndpointDialerTestUtils.hpp"
#include "HostEventInbox.hpp"
#include "NodeAnnounce.hpp"
#include "NodeFormation.hpp"
#include "NodePresenceTier.hpp"
#include "NodeRoster.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/net/ISocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/FleetHistoryFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScriptedHostEvents.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{

namespace Wire = FastCache::CompileCacheWire;

constexpr std::string_view Scheduler = "scheduler.example:6676";
constexpr std::string_view ThisMachine = "10.0.0.4:6674";

/// A host that reports nothing, so a round reads no CPU, memory or disk at all.
///
/// Which is also what a machine running no worker genuinely reports for its scratch space --
/// there is no scratch directory to measure -- so the absent figures here are the production
/// shape rather than a convenience.
class SilentLoadSampler final: public IHostLoadSampler
{
  public:
    [[nodiscard]] HostLoad Sample() override
    {
        return HostLoad {};
    }
};

/// @return A snapshot provider answering for a machine with no storage and no upstream.
[[nodiscard]] AdminHttpServer::SnapshotProvider NodeFacts()
{
    return [] {
        return MetricsSnapshot { .storage = std::nullopt,
                                 .storageTiers = {},
                                 .host = HostCapacity { .configuredSlots = 0, .busySlots = 0 },
                                 .upstreamConfigured = std::nullopt,
                                 .uptime = {} };
    };
}

/// The frames a dial carried, as `(op, payload)` pairs.
/// @param sent Everything written on that connection.
/// @return One entry per whole frame, in order.
[[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> FramesIn(std::span<std::byte const> sent)
{
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> frames;
    while (sent.size() >= Wire::RequestHeaderSize)
    {
        auto const header = Wire::DecodeRequestHeader(sent);
        if (!header.has_value())
            break;
        auto const whole = Wire::RequestHeaderSize + std::size_t { header->payloadLength };
        if (sent.size() < whole)
            break;
        auto const payload = sent.subspan(Wire::RequestHeaderSize, header->payloadLength);
        frames.emplace_back(header->opRaw, std::vector<std::byte> { payload.begin(), payload.end() });
        sent = sent.subspan(whole);
    }
    return frames;
}

/// A formation record's memos, handed on as the formation controller hands them.
class ListedAskedJoins final: public Cluster::IAskedJoinsSource
{
  public:
    /// @param joins What the record keeps.
    explicit ListedAskedJoins(std::vector<Cluster::AskedJoin> joins):
        _joins { std::move(joins) }
    {
    }

    [[nodiscard]] std::vector<Cluster::AskedJoin> AskedJoins() const override
    {
        return _joins;
    }

  private:
    std::vector<Cluster::AskedJoin> _joins;
};

/// Everything one presence round needs, with a sampler holding one closed bucket.
///
/// The bucket is produced the way the node produces one -- a sample, a minute of wall clock,
/// another sample -- rather than by building a `FleetBucket` by hand: a hand-built one would
/// pass even if nothing in the process could ever close a window.
struct PresenceFixture
{
    NodeConfig cfg;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    SilentLoadSampler loadSampler;
    Testing::PlacedWallClock wall;
    Wire::CapacityFields capacity {};
    FleetSampler sampler { std::nullopt, metrics, NodeFacts(), wall, HistoryPaths {}, logger };
    NodeConditions conditions;
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock, &conditions };
    Cluster::IAskedJoinsSource const* askedJoins { nullptr }; ///< What the machine once asked; none by default.
    /// Where each loop built over this fixture registers, kept for as long as the fixture.
    std::vector<std::unique_ptr<AppliedSchedulers>> schedulers;

    PresenceFixture()
    {
        cfg = Testing::LearnerRegisteringWith(std::move(cfg), { std::string { Scheduler } });

        // Only a CLOSED bucket may travel: an open one is a partial window a leader could
        // never be told to correct.
        REQUIRE_FALSE(sampler.SampleOnce());
        wall.Advance(std::chrono::minutes { 1 });
        REQUIRE_FALSE(sampler.SampleOnce());
        REQUIRE(sampler.NextHistoryBatch(8).size() == 1);
    }

    /// The round production builds, over this fixture.
    /// @return The round.
    [[nodiscard]] PresenceRound Round()
    {
        return PresenceRound { .loadSampler = loadSampler,
                               .cacheTier = nullptr,
                               .metrics = metrics,
                               .sampler = sampler,
                               .capacity = capacity,
                               .endpoint = ThisMachine,
                               .logger = logger,
                               .conditions = conditions,
                               // Nothing proves: the scripted fleet serves no handshake (#178).
                               .prover = nullptr,
                               .reachability = reachability,
                               .askedJoins = askedJoins };
    }

    /// Announce once through @p dialer.
    ///
    /// The dialler is the CASE's, passed in rather than made here, so a case can read the walk
    /// afterwards -- which endpoints were reached, in what order -- against an object whose
    /// existence is in its type. A fixture that owned one in a `std::optional` made every such
    /// read an unchecked optional access, and Catch2's `REQUIRE` is a macro no analyser can see
    /// through, so the guard that looked sufficient established nothing.
    ///
    /// Its script carries one reply PER DIAL, because a `NotLeader` is an instruction and the
    /// round FOLLOWS it -- so the realistic refusal costs two dials, and a script of one reply
    /// does not measure a refusal at all, it runs out. That is how the first version of the
    /// refusal case below failed, and the scripted dialler said so in as many words rather
    /// than letting it read as a dial failure.
    /// @param dialer How the round reaches a scheduler, and what records the walk.
    /// @return Whether the round reported acceptance.
    [[nodiscard]] bool AnnounceThrough(Testing::ScriptedDialer& dialer)
    {
        auto link = Unwrap(SchedulerLink::For(SchedulersOf(cfg, AsConfigured)));
        return AnnounceMachineOnce(Round(), link, dialer);
    }
};

} // namespace

TEST_CASE("A zero-config node announces, so scheduler-unreachable and own-record-awaited are evaluated",
          "[node][presence][conditions][formation]")
{
    // H-b. A serving node is refused `--scheduler` and registers where its formation record says, so
    // a row scoped on the typed flag reads "names no scheduler" on EVERY node -- a confident wrong
    // signal. Asked of what a body actually registers with, a first start announces to the
    // scheduler it serves, a learner to its fleet, and a node whose consensus is closed to nobody.
    auto const zeroConfig = Testing::FirstStart(NodeConfig {});
    REQUIRE(zeroConfig.schedulers.empty());
    REQUIRE(RunsConsensus(zeroConfig));
    CHECK(AnnouncesToAScheduler(zeroConfig, AsConfigured));
    CHECK(AwaitsItsOwnRecord(zeroConfig, AsConfigured));

    auto const learner = Testing::LearnerRegisteringWith(NodeConfig {}, { std::string { Scheduler } });
    REQUIRE(learner.schedulers.empty());
    CHECK(AnnouncesToAScheduler(learner, AsConfigured));

    // The control, both rows: consensus closed, so no scheduler served and none remembered.
    auto closed = NodeConfig {};
    closed.raftListen.clear();
    closed = Testing::FirstStart(std::move(closed));
    REQUIRE_FALSE(RunsConsensus(closed));
    REQUIRE(SchedulersOf(closed, AsConfigured).empty());
    CHECK_FALSE(AnnouncesToAScheduler(closed, AsConfigured));
    CHECK_FALSE(AwaitsItsOwnRecord(closed, AsConfigured));
}

TEST_CASE("own-record-awaited is a consensus node's: a learner announces and awaits, a worker registering nowhere does not",
          "[node][presence][conditions][formation]")
{
    // The second row's own question: it needs consensus AND an announcement. A learner runs both;
    // and the scope never reads `--scheduler`, which a one-shot verb may still name -- typed on a
    // node that serves nothing, it decides nothing here.
    auto const learner = Testing::LearnerRegisteringWith(NodeConfig {}, { std::string { Scheduler } });
    REQUIRE(RunsConsensus(learner));
    CHECK(AwaitsItsOwnRecord(learner, AsConfigured));

    auto aimed = NodeConfig {};
    aimed.raftListen.clear();
    aimed = Testing::FirstStart(std::move(aimed));
    aimed.schedulers = { std::string { Scheduler } };
    REQUIRE(SchedulersOf(aimed, AsConfigured).empty());
    CHECK_FALSE(AnnouncesToAScheduler(aimed, AsConfigured));
    CHECK_FALSE(AwaitsItsOwnRecord(aimed, AsConfigured));
}

TEST_CASE("A presence round hands the leader every fleet this machine once asked", "[node][presence][formation]")
{
    // The evidence a split of the fleet is told on must reach the LEADER whichever machine asked, so it
    // rides the one verb every node sends, read afresh each round from the record.
    auto const office = Testing::TestKeyPair("n-office").PublicKey();
    auto const lab = Testing::TestKeyPair("n-lab").PublicKey();
    ListedAskedJoins const joins {
        { Cluster::AskedJoin { .clusterId = "c-office", .provenKey = office, .askedAtUnixSeconds = 1 },
          Cluster::AskedJoin { .clusterId = "c-lab", .provenKey = lab, .askedAtUnixSeconds = 2 } }
    };
    PresenceFixture fixture;
    fixture.askedJoins = &joins;
    Testing::ScriptedDialer dialer { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    REQUIRE(fixture.AnnounceThrough(dialer));

    auto const frames = FramesIn(dialer.SentOn(0));
    REQUIRE(frames.size() == 1);
    auto const announced = Unwrap(Wire::DecodeNodeAnnouncePayload(frames.front().second));
    CHECK(announced.joinMemos
          == std::vector { Wire::JoinMemoFields { .clusterId = "c-office", .provenKey = office },
                           Wire::JoinMemoFields { .clusterId = "c-lab", .provenKey = lab } });

    // A machine that keeps no record announces none -- an empty list, never an absent field.
    PresenceFixture plain;
    Testing::ScriptedDialer second { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    REQUIRE(plain.AnnounceThrough(second));
    auto const none = Unwrap(Wire::DecodeNodeAnnouncePayload(FramesIn(second.SentOn(0)).front().second));
    CHECK(none.joinMemos.empty());
}

TEST_CASE("A machine announces itself under its own address and hands over its history", "[node][presence][fleethistory]")
{
    PresenceFixture fixture;
    Testing::ScriptedDialer dialer { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    REQUIRE(fixture.AnnounceThrough(dialer));

    auto const frames = FramesIn(dialer.SentOn(0));
    REQUIRE(frames.size() == 1);

    // The VERB, pinned by its byte as well as by its name: a symbol both ends spell can only
    // test that they agree, not that the agreement is the one on the wire.
    CHECK(frames.front().first == 0x1A);
    CHECK(frames.front().first == static_cast<std::uint8_t>(Wire::Op::NodeAnnounce));

    auto const announced = Unwrap(Wire::DecodeNodeAnnouncePayload(frames.front().second));

    // **Filed under the machine, never under the scheduler it reached.** The two are different
    // addresses and the round has both in hand, so asserting the payload rather than the dial
    // is what separates "announced itself" from "announced whoever answered".
    CHECK(Wire::AsStringView(announced.endpoint) == ThisMachine);
    CHECK(Wire::AsStringView(announced.endpoint) != Scheduler);

    // The batch travelled, which is the whole reason this verb carries a load record at all.
    CHECK(announced.load.history.size() == 1);

    // And the cursor moved past it, so the next round offers something else.
    CHECK(fixture.sampler.NextHistoryBatch(8).empty());
}

TEST_CASE("A refused announcement leaves the history to be offered again", "[node][presence][fleethistory]")
{
    PresenceFixture fixture;

    // `NotLeader` first, because it is the refusal a fleet actually produces while it
    // re-elects -- the moment the handover exists for -- and then a refusal at the endpoint it
    // named, since the round FOLLOWS a redirect. Two dials is the production walk, not a
    // contrivance: `NotAMember` is what a leader answers a machine it does not admit.
    Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "10.0.0.7:6676"),
                                       Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "") } };
    CHECK_FALSE(fixture.AnnounceThrough(dialer));

    // The redirect WAS followed, so this case covers the whole walk rather than a round that
    // gave up at the first answer.
    REQUIRE(dialer.Dialed().size() == 2);
    CHECK(dialer.Dialed().at(1) == "10.0.0.7:6676");

    // It was SENT -- on BOTH dials -- which is what makes the next assertion about the cursor
    // rather than about whether anything happened at all: a round that skipped the
    // announcement would also leave the batch in place, and would pass a test that only
    // looked at the sampler.
    auto const first = FramesIn(dialer.SentOn(0));
    REQUIRE(first.size() == 1);
    CHECK(first.front().first == static_cast<std::uint8_t>(Wire::Op::NodeAnnounce));
    REQUIRE(FramesIn(dialer.SentOn(1)).size() == 1);

    // Unmoved. This is the direction that gets skipped, and the one a fleet depends on.
    CHECK(fixture.sampler.NextHistoryBatch(8).size() == 1);
}

TEST_CASE("A machine with no closed window announces itself anyway", "[node][presence]")
{
    // The control for the pair above, and not a formality: a round that only announced when it
    // had history to carry would pass both of them, and would leave a freshly started node
    // absent from the Machines table for its first minute -- which is precisely the minute an
    // operator provisioning machine 7 of 40 is looking at it.
    auto const cfg = Testing::LearnerRegisteringWith(NodeConfig {}, { std::string { Scheduler } });
    AtomicMetricsSink metrics;
    NullLogger logger;
    SilentLoadSampler loadSampler;
    Testing::PlacedWallClock wall;
    Wire::CapacityFields const capacity {};
    FleetSampler sampler { std::nullopt, metrics, NodeFacts(), wall, HistoryPaths {}, logger };
    NodeConditions const conditions;
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };

    REQUIRE(sampler.NextHistoryBatch(8).empty());

    Testing::ScriptedDialer dialer { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    auto link = Unwrap(SchedulerLink::For(SchedulersOf(cfg, AsConfigured)));
    auto const accepted = AnnounceMachineOnce(PresenceRound { .loadSampler = loadSampler,
                                                              .cacheTier = nullptr,
                                                              .metrics = metrics,
                                                              .sampler = sampler,
                                                              .capacity = capacity,
                                                              .endpoint = ThisMachine,
                                                              .logger = logger,
                                                              .conditions = conditions,
                                                              .prover = nullptr,
                                                              .reachability = reachability,
                                                              .askedJoins = nullptr },
                                              link,
                                              dialer);

    CHECK(accepted);
    auto const frames = FramesIn(dialer.SentOn(0));
    REQUIRE(frames.size() == 1);
    CHECK(frames.front().first == static_cast<std::uint8_t>(Wire::Op::NodeAnnounce));

    auto const announced = Unwrap(Wire::DecodeNodeAnnouncePayload(frames.front().second));
    CHECK(Wire::AsStringView(announced.endpoint) == ThisMachine);
    CHECK(announced.load.history.empty());
}

TEST_CASE("A machine announces its condition rows, as they stand when the round runs", "[node][presence][conditions]")
{
    // #1364. NODE-ANNOUNCE is the one verb every machine sends, workerless ones included, so it is
    // what carries the rows to the leader's page. Read when the ROUND runs rather than when the
    // presence tier was built: a live row that cleared between two rounds must arrive cleared.
    PresenceFixture fixture;
    fixture.conditions.Raise(NodeCondition::ScratchRootUnmappable, "a space in the root");
    fixture.conditions.Raise(NodeCondition::EnrollmentWindowOpen, "the window is open");

    Testing::ScriptedDialer first { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    REQUIRE(fixture.AnnounceThrough(first));
    auto const opened = Unwrap(Wire::DecodeNodeAnnouncePayload(FramesIn(first.SentOn(0)).front().second));
    CHECK(Unwrap(opened.load.conditions) == fixture.conditions.Snapshot());

    fixture.conditions.Clear(NodeCondition::EnrollmentWindowOpen);
    Testing::ScriptedDialer second { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    REQUIRE(fixture.AnnounceThrough(second));
    auto const closed = Unwrap(Wire::DecodeNodeAnnouncePayload(FramesIn(second.SentOn(0)).front().second));
    auto const rows = Unwrap(closed.load.conditions);
    CHECK(rows == fixture.conditions.Snapshot());
    auto const stateOf = [&rows](NodeCondition condition) {
        auto const row = std::ranges::find(rows, RowFor(condition).id, &Wire::NodeConditionFields::id);
        REQUIRE(row != rows.end());
        return row->state;
    };
    CHECK(stateOf(NodeCondition::EnrollmentWindowOpen) == "clear");
    CHECK(stateOf(NodeCondition::ScratchRootUnmappable) == "raised");
}

TEST_CASE("A presence refused round after round is one Warn and not one per round", "[node][presence][reachability]")
{
    PresenceFixture fixture;
    auto const refused = Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "");
    Testing::ScriptedDialer dialer { { refused, refused, refused } };

    for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
    {
        CHECK_FALSE(fixture.AnnounceThrough(dialer));
        fixture.clock.advance(NodeAnnounceInterval);
    }

    auto const records = fixture.logger.Snapshot();
    auto const warns =
        std::ranges::count_if(records, [](CapturingLogger::Record const& record) { return record.level == LogLevel::Warn; });
    CHECK(warns == 1);
    CHECK(std::ranges::any_of(records, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Warn && record.message.contains("did not record this machine at");
    }));
}

TEST_CASE("A presence exchange that stalls after connecting is unreachable, not a refused presence",
          "[node][presence][reachability][conditions]")
{
    // A connection this scheduler ACCEPTED and then never finished answering never told this node
    // anything to act on: it is the scheduler being unreachable, not a refusal to record this
    // machine. Counting it as `PresenceRefused` sends an operator hunting for a problem with the
    // machine's own record that was never the issue, and it left `scheduler-unreachable` unraised
    // for exactly the outage it exists to report.
    PresenceFixture fixture;
    // Two bytes -- short of `Wire::ReplyHeaderSize` -- so the dial SUCCEEDS (a real scripted
    // socket, never a failed dial) and the reply read then hits EOF partway through the header:
    // what a peer that accepted the connection and then stalled or was lost produces.
    Testing::ScriptedDialer dialer { { std::vector<std::byte>(2) } };

    CHECK_FALSE(fixture.AnnounceThrough(dialer));

    CHECK(fixture.conditions.StateOf(NodeCondition::SchedulerUnreachable) == Wire::ConditionState::Raised);
    auto const records = fixture.logger.Snapshot();
    CHECK(std::ranges::any_of(records, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Warn && record.message.contains("unreachable");
    }));
    CHECK_FALSE(std::ranges::any_of(
        records, [](CapturingLogger::Record const& record) { return record.message.contains("did not record"); }));
}

TEST_CASE("A presence refusal the scheduler actually answered never raises scheduler-unreachable",
          "[node][presence][reachability][conditions]")
{
    // The control for the case above: a real `PresenceRefused` -- the scheduler answered, plainly
    // -- must leave the condition exactly where the constructor left it, `clear`, because the
    // remedy for `NotAMember` names this node's membership, never the network.
    PresenceFixture fixture;
    Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "") } };

    CHECK_FALSE(fixture.AnnounceThrough(dialer));

    CHECK(fixture.conditions.StateOf(NodeCondition::SchedulerUnreachable) == Wire::ConditionState::Clear);
}

// --- Host events (the presence round's half) --------------------------------------------------

TEST_CASE("A resume or a network change ends the presence wait at once, and a suspend does not",
          "[node][presence][host-events]")
{
    PresenceWake wake;
    std::stop_source stop;

    wake.OnHostEvent(HostEvent::Resumed);
    CHECK(wake.WaitOut(stop.get_token(), std::chrono::seconds { 30 }) == PresenceWakeReason::HostEvent);

    wake.OnHostEvent(HostEvent::NetworkChanged);
    CHECK(wake.WaitOut(stop.get_token(), std::chrono::seconds { 30 }) == PresenceWakeReason::HostEvent);

    // A suspend is the worker's to act on; the machine's presence row simply expires.
    wake.OnHostEvent(HostEvent::Suspending);
    CHECK(wake.WaitOut(stop.get_token(), std::chrono::milliseconds { 20 }) == PresenceWakeReason::Elapsed);

    stop.request_stop();
    CHECK(wake.WaitOut(stop.get_token(), std::chrono::seconds { 30 }) == PresenceWakeReason::Stopped);
}

TEST_CASE("A suspend supersedes a presence wake posted before it, and not one posted after", "[node][presence][host-events]")
{
    // Deliveries are serialised by the hub, so a wake still pending when a suspend is posted is
    // older than the suspend. Run after it, the round would re-announce a machine about to sleep
    // and keep its row alive through the sleep; the worker's inbox drops the same wake for the
    // same reason, from the same table row.
    PresenceWake wake;
    std::stop_source stop;

    wake.OnHostEvent(HostEvent::Resumed);
    wake.OnHostEvent(HostEvent::Suspending);
    CHECK(wake.WaitOut(stop.get_token(), std::chrono::milliseconds { 20 }) == PresenceWakeReason::Elapsed);

    // The control: a wake posted after the suspend is owed as ever.
    wake.OnHostEvent(HostEvent::Suspending);
    wake.OnHostEvent(HostEvent::NetworkChanged);
    CHECK(wake.WaitOut(stop.get_token(), std::chrono::seconds { 30 }) == PresenceWakeReason::HostEvent);
}

namespace
{

/// A dialer whose every dial fails -- production's spelling of an endpoint that is not there -- and
/// which notes WHICH THREAD dialled, so a case can tell the loop's round from one run on the thread
/// that delivered a host event. Safe to read while the loop dials.
class ThreadNotingDialer final: public IEndpointDialer
{
  public:
    /// @copydoc IEndpointDialer::Dial
    [[nodiscard]] std::unique_ptr<core::net::ISocket> Dial(std::string_view /*endpoint*/,
                                                           core::net::DialOptions /*options*/) override
    {
        std::scoped_lock const lock { _mutex };
        _threads.push_back(std::this_thread::get_id());
        return nullptr;
    }

    /// @return The thread of every dial so far, in order.
    [[nodiscard]] std::vector<std::thread::id> Threads() const
    {
        std::scoped_lock const lock { _mutex };
        return _threads;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::thread::id> _threads;
};

/// The parts a presence loop borrows, over @p fix and a dialer and host of the case's.
/// @param fix The fixture.
/// @param capacity What this machine is.
/// @param announced Where it answers.
/// @param dialer How its rounds dial.
/// @param events Where the host's events arrive.
/// @return The parts.
[[nodiscard]] NodePresenceParts PartsOver(PresenceFixture& fix,
                                          Distributed::NodeCapacity const& capacity,
                                          AnnouncedEndpoint const& announced,
                                          IEndpointDialer& dialer,
                                          IHostEvents& events)
{
    fix.schedulers.push_back(std::make_unique<AppliedSchedulers>(fix.cfg, AsConfigured));
    return NodePresenceParts { .cfg = fix.cfg,
                               .schedulers = *fix.schedulers.back(),
                               .capacity = capacity,
                               .announced = announced,
                               .cacheTier = nullptr,
                               .metrics = fix.metrics,
                               .sampler = fix.sampler,
                               .logger = fix.logger,
                               .conditions = fix.conditions,
                               .prover = nullptr,
                               .reachability = fix.reachability,
                               .dialer = dialer,
                               .hostEvents = events };
}

} // namespace

TEST_CASE("A presence loop hears the host's events for as long as it runs", "[node][presence][host-events]")
{
    PresenceFixture fix;
    Testing::ScriptedHostEvents events;
    ThreadNotingDialer dialer;
    Distributed::NodeCapacity const capacity { .logicalCores = 4 };
    AnnouncedEndpoint const announced { ThisMachine };
    {
        // Every dial fails at once, so the loop's first round is over quickly and it then waits,
        // which is all this case needs of it.
        auto const presence = NodePresence::Start(PartsOver(fix, capacity, announced, dialer, events));
        REQUIRE(presence != nullptr);
        CHECK(events.SubscriberCount() == 1);
    }
    CHECK(events.SubscriberCount() == 0);
}

TEST_CASE("A host event's presence round runs on the loop's thread and never on the one that delivered it",
          "[node][presence][host-events]")
{
    // The sink contract (`IHostEventSink`): the delivering thread -- the SCM's handler, the network
    // watcher's -- returns promptly and dials nobody. So the round a network change asks for is the
    // LOOP's, and the case tells the two apart by the thread each dial ran on.
    PresenceFixture fix;
    Testing::ScriptedHostEvents events;
    ThreadNotingDialer dialer;
    Distributed::NodeCapacity const capacity { .logicalCores = 4 };
    AnnouncedEndpoint const announced { ThisMachine };
    auto const dials = [&dialer] {
        return dialer.Threads().size();
    };
    {
        auto const presence = NodePresence::Start(PartsOver(fix, capacity, announced, dialer, events));
        REQUIRE(presence != nullptr);
        REQUIRE(Testing::WaitUntil(
            "the presence loop's first round",
            [&dials] { return dials() >= 1; },
            [&dials] { return std::format("{} dial(s)", dials()); }));

        events.Fire(HostEvent::NetworkChanged);

        // With no roster the loop waits `NodeAnnounceInterval`, longer than this wait's own bound,
        // so a second dial inside it is the network change's round and nothing else.
        static_assert(NodeAnnounceInterval > Testing::WaitHangGuard);
        REQUIRE(Testing::WaitUntil(
            "the round the network change asked for",
            [&dials] { return dials() >= 2; },
            [&dials] { return std::format("{} dial(s)", dials()); }));
    }
    auto const threads = dialer.Threads();
    REQUIRE(threads.size() >= 2);
    CHECK(std::ranges::count(threads, std::this_thread::get_id()) == 0);
    CHECK(std::ranges::count(threads, threads.front()) == static_cast<std::ptrdiff_t>(threads.size()));
}

TEST_CASE("AnnouncedCapacity sets this build's compiled-in version on the converted capacity", "[node][presence]")
{
    // #1440's second half: `Distributed::CapacityToWire` derives the capacity record from
    // `NodeCapacity`, which has no version field to read, so a caller that stopped at its
    // answer -- as `NodePresenceTier` once did -- carries an absent version, indistinguishable
    // on the leader's page from a build too old to know the field at all. `AnnouncedCapacity` is
    // `NodePresenceTier`'s one call for both questions; this asserts what it answers.
    Distributed::NodeCapacity const capacity { .logicalCores = 8 };
    auto const wire = AnnouncedCapacity(capacity);
    CHECK(wire.version == FastCache::VersionString);
    // And the conversion itself still ran -- a version stapled onto a default-constructed
    // record would pass the check above having thrown away everything else `capacity` said.
    CHECK(wire.logicalCores == 8);
}
