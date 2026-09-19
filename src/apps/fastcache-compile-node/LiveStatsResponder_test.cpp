// SPDX-License-Identifier: Apache-2.0
#include "LiveStatsResponder.hpp"
#include "LiveStatsSources.hpp"

#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/MembershipFakes.hpp>
#include <tests/NodeProofFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ListedMembership;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The peer every subscription below arrives from, unless a case says otherwise.
constexpr std::string_view Watcher = "10.0.0.7";

/// Captures what a case scripts, and counts how often it was asked.
class ScriptedSources final: public ILiveStatsSources
{
  public:
    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject subject) const override
    {
        ++captures;
        return LiveCapture { .body = { static_cast<std::byte>(subject), std::byte { 0xB0 }, std::byte { 0xD7 } },
                             .probe = probe };
    }

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return leadership;
    }

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "n1.test:6674";
    }

    /// @copydoc ILiveStatsSources::FleetText
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/) const override
    {
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = {} });
    }

    mutable std::size_t captures { 0 }; ///< Captures taken.
    LiveEventProbe probe {};            ///< What every capture says.

    /// Who leads; a leader by default, so a fleet case states only what it changes.
    std::optional<LiveLeadership> leadership { LiveLeadership { .leads = true, .leaderEndpoint = {} } };
};

/// What a stream writes, recorded; and what the endpoint would have seen, scripted.
class RecordingSink final: public IPushSink
{
  public:
    /// @param loop Where a parked push sleeps.
    explicit RecordingSink(IReactor& loop) noexcept:
        reactor { &loop }
    {
    }

    /// @copydoc IPushSink::Push
    [[nodiscard]] Task<PushOutcome> Push(std::vector<std::byte> frame, std::chrono::milliseconds hold) override
    {
        holds.push_back(hold);
        if (parkNextUntil.has_value())
        {
            auto const until = *parkNextUntil;
            parkNextUntil.reset();
            co_await SleepUntil { .reactor = reactor, .deadline = until };
        }
        if (nextOutcome != PushOutcome::Delivered)
            co_return std::exchange(nextOutcome, PushOutcome::Delivered);
        frames.push_back(std::move(frame));
        co_return PushOutcome::Delivered;
    }

    /// @copydoc IPushSink::Activity
    [[nodiscard]] PeerActivity Activity() const noexcept override
    {
        return activity;
    }

    /// @copydoc IPushSink::Stopping
    [[nodiscard]] bool Stopping() const noexcept override
    {
        return stopping;
    }

    /// @return The kind of every push so far, in order.
    [[nodiscard]] std::vector<Wire::PushKind> Kinds() const
    {
        std::vector<Wire::PushKind> kinds;
        for (auto const& frame: frames)
        {
            REQUIRE(Testing::StatusOf(frame) == Wire::Status::Push);
            auto const view = Wire::DecodePush(Testing::PayloadOf(frame));
            REQUIRE(view.has_value());
            kinds.push_back(Unwrap(view).kind);
        }
        return kinds;
    }

    /// @param index Which push.
    /// @return Its fields.
    [[nodiscard]] std::span<std::byte const> FieldsOf(std::size_t index) const
    {
        REQUIRE(index < frames.size());
        auto const view = Wire::DecodePush(Testing::PayloadOf(frames[index]));
        REQUIRE(view.has_value());
        return Unwrap(view).fields;
    }

    /// @return The tick of every snapshot so far, in order.
    [[nodiscard]] std::vector<std::uint64_t> SnapshotTicks() const
    {
        std::vector<std::uint64_t> ticks;
        for (auto const& frame: frames)
            if (auto const view = Wire::DecodePush(Testing::PayloadOf(frame));
                view.has_value() && view->kind == Wire::PushKind::Snapshot)
            {
                auto const snapshot = Wire::DecodeLiveSnapshot(view->fields);
                REQUIRE(snapshot.has_value());
                ticks.push_back(Unwrap(snapshot).tick);
            }
        return ticks;
    }

