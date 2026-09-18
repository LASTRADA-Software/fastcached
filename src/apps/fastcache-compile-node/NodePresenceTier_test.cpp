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
#include "NodePresenceTier.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/FleetHistoryFakes.hpp>
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

/// Everything one presence round needs, with a sampler holding one closed bucket.
///
/// The bucket is produced the way the node produces one -- a sample, a minute of wall clock,
/// another sample -- rather than by building a `FleetBucket` by hand: a hand-built one would
/// pass even if nothing in the process could ever close a window.
struct PresenceFixture
{
    NodeConfig cfg;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SilentLoadSampler loadSampler;
    Testing::PlacedWallClock wall;
    Cc::CredentialNotice notice { Cc::CredentialNotice::Silent() };
    Wire::CapacityFields capacity {};
    FleetSampler sampler { std::nullopt, metrics, NodeFacts(), wall, HistoryPaths {}, logger };
    ConfiguredCredential credential { cfg, nullptr };
    NodeConditions conditions;

    PresenceFixture()
    {
        cfg.schedulers = { std::string { Scheduler } };

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
                               .credential = credential,
                               .notice = notice,
                               .capacity = capacity,
                               .endpoint = ThisMachine,
                               .logger = logger,
                               .conditions = conditions,
                               .roster = nullptr };
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
        auto link = Unwrap(SchedulerLink::For(cfg.schedulers));
        return AnnounceMachineOnce(Round(), link, dialer);
    }
};

} // namespace

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
    NodeConfig cfg;
    cfg.schedulers = { std::string { Scheduler } };
    AtomicMetricsSink metrics;
    NullLogger logger;
    SilentLoadSampler loadSampler;
    Testing::PlacedWallClock wall;
    Cc::CredentialNotice notice { Cc::CredentialNotice::Silent() };
    Wire::CapacityFields const capacity {};
    FleetSampler sampler { std::nullopt, metrics, NodeFacts(), wall, HistoryPaths {}, logger };
    ConfiguredCredential credential { cfg, nullptr };
    NodeConditions const conditions;

    REQUIRE(sampler.NextHistoryBatch(8).empty());

    Testing::ScriptedDialer dialer { { Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) } };
    auto link = Unwrap(SchedulerLink::For(cfg.schedulers));
    auto const accepted = AnnounceMachineOnce(PresenceRound { .loadSampler = loadSampler,
                                                              .cacheTier = nullptr,
                                                              .metrics = metrics,
                                                              .sampler = sampler,
                                                              .credential = credential,
                                                              .notice = notice,
                                                              .capacity = capacity,
                                                              .endpoint = ThisMachine,
                                                              .logger = logger,
                                                              .conditions = conditions,
                                                              .roster = nullptr },
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