    std::vector<std::vector<std::byte>> frames;         ///< Every delivered push.
    std::vector<std::chrono::milliseconds> holds;       ///< The hold every push asked for.
    std::optional<TimePoint> parkNextUntil {};          ///< Park the next push until then.
    PushOutcome nextOutcome { PushOutcome::Delivered }; ///< What the next push answers.
    PeerActivity activity { PeerActivity::Quiet };      ///< What the read watch saw.
    bool stopping { false };                            ///< Whether the endpoint is stopping.

    /// Where a parked push sleeps. Public with the rest: this fake is its state.
    IReactor* reactor;
};

/// A SUBSCRIBE frame.
[[nodiscard]] std::vector<std::byte> SubscribeFrame(Wire::LiveSubject subject,
                                                    std::uint32_t cadenceMillis,
                                                    std::string token = {})
{
    return Wire::EncodeSubscribeRequest(
        Wire::SubscribeRequest { .subject = subject, .cadenceMillis = cadenceMillis, .dashboardToken = std::move(token) });
}

/// One subscription a case opened: what it pushed, and how it ended.
struct Stream
{
    explicit Stream(IReactor& reactor) noexcept:
        sink { reactor }
    {
    }

    RecordingSink sink;              ///< What it pushed.
    bool finished { false };         ///< The stream has returned.
    std::vector<std::byte> reply {}; ///< What it returned.
};

/// The machine whose connection proves an identity in the cases that ask for one.
constexpr std::string_view ProvenMachine = "watcher-node";

/// What that machine's connection proved: its id, under its own test key.
/// @return The identity.
[[nodiscard]] ProvenIdentity ProvenWatcher()
{
    return ProvenIdentity { .id = std::string { ProvenMachine },
                            .key = Testing::TestKeyPair(std::string { ProvenMachine }).PublicKey() };
}

/// Run one subscription on the reactor, as the endpoint would.
///
/// @param proved Whether this CONNECTION proved an identity the rig's roster admits, which the
///        endpoint records on the identity it hands down (#1428, #178). Since #1512 this surface
///        folds it, so a case can ask either question.
DetachedTask RunStream(
    LiveStatsResponder* responder, std::vector<std::byte> frame, std::string peer, bool proved, Stream* stream)
{
    auto identity = PeerIdentity { .host = std::move(peer) };
    if (proved)
        identity.proven = ProvenWatcher();
    stream->reply = co_await responder->Serve(frame, std::move(identity), &stream->sink);
    stream->finished = true;
}

/// Everything a case needs, over one manual clock.
///
/// **It ends every stream it opened before any member goes, and it owns every responder those
/// streams run on.** A stream resumed by that ending runs code in its responder, so a responder
/// that is a case's own local -- destroyed before the rig when an assertion unwinds the case --
/// would be resumed into after it is gone. Measured: two neutered builds turned a failing case
/// into an AddressSanitizer abort that way, which reports nothing about the assertion.
struct Rig
{
    Rig()
    {
        Testing::PublishKeyRoster(keys, { std::string { ProvenMachine } });
    }

    Rig(Rig const&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig const&) = delete;
    Rig& operator=(Rig&&) = delete;

    ~Rig()
    {
        for (auto const& stream: streams)
            stream->sink.stopping = true;
        clock.Advance(2s);
        reactor.Drain();
    }

    ManualClock clock;
    TestReactor reactor { clock };
    AtomicMetricsSink metrics;
    ScriptedSources sources;
    // MUTABLE, and bound once by reference like the production oracle: the removal case
    // below edits THIS object after the stream began, and a fixture that handed the
    // responder a fresh oracle would pass under the very defect re-gating exists for.
    ListedMembership membership { { std::string { Watcher }, "127.0.0.1", "10.0.0.8" },
                                  Distributed::MembershipParticipant::FleetMemberList };
    // The identity keys the cluster holds, MUTABLE for the same reason: the revocation case
    // below revokes one after the stream began.
    Distributed::KeyRosterMembership keys;
    // Composed as `NodeMembership` composes the node's oracle: the listed hosts and the key
    // roster under one fold, bound once.
    Distributed::AnyOfMembership oracle { { &membership, &keys } };
    LiveStatsResponder responder { sources, oracle, AdminCredential {}, reactor, metrics };
    std::deque<std::unique_ptr<LiveStatsResponder>> responders;
    std::deque<std::unique_ptr<Stream>> streams;

    /// A responder of the case's own, owned here for the reason above.
    [[nodiscard]] LiveStatsResponder& Responder(AdminCredential dashboard)
    {
        responders.push_back(std::make_unique<LiveStatsResponder>(sources, oracle, std::move(dashboard), reactor, metrics));
        return *responders.back();
    }

    /// Open a subscription and let it run as far as it can now.
    [[nodiscard]] Stream& Open(std::vector<std::byte> frame, std::string peer = std::string { Watcher })
    {
        return OpenOn(responder, std::move(frame), std::move(peer), false);
    }

    /// Open a subscription whose connection PROVED an identity the cluster holds.
    ///
    /// Its own spelling rather than a defaulted flag on `Open`: every existing case here is
    /// about a caller that proved nothing, and a default would let a new one be about a proof
    /// without saying so.
    [[nodiscard]] Stream& OpenProven(std::vector<std::byte> frame, std::string peer)
    {
        return OpenOn(responder, std::move(frame), std::move(peer), true);
    }

    /// Open a subscription on a responder of the case's own.
    [[nodiscard]] Stream& OpenOn(LiveStatsResponder& on, std::vector<std::byte> frame, std::string peer, bool proved = false)
    {
        streams.push_back(std::make_unique<Stream>(reactor));
        RunStream(&on, std::move(frame), std::move(peer), proved, streams.back().get());
        reactor.Drain();
        return *streams.back();
    }

    /// Move the clock to @p at and let every stream due by then run.
    void RunTo(std::chrono::milliseconds at)
    {
        clock.SetNow(TimePoint { std::chrono::duration_cast<Duration>(at) });
        reactor.Drain();
    }

    /// @return What @p counter reads.
    [[nodiscard]] std::uint64_t Read(IMetricsSink::Counter counter) const noexcept
    {
        return metrics.Read(counter);
    }
};

} // namespace

TEST_CASE("A subscription opens with what was granted, then a snapshot every granted cadence", "[node][livestats]")
{
    Rig rig;

    // 1200 ms against the node floor of 500 is three ticks, so snapshots are owed at 0, 3 and 6.
    auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 1200));
    REQUIRE_FALSE(stream.finished);
    REQUIRE(stream.sink.frames.size() == 2);
    CHECK(stream.sink.Kinds() == std::vector { Wire::PushKind::Subscribed, Wire::PushKind::Snapshot });

    auto const granted = Wire::DecodeLiveSubscribed(stream.sink.FieldsOf(0));
    REQUIRE(granted.has_value());
    CHECK(Unwrap(granted).subject == Wire::LiveSubject::Node);
    CHECK(Unwrap(granted).grantedCadenceMillis == 1200);
    CHECK(Unwrap(granted).statsLayout == StatsReadingLayout);
    CHECK(Unwrap(granted).endpoint == "n1.test:6674");

    for (auto const at: { 500ms, 1000ms, 1500ms, 2000ms, 2500ms, 3000ms })
        rig.RunTo(at);
    CHECK(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 3, 6 });

    // Three cadences of 1200 ms is under the wire's floor, so every push holds for the floor.
    CHECK(std::ranges::all_of(stream.sink.holds, [](auto hold) { return hold == Wire::MinLiveStreamWriteStall; }));
    CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsOpened) == 1);

    stream.sink.stopping = true;
    rig.RunTo(3500ms);
    REQUIRE(stream.finished);
    CHECK(Testing::StatusOf(stream.reply) == Wire::Status::Ok);
}

TEST_CASE("A slow cadence holds a parked push for its own idle bound, not the floor", "[node][livestats]")
{
    Rig rig;
    auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Cache, 30'000));
    REQUIRE_FALSE(stream.sink.holds.empty());
    CHECK(stream.sink.holds.front() == 30'000ms * Wire::LiveIdleCadences);
}

TEST_CASE("Two watchers on one tick share one render, and the render counter moves with ticks", "[node][livestats]")
{
    // **The acceptance case for render-once.** Two subscribers on one subject wake on the same
    // tick; the first captures and the second shares the bytes, so the counter rises once per
    // tick. A render inside the per-subscriber path reads two per tick here.
    Rig rig;
    auto& first = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000), "127.0.0.1");
    auto& second = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000), "127.0.0.1");
    CHECK(rig.Read(IMetricsSink::Counter::LiveSnapshotsRendered) == 1);
    CHECK(rig.sources.captures == 1);

    for (auto const at: { 1000ms, 2000ms, 3000ms })
        rig.RunTo(at);
    CHECK(rig.Read(IMetricsSink::Counter::LiveSnapshotsRendered) == 4);
    CHECK(first.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 1, 2, 3 });
    CHECK(second.sink.SnapshotTicks() == first.sink.SnapshotTicks());

    // And they carry the SAME bytes, which is what sharing a render means to a client.
    CHECK(first.sink.frames.back() == second.sink.frames.back());
}

TEST_CASE("A watcher whose push parks is told what it missed, and nobody else waits for it", "[node][livestats]")
{
    Rig rig;
    auto& slow = rig.Open(SubscribeFrame(Wire::LiveSubject::Cache, 500));
    auto& quick = rig.Open(SubscribeFrame(Wire::LiveSubject::Cache, 500), "10.0.0.8");

    // The slow watcher's tick-1 snapshot parks until 2000 ms: a reader that stopped for four ticks.
    slow.sink.parkNextUntil = TimePoint { std::chrono::duration_cast<Duration>(2000ms) };
    for (auto const at: { 500ms, 1000ms, 1500ms, 2000ms })
        rig.RunTo(at);

    // Nobody waited: the quick watcher kept its cadence the whole time.
    CHECK(quick.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 1, 2, 3, 4 });
    CHECK(std::ranges::count(quick.sink.Kinds(), Wire::PushKind::Gap) == 0);

    // The slow one wakes on tick 4 owing ticks 2 and 3, says so, and carries on at the present.
    CHECK(slow.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 1, 4 });
    auto const kinds = slow.sink.Kinds();
    auto const gapAt = std::ranges::find(kinds, Wire::PushKind::Gap);
    REQUIRE(gapAt != kinds.end());
    auto const gap = Wire::DecodeLiveGap(slow.sink.FieldsOf(static_cast<std::size_t>(gapAt - kinds.begin())));
    REQUIRE(gap.has_value());
    CHECK(Unwrap(gap).dropped == 2);
    CHECK(Unwrap(gap).firstTick == 2);
    CHECK(Unwrap(gap).lastTick == 3);
    CHECK(rig.Read(IMetricsSink::Counter::LiveSnapshotsSkipped) == 2);
}

TEST_CASE("A watcher removed from the membership loses its stream on the next tick", "[node][livestats]")
{
    // **The removal direction**, which fails OPEN when nothing re-checks a live connection.
    Rig rig;
    auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 500));

    SECTION("an unrelated removal keeps the stream")
    {
        rig.membership.Remove("10.0.0.8");
        rig.RunTo(500ms);
        CHECK_FALSE(stream.finished);
        CHECK(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 1 });
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRevoked) == 0);
    }

    SECTION("removing the watcher ends it, named and counted")
    {
        rig.membership.Remove(Watcher);
        rig.RunTo(500ms);
        REQUIRE(stream.finished);
        CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::NotAMember);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRevoked) == 1);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember) == 0);
    }
}

TEST_CASE("A key holder streams from an address on no list, and keeps streaming past the first tick", "[node][livestats]")
{
    // #1512, and what distinguishes the FIX from the shape it was avoiding is that the stream is
    // STILL RUNNING at the third sample. MEASURED against a door-only build (the proof folded at
    // `RefuseWatcher` and not at `Recheck`): it produces ticks `{0, 1}` and then ends -- the
    // opening snapshot plus one more. So "it streamed", "it got a snapshot" and even "it got two"
    // are all true of the broken build, and only `{0, 1, 2}` with the stream unfinished is not.
    constexpr std::string_view Roaming = "203.0.113.41";

    Rig rig;
    // The premise, asserted rather than assumed: this address is on no list, so anything that
    // admits it below did so for the proof and not because the fixture was generous.
    REQUIRE(rig.membership.Explain(Roaming).verdict == Distributed::Membership::Outsider);

    SECTION("proved: it streams, and goes on streaming")
    {
        auto& stream = rig.OpenProven(SubscribeFrame(Wire::LiveSubject::Node, 500), std::string { Roaming });
        REQUIRE_FALSE(stream.finished);

        rig.RunTo(500ms);
        rig.RunTo(1000ms);
        CHECK_FALSE(stream.finished);
        CHECK(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 1, 2 });
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRevoked) == 0);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember) == 0);
    }

    SECTION("the same address proving nothing is still refused at the door")
    {
        // The control, and it is what says the section above is about the PROOF rather than
        // about this address having become admissible to everybody.
        auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 500), std::string { Roaming });
        REQUIRE(stream.finished);
        CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::NotAMember);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember) == 1);
    }
}

TEST_CASE("A proven watcher dropped from the member list keeps its stream, because the key still admits it",
          "[node][livestats]")
{
    // The exact inverse of *A watcher removed from the membership loses its stream on the next
    // tick* above, and both are correct: `--fleet-member` is ONE route to admission and what the
    // cluster agrees is ADDED rather than substituted, so a host that holds the key is admitted
    // by the key after the list stops naming it (#1471's union, folded by `ExplainConnection`).
    //
    // It is here because it is the operator-visible consequence, and the direction an operator
    // gets wrong: editing the list is not how a proven machine is removed. Revoking its key is,
    // and that ends the stream on the next tick -- the case after this one.
    Rig rig;
    auto& stream = rig.OpenProven(SubscribeFrame(Wire::LiveSubject::Node, 500), std::string { Watcher });
    REQUIRE_FALSE(stream.finished);
    rig.RunTo(500ms);
    REQUIRE_FALSE(stream.finished);

    rig.membership.Remove(std::string { Watcher });
    rig.RunTo(1000ms);
    CHECK_FALSE(stream.finished);
    CHECK(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 1, 2 });
    CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRevoked) == 0);
}

TEST_CASE("A proven watcher whose key is revoked loses its stream on the next tick, from any address",
          "[node][livestats][revoke]")
{
    // #178: removing one machine is revoking its key, and a revocation has to reach a stream that
    // is ALREADY RUNNING -- a gate asked once at subscribe time would leave a forgotten machine
    // watching the fleet for as long as it keeps the connection open. The watcher dials from an
    // address `--fleet-member` still lists, so the key's tombstone is what ends it.
    Rig rig;
    auto& stream = rig.OpenProven(SubscribeFrame(Wire::LiveSubject::Node, 500), std::string { Watcher });
    REQUIRE_FALSE(stream.finished);
    rig.RunTo(500ms);
    REQUIRE_FALSE(stream.finished);

    rig.keys.Publish({}, { ProvenWatcher().key });
    rig.RunTo(1000ms);
    CHECK(stream.finished);
    CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::NotAMember);
    // Counted as the forgotten MACHINE, the gate's own row, rather than as the generic removal
    // of a host: the two are opposite diagnoses, and an operator watching a subscriber drop has
    // to be able to tell a revoked key from an edited list.
    CHECK(rig.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 1);
    CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRevoked) == 0);

    // The control: a watcher at the same listed address that proved nothing is untouched by a
    // revocation of a key it never presented.
    auto& unproved = rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 500), std::string { Watcher });
    REQUIRE_FALSE(unproved.finished);
    rig.RunTo(1500ms);
    CHECK_FALSE(unproved.finished);
}

TEST_CASE("A fleet stream ends the tick its node stops leading, naming the new leader", "[node][livestats]")
{
    Rig rig;
    auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000), "127.0.0.1");
    REQUIRE_FALSE(stream.finished);

    rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = "10.0.0.2:6674" };
    rig.RunTo(1000ms);

    REQUIRE(stream.finished);
    CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::NotLeader);
    auto const refusal = Wire::DecodeErrorPayload(Testing::PayloadOf(stream.reply));
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).second == "10.0.0.2:6674");
    CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedNotLeader) == 1);
}

TEST_CASE("The fleet streams to a holder of the dashboard credential, and with none to this machine", "[node][livestats]")
{
    Rig rig;

    SECTION("with no token file, a remote member is refused and this machine is not")
    {
        auto const& remote = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000));
        CHECK(remote.sink.frames.empty());
        CHECK(Testing::ErrorOf(remote.reply) == Wire::ErrorCode::Unauthenticated);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated) == 1);

        CHECK_FALSE(rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000), "127.0.0.1").sink.frames.empty());

        // The node subject is not the fleet map and needs no credential from anywhere.
        CHECK_FALSE(rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 500)).sink.frames.empty());
    }

    SECTION("with a token file, the token decides, from anywhere")
    {
        auto& guarded = rig.Responder(AdminCredential { "s3cret" });
        auto const& wrong = rig.OpenOn(guarded, SubscribeFrame(Wire::LiveSubject::Fleet, 1000, "guess"), "127.0.0.1");
        CHECK(wrong.sink.frames.empty());
        CHECK(Testing::ErrorOf(wrong.reply) == Wire::ErrorCode::Unauthenticated);

        auto const& right =
            rig.OpenOn(guarded, SubscribeFrame(Wire::LiveSubject::Fleet, 1000, "s3cret"), std::string { Watcher });
        CHECK_FALSE(right.sink.frames.empty());
    }

    SECTION("a follower redirects a watcher, uncounted")
    {
        rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = "10.0.0.2:6674" };
        auto const& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000), "127.0.0.1");
        CHECK(stream.sink.frames.empty());
        CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::NotLeader);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedNotLeader) == 0);
    }

    SECTION("a node with no scheduler serves no fleet, and says it is served elsewhere")
    {
        rig.sources.leadership = std::nullopt;
        auto const& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 1000), "127.0.0.1");
        CHECK(stream.sink.frames.empty());
        CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::DispatchNotPermitted);
    }
}

TEST_CASE("A stranger, a malformed request and a full node are each refused and counted apart", "[node][livestats]")
{
    Rig rig;

    SECTION("a stranger")
    {
        auto const& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 500), "192.0.2.1");
        REQUIRE(stream.finished);
        CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::NotAMember);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember) == 1);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRevoked) == 0);
    }

    SECTION("a request naming a subject this build does not serve")
    {
        auto frame = SubscribeFrame(Wire::LiveSubject::Node, 500);
        frame[Wire::RequestHeaderSize + WireFields::FieldPrefixSize] = std::byte { 0x7F };
        auto const& stream = rig.Open(std::move(frame));
        REQUIRE(stream.finished);
        CHECK(Testing::ErrorOf(stream.reply) == Wire::ErrorCode::MalformedFrame);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed) == 1);
    }

    SECTION("the subscription past the cap")
    {
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Wire::MaxLiveSubscriptions))
            (void) rig.Open(SubscribeFrame(Wire::LiveSubject::Cache, 500));
        CHECK(rig.responder.ActiveSubscriptions() == Wire::MaxLiveSubscriptions);

        auto const& refused = rig.Open(SubscribeFrame(Wire::LiveSubject::Cache, 500));
        REQUIRE(refused.finished);
        CHECK(Testing::ErrorOf(refused.reply) == Wire::ErrorCode::EndpointBusy);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedAtCapacity) == 1);

        // A place is given back when a stream ends, however it ends.
        rig.streams.front()->sink.activity = PeerActivity::Departed;
        rig.RunTo(500ms);
        CHECK(rig.streams.front()->finished);
        CHECK(rig.responder.ActiveSubscriptions() == Wire::MaxLiveSubscriptions - 1);
    }
}

TEST_CASE("A watcher that leaves, resets, sends a request or stops reading ends its stream the way it should",
          "[node][livestats]")
{
    Rig rig;
    auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Node, 500));

    SECTION("a goodbye closes without a reply")
    {
        stream.sink.activity = PeerActivity::Departed;
        rig.RunTo(500ms);
        REQUIRE(stream.finished);
        CHECK(stream.reply.empty());
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedByClient) == 1);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedByReset) == 0);
    }

    SECTION("a reset closes without a reply, counted apart")
    {
        stream.sink.activity = PeerActivity::Reset;
        rig.RunTo(500ms);
        REQUIRE(stream.finished);
        CHECK(stream.reply.empty());
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedByReset) == 1);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedByClient) == 0);
    }

    SECTION("a request ends it in order, so the endpoint can serve what was sent")
    {
        stream.sink.activity = PeerActivity::Sent;
        rig.RunTo(500ms);
        REQUIRE(stream.finished);
        CHECK(Testing::StatusOf(stream.reply) == Wire::Status::Ok);
    }

    SECTION("a push the endpoint gave up on is a stall, not a departure")
    {
        stream.sink.nextOutcome = PushOutcome::Stalled;
        rig.RunTo(500ms);
        REQUIRE(stream.finished);
        CHECK(stream.reply.empty());
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsStalled) == 1);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedByClient) == 0);
        CHECK(rig.Read(IMetricsSink::Counter::LiveSubscriptionsEndedByReset) == 0);
    }
}

TEST_CASE("An event is pushed as it is observed, with the state it describes on the same tick", "[node][livestats]")
{
    Rig rig;

    // Five ticks per cadence, so a snapshot at tick 2 can only have been owed by the event.
    auto& stream = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 5000), "127.0.0.1");
    rig.RunTo(1000ms);
    CHECK(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0 });

    rig.sources.probe.workers = { LiveFact { .key = "w1:6674|fp", .detail = "w1:6674" } };
    rig.RunTo(2000ms);

    auto const kinds = stream.sink.Kinds();
    REQUIRE(kinds.size() == 4);
    CHECK(kinds[2] == Wire::PushKind::Event);
    auto const event = Wire::DecodeLiveEvent(stream.sink.FieldsOf(2));
    REQUIRE(event.has_value());
    CHECK(Unwrap(event).kind == Wire::LiveEventKind::WorkerRegistered);
    CHECK(Unwrap(event).detail == "w1:6674");
    CHECK(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0, 2 });

    // A watcher arriving now is not replayed what happened before it came.
    auto const& late = rig.Open(SubscribeFrame(Wire::LiveSubject::Fleet, 5000), "127.0.0.1");
    CHECK(std::ranges::count(late.sink.Kinds(), Wire::PushKind::Event) == 0);
}

TEST_CASE("Detaching the sources ends every stream in order at its next tick", "[node][livestats]")
{
    ManualClock clock;
    TestReactor reactor { clock };
    AtomicMetricsSink metrics;
    ScriptedSources sources;
    LiveStatsSourceSlot slot;
    ListedMembership membership { { std::string { Watcher } }, Distributed::MembershipParticipant::FleetMemberList };
    LiveStatsResponder responder { slot, membership, AdminCredential {}, reactor, metrics };

    Stream stream { reactor };
    {
        auto const attached = slot.Attach(sources);
        RunStream(&responder, SubscribeFrame(Wire::LiveSubject::Cache, 500), std::string { Watcher }, false, &stream);
        reactor.Drain();
        REQUIRE(stream.sink.SnapshotTicks() == std::vector<std::uint64_t> { 0 });
    }

    clock.Advance(500ms);
    reactor.Drain();
    REQUIRE(stream.finished);
    CHECK(Testing::StatusOf(stream.reply) == Wire::Status::Ok);
    CHECK(sources.captures == 1);
}

TEST_CASE("A node status probe keys on states and leaves counts to the words", "[node][livestats]")
{
    Wire::NodeStatusFields status;
    CHECK_FALSE(ProbeNodeStatus(status).leadership.has_value());
    CHECK_FALSE(ProbeNodeStatus(status).survey.has_value());

    status.runtime.schedulerRole = Wire::WireSchedulerRole::Follower;
    status.runtime.leaderEndpoint = "n2:6674";
    status.runtime.toolchains = Wire::ToolchainState::Surveying;
    status.runtime.toolchainsServed = 1;
    status.runtime.toolchainsDiscovered = 4;
    auto const surveying = ProbeNodeStatus(status);

    status.runtime.toolchainsServed = 3;
    CHECK(DiffLiveEvents(surveying, ProbeNodeStatus(status)).empty());

    status.runtime.leaderEndpoint = "n3:6674";
    auto const moved = DiffLiveEvents(surveying, ProbeNodeStatus(status));
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].kind == Wire::LiveEventKind::LeadershipChanged);
    CHECK(moved[0].detail == "n3:6674");
}
