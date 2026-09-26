// SPDX-License-Identifier: Apache-2.0
#include "CliEndpoint.hpp"
#include "LiveSourceRig.hpp"

#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using namespace std::chrono_literals;

namespace
{

/// A stream that dials and then answers @p frames.
/// @param frames Each read's answer, in order.
/// @return The stream.
[[nodiscard]] ScriptedStream Stream(std::vector<ScriptedFrame> frames)
{
    return ScriptedStream { .refused = std::nullopt, .frames = std::move(frames) };
}

/// Run the source's next @p operations -- one dial or one read each, one pool turn each -- and leave
/// the operation after them outstanding on the pool, as a live stream waiting for its next push is.
///
/// `Settle()` runs a scripted stream to its end, which queues the silence and a retry behind every
/// reading; a case about something other than the stream reads the stream this far and no further.
/// @param rig The rig.
/// @param operations How many dials and reads to run.
void Pump(Rig& rig, int operations)
{
    rig.reactor.drain();
    for ([[maybe_unused]] auto const turn: std::views::iota(0, operations))
    {
        rig.pool.drain();
        rig.reactor.drain();
    }
}

/// Every endpoint @p subscription dialled, as text.
/// @param subscription The subscription.
/// @return The endpoints, in order.
[[nodiscard]] std::vector<std::string> DialledTexts(ScriptedSubscription const& subscription)
{
    auto texts = std::vector<std::string> {};
    for (auto const& endpoint: subscription.Dialled())
        texts.push_back(EndpointText(endpoint));
    return texts;
}

/// The rig's `--addr`, as text.
/// @return `127.0.0.1:6674`.
[[nodiscard]] std::string RigAddress()
{
    return EndpointText(RigEndpoint());
}

/// Whether @p event is a failed sample with @p outcome.
/// @param event The event.
/// @param outcome The outcome it must carry.
/// @return True when it is.
[[nodiscard]] bool FailedWith(std::optional<DashboardEvent> const& event, Outcome outcome)
{
    return event.has_value() && event->kind == DashboardEventKind::SampleFailed && event->outcome == outcome;
}

/// The note an event carries, or empty when there was none.
/// @param event The event.
/// @return The note.
[[nodiscard]] std::string NoteOf(std::optional<DashboardEvent> const& event)
{
    return event.has_value() ? event->note : std::string {};
}

} // namespace

TEST_CASE("a live source subscribes with the operator's interval and delivers each pushed reading stamped when it arrived",
          "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: the grant says 3 s where the operator asked 2 s, so a source stating its own
    // interval as the cadence is caught; the read of the first reading is handed off at the epoch and runs
    // 700 ms later, so a stamp taken where it was ASKED for is caught; and the two readings differ, so a
    // source repeating one is caught. No timer is pending while the stream answers: the server keeps the
    // cadence, and a source polling on its own clock would have armed one.
    auto const first = ReadingOpened(1);
    auto const second = ReadingOpened(2);
    REQUIRE(first != second);

    Rig rig;
    ScriptedSubscription stream { { Stream({ GrantFrame(CompileCacheWire::LiveSubject::Cache, 3000ms),
                                             CacheReadingFrame(first, 1),
                                             CacheReadingFrame(second, 2) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };

    // The dial is handed to the pool and has not run: nothing blocking ever runs on the reactor.
    rig.reactor.drain();
    CHECK(stream.Opens() == 0);
    CHECK(rig.pool.pendingSubmissions() == 1);

    // The dial, then the grant; the read of the first reading is now out, asked at the epoch.
    Pump(rig, 2);
    REQUIRE(stream.Opens() == 1);
    CHECK(DialledTexts(stream) == std::vector<std::string> { RigAddress() });
    REQUIRE(stream.Requests().size() == 1);
    CHECK(stream.Requests().front().subject == CompileCacheWire::LiveSubject::Cache);
    CHECK(stream.Requests().front().cadenceMillis == Interval.count());
    CHECK(stream.Requests().front().dashboardToken.empty());
    CHECK(stream.Expected() == std::vector<std::chrono::milliseconds> { 3000ms });

    rig.clock.advance(700ms);
    rig.pool.drain();
    rig.reactor.drain();
    auto const one = NextDue(rig, source);
    REQUIRE(KindOf(one) == DashboardEventKind::Sample);
    CHECK(Unwrap(one).at == core::platform::SteadyTimePoint {} + 700ms);
    REQUIRE(Unwrap(one).reading.has_value());
    CHECK(Unwrap(Unwrap(one).reading) == first);
    CHECK(Unwrap(one).where == "127.0.0.1:6674");
    CHECK(Unwrap(one).cadence == std::optional<std::chrono::milliseconds> { 3000ms });
    CHECK_FALSE(Unwrap(one).document.has_value());
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(rig.reactor.pendingTimers() == 0);

    rig.pool.drain();
    rig.reactor.drain();
    auto const two = NextDue(rig, source);
    REQUIRE(KindOf(two) == DashboardEventKind::Sample);
    REQUIRE(Unwrap(two).reading.has_value());
    CHECK(Unwrap(Unwrap(two).reading) == second);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(rig.reactor.pendingTimers() == 0);
    CHECK(stream.Opens() == 1);

    CloseAndDrain(rig, source);
}

TEST_CASE("a live source stream keeps no timer: a push slower than the interval is waited for and never overlapped",
          "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: three intervals pass with a read outstanding. A source that kept a cadence of its
    // own would arm a timer or hand a second read or dial to the pool; one that waits for the server's push
    // has exactly the one read out and nothing else.
    Rig rig;
    LiveEventSource source { rig.Parts() };
    Pump(rig, 2);
    REQUIRE(rig.subscription.Reads() == 1);

    rig.clock.advance(3 * Interval);
    rig.reactor.drain();
    CHECK(rig.pool.pendingSubmissions() == 1);
    CHECK(rig.reactor.pendingTimers() == 0);
    CHECK(rig.subscription.Opens() == 1);
    CHECK(rig.subscription.Reads() == 1);

    CloseAndDrain(rig, source);
}

TEST_CASE("a keystroke that arrives while a dial or a read is outstanding is delivered first", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };

    // The reactor has run and the pool has not: the dial is handed off and has not happened, which
    // is the window this case is about.
    rig.reactor.drain();
    CHECK(rig.pool.pendingSubmissions() == 1);

    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" });
    auto const key = NextDue(rig, source);
    CHECK(KindOf(key) == DashboardEventKind::Key);
    CHECK((key.has_value() && key->keys == "x"));
    // What it overtook really was still out: the dial had not run.
    CHECK(rig.subscription.Opens() == 0);

    // And the same while a READ is out: the dial and the grant have run, the reading has not.
    Pump(rig, 2);
    REQUIRE(rig.subscription.Reads() == 1);
    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "y" });
    auto const during = NextDue(rig, source);
    CHECK(KindOf(during) == DashboardEventKind::Key);
    CHECK((during.has_value() && during->keys == "y"));
    CHECK(rig.subscription.Reads() == 1);

    rig.pool.drain();
    rig.reactor.drain();
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);

    CloseAndDrain(rig, source);
}

TEST_CASE(
    "quitting during a read ends the dashboard at once and leaves the stream and the source drains when the read returns",
    "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: the stream has a second reading scripted, and its read is still on the pool when
    // the operator quits. The loop returns without it; the source LEAVES the stream before the read
    // returns, because a half-close is what makes a real node close and a blocked read return; and the
    // second reading, when it does come back, is dropped rather than drawn.
    Rig rig;
    ScriptedSubscription stream { { CacheStream({ ReadingOpened(1), ReadingOpened(2) }) } };
    auto parts = rig.SpokenParts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.submit(run.handle());
    Pump(rig, 3);
    REQUIRE(rig.sink.frames == 1);
    CHECK(rig.pool.pendingSubmissions() == 1);
    CHECK(stream.Leaves() == 0);

    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.reactor.drain();

    // The operator was not made to wait for the read: the loop has returned, it left the stream,
    // and the read is still out.
    CHECK(StopOf(exit) == DashboardStop::Quit);
    CHECK(stream.Leaves() == 1);
    CHECK(stream.Reads() == 2);

    // The SESSION is over and the SOURCE is not: the read is still on the pool, inside a
    // subscription the caller must therefore not destroy yet.
    auto drained = false;
    auto wait = AwaitDrained(&source, &drained);
    rig.reactor.submit(wait.handle());
    rig.reactor.drain();
    CHECK_FALSE(drained);

    rig.Settle();
    CHECK(drained);
    CHECK(stream.Reads() == 3);
    // Returned into a closed session, the second reading is dropped rather than drawn, and nothing re-dials.
    CHECK(rig.sink.frames == 1);
    CHECK(stream.Opens() == 1);
    CHECK(rig.reactor.pendingTimers() == 0);
    CHECK(rig.reactor.pendingSubmissions() == 0);
    CHECK(rig.pool.pendingSubmissions() == 0);

    // Whatever the checks above found, nothing may be left parked when the tasks go.
    source.Close();
    rig.Settle();
}

TEST_CASE("closing a source that waits to subscribe again retires its timer without waiting for the deadline",
          "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: the stream went silent and the retry is armed. Drained, with the timer gone from the
    // heap, the clock where it was and no second dial: the close is heard on the reactor's next turn.
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.Settle();
    CHECK(rig.reactor.pendingTimers() == 1);

    CloseAndDrain(rig, source);
    CHECK(rig.subscription.Opens() == 1);
}

TEST_CASE("a closed live source answers Detached, including to a read already waiting", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    Pump(rig, 3);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    auto waiting = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &waiting);
    rig.reactor.submit(task.handle());
    rig.reactor.drain();
    CHECK_FALSE(waiting.has_value());

    source.Close();
    rig.reactor.drain();
    CHECK(KindOf(waiting) == DashboardEventKind::Detached);
    CHECK((waiting.has_value() && waiting->note == SessionClosedNote));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Detached);

    CloseAndDrain(rig, source);
}

TEST_CASE("a stream that ends is one gap and the next subscription is one interval later at the address asked",
          "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: one interval less a millisecond after the end, still ONE dial -- a source that
    // re-subscribes at once, in a loop, dials a second time there -- and at the interval exactly the
    // second, at `--addr`. The end is `Unreachable` with its reason rather than the session's end.
    auto const second = ReadingOpened(2);

    SECTION("the server ends it in order")
    {
        Rig rig;
        ScriptedSubscription stream { { Stream({ GrantFrame(CompileCacheWire::LiveSubject::Cache),
                                                 CacheReadingFrame(ReadingOpened(1), 1),
                                                 EndedFrame() }),
                                        CacheStream({ second }) } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        auto const ended = NextDue(rig, source);
        CHECK(FailedWith(ended, Outcome::Unreachable));
        CHECK(NoteOf(ended) == std::format("{}: the server ended the stream", RigAddress()));
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        CHECK(stream.Opens() == 1);
        CHECK(rig.reactor.pendingTimers() == 1);

        rig.clock.advance(Interval - 1ms);
        rig.Settle();
        CHECK(stream.Opens() == 1);

        rig.clock.advance(1ms);
        rig.Settle();
        REQUIRE(stream.Opens() == 2);
        CHECK(DialledTexts(stream).back() == RigAddress());
        auto const again = NextDue(rig, source);
        REQUIRE(KindOf(again) == DashboardEventKind::Sample);
        CHECK(Unwrap(again).reading == std::optional<StatsReading> { second });

        CloseAndDrain(rig, source);
    }

    SECTION("it goes silent past its idle bound")
    {
        Rig rig;
        ScriptedSubscription stream { { CacheStream({ ReadingOpened(1) }), CacheStream({ second }) } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        auto const silent = NextDue(rig, source);
        CHECK(FailedWith(silent, Outcome::Unreachable));
        CHECK(NoteOf(silent) == std::format("{}: {}", RigAddress(), SilentStream));
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

        rig.clock.advance(Interval - 1ms);
        rig.Settle();
        CHECK(stream.Opens() == 1);
        rig.clock.advance(1ms);
        rig.Settle();
        CHECK(stream.Opens() == 2);
        CHECK(DialledTexts(stream).back() == RigAddress());

        CloseAndDrain(rig, source);
    }
}

TEST_CASE("a dial that fails is a gap and is retried once per interval and never in a loop",
          "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: the dial fails every time. A source retrying at once dials again inside one
    // `Settle()`; one that ends the session on a failed dial answers `Detached`; this one records a gap
    // stamped when the dial answered, and dials exactly once more per interval.
    Rig rig;
    ScriptedSubscription stream { { RefusedDial("connection refused") } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    rig.reactor.drain();
    rig.clock.advance(40ms);
    rig.Settle();

    CHECK(stream.Opens() == 1);
    auto const failed = NextDue(rig, source);
    CHECK(FailedWith(failed, Outcome::Unreachable));
    CHECK(NoteOf(failed) == "connection refused");
    CHECK((failed.has_value() && failed->at == core::platform::SteadyTimePoint {} + 40ms));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    rig.Settle();
    CHECK(stream.Opens() == 1);

    rig.clock.advance(Interval);
    rig.Settle();
    CHECK(stream.Opens() == 2);
    rig.Settle();
    CHECK(stream.Opens() == 2);
    CHECK(FailedWith(NextDue(rig, source), Outcome::Unreachable));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    CloseAndDrain(rig, source);
}

TEST_CASE("a NotLeader naming the leader is followed at once and the reading names the leader",
          "[cli][live][source][redirect]")
{
    // WHAT DISTINGUISHES: the follow costs no gap and no interval. A source treating the refusal as a
    // failure queues a `SampleFailed` first and dials the second time an interval later, at `--addr`; one
    // that followed but kept naming `--addr` would put the wrong address on the panel's source line. And
    // the retry after the leader's stream goes silent is at `--addr` again, not at the leader it followed.
    Rig rig;
    ScriptedSubscription stream { { Stream({ RefusalFrame(CompileCacheWire::ErrorCode::NotLeader, "10.0.0.9:7071") }),
                                    CacheStream({ ReadingOpened(1) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    CHECK(DialledTexts(stream) == std::vector<std::string> { RigAddress(), "10.0.0.9:7071" });
    auto const sample = NextDue(rig, source);
    REQUIRE(KindOf(sample) == DashboardEventKind::Sample);
    CHECK(Unwrap(sample).where == "10.0.0.9:7071");
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    rig.clock.advance(Interval);
    rig.Settle();
    REQUIRE(stream.Opens() >= 3);
    CHECK(DialledTexts(stream)[2] == RigAddress());

    CloseAndDrain(rig, source);
}

TEST_CASE("leader redirects are bounded and the count restarts with every subscription from the address asked",
          "[cli][live][source][redirect]")
{
    // WHAT DISTINGUISHES: four nodes each name another. The third redirect is not followed -- a source with no
    // bound dials a fourth node inside one `Settle()` -- and is a gap naming the bound and the last node named.
    // The retry starts at `--addr`, and the redirect it meets there IS followed: a source that kept counting
    // across subscriptions could never follow a leader again once it had run out.
    Rig rig;
    auto const redirect = [](std::string to) {
        return Stream({ RefusalFrame(CompileCacheWire::ErrorCode::NotLeader, std::move(to)) });
    };
    ScriptedSubscription stream { { redirect("10.0.0.1:7071"),
                                    redirect("10.0.0.2:7071"),
                                    redirect("10.0.0.3:7071"),
                                    redirect("10.0.0.4:7071"),
                                    CacheStream({ ReadingOpened(1) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    REQUIRE(MaxLeaderRedirects == 2);
    CHECK(DialledTexts(stream) == std::vector<std::string> { RigAddress(), "10.0.0.1:7071", "10.0.0.2:7071" });
    auto const bounded = NextDue(rig, source);
    CHECK(FailedWith(bounded, Outcome::Unreachable));
    CHECK(NoteOf(bounded).contains(std::format("followed {} leader redirects", MaxLeaderRedirects)));
    CHECK(NoteOf(bounded).contains("10.0.0.3:7071"));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(rig.reactor.pendingTimers() == 1);

    rig.clock.advance(Interval);
    rig.Settle();
    REQUIRE(stream.Opens() >= 5);
    CHECK(DialledTexts(stream)[3] == RigAddress());
    CHECK(DialledTexts(stream)[4] == "10.0.0.4:7071");
    auto const followed = NextDue(rig, source);
    REQUIRE(KindOf(followed) == DashboardEventKind::Sample);
    CHECK(Unwrap(followed).where == "10.0.0.4:7071");

    CloseAndDrain(rig, source);
}

TEST_CASE("a refusal before any reading ends the session Refused and names the credential flag",
          "[cli][live][source][refusal]")
{
    // WHAT DISTINGUISHES: the refusal is `Refused`, not `Unreachable`; it names the flag that carries the
    // credential THIS subject presents -- a cache or node session's `--token-file`, a fleet session's
    // `--dashboard-token-file`; and it ends the session (`Detached`, no retry armed). A source that retried
    // would wait forever on a credential nobody is going to type in.
    auto const refusal =
        Stream({ RefusalFrame(CompileCacheWire::ErrorCode::Unauthenticated, "no credential was presented") });

    SECTION("a cache session")
    {
        Rig rig;
        ScriptedSubscription stream { { refusal } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        auto const refused = NextDue(rig, source);
        CHECK(FailedWith(refused, Outcome::Refused));
        CHECK(NoteOf(refused).contains("127.0.0.1:6674 refused the subscription: no credential was presented"));
        CHECK(NoteOf(refused).contains("--token-file"));
        CHECK_FALSE(NoteOf(refused).contains("--dashboard-token-file"));
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        CHECK(FinishedItself(NextDue(rig, source)));
        CHECK(rig.reactor.pendingTimers() == 0);
        CHECK(stream.Opens() == 1);

        CloseAndDrain(rig, source);
    }

    SECTION("a fleet session with no dashboard credential")
    {
        // The tail says where to pass the credential -- and restates nothing the node's detail already said.
        Rig rig;
        ScriptedSubscription stream { { refusal } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        parts.subject = CompileCacheWire::LiveSubject::Fleet;
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        REQUIRE(stream.Requests().size() == 1);
        CHECK(stream.Requests().front().dashboardToken.empty());
        auto const refused = NextDue(rig, source);
        CHECK(FailedWith(refused, Outcome::Refused));
        CHECK(NoteOf(refused)
              == "127.0.0.1:6674 refused the subscription: no credential was presented; present the dashboard "
                 "credential with --dashboard-token-file");
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        CHECK(FinishedItself(NextDue(rig, source)));

        CloseAndDrain(rig, source);
    }

    SECTION("a fleet session, which presents the dashboard credential")
    {
        Rig rig;
        ScriptedSubscription stream { { refusal } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        parts.subject = CompileCacheWire::LiveSubject::Fleet;
        parts.dashboardToken = "dash-secret";
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        REQUIRE(stream.Requests().size() == 1);
        CHECK(stream.Requests().front().subject == CompileCacheWire::LiveSubject::Fleet);
        CHECK(stream.Requests().front().dashboardToken == "dash-secret");
        auto const refused = NextDue(rig, source);
        CHECK(FailedWith(refused, Outcome::Refused));
        // Presented and refused: telling the operator to present it would send them to do what they did.
        CHECK(NoteOf(refused).ends_with("; the dashboard credential from --dashboard-token-file was not accepted"));
        CHECK_FALSE(NoteOf(refused).contains(" --token-file"));
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        CHECK(FinishedItself(NextDue(rig, source)));

        CloseAndDrain(rig, source);
    }
}

TEST_CASE("a refusal after a reading is a gap and a retry and does not end the session", "[cli][live][source][refusal]")
{
    // WHAT DISTINGUISHES: the same refusal as the case above, after one reading. A node that revoked a watcher
    // may admit it again (§9.17), so a session that showed data keeps its panel: the refusal is still
    // `Refused` in the history, but no `Detached` follows and the retry is armed and taken.
    auto const second = ReadingOpened(2);
    Rig rig;
    ScriptedSubscription stream {
        { Stream({ GrantFrame(CompileCacheWire::LiveSubject::Cache),
                   CacheReadingFrame(ReadingOpened(1), 1),
                   RefusalFrame(CompileCacheWire::ErrorCode::Unauthenticated, "the credential was rotated") }),
          CacheStream({ second }) }
    };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    auto const refused = NextDue(rig, source);
    CHECK(FailedWith(refused, Outcome::Refused));
    CHECK(NoteOf(refused).contains("--token-file"));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(rig.reactor.pendingTimers() == 1);
    CHECK(stream.Opens() == 1);

    rig.clock.advance(Interval);
    rig.Settle();
    CHECK(stream.Opens() == 2);
    auto const again = NextDue(rig, source);
    REQUIRE(KindOf(again) == DashboardEventKind::Sample);
    CHECK(Unwrap(again).reading == std::optional<StatsReading> { second });

    CloseAndDrain(rig, source);
}

TEST_CASE("a NotLeader naming nobody and a full node are gaps and retries even before any reading",
          "[cli][live][source][refusal]")
{
    // WHAT DISTINGUISHES: nothing has been read, which is exactly when a credential refusal ends the session.
    // These two are refusals a later attempt can outlive -- an election in progress, a watcher leaving -- so
    // each is `Unreachable`, not `Refused`, no `Detached` follows, and the retry dials `--addr`.
    auto const check = [](CompileCacheWire::ErrorCode code, std::string const& detail) {
        Rig rig;
        ScriptedSubscription stream { { Stream({ RefusalFrame(code, detail) }), CacheStream({ ReadingOpened(1) }) } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        auto const gap = NextDue(rig, source);
        CHECK(FailedWith(gap, Outcome::Unreachable));
        CHECK(NoteOf(gap).contains(detail));
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        CHECK(rig.reactor.pendingTimers() == 1);
        CHECK(stream.Opens() == 1);

        rig.clock.advance(Interval);
        rig.Settle();
        REQUIRE(stream.Opens() == 2);
        CHECK(DialledTexts(stream).back() == RigAddress());
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);

        CloseAndDrain(rig, source);
    };

    SECTION("no leader is known yet")
    {
        check(CompileCacheWire::ErrorCode::NotLeader, "no leader is known yet");
    }

    SECTION("the node streams to as many watchers as it will")
    {
        check(CompileCacheWire::ErrorCode::EndpointBusy, "this process already streams to 64 watchers");
    }
}

TEST_CASE("a node this client cannot stream with ends the session Protocol even after a reading",
          "[cli][live][source][refusal]")
{
    // WHAT DISTINGUISHES: a reading has been shown, which is when a refusal about the CALLER becomes a gap and a
    // retry. A version or an unimplemented-verb refusal is about the two builds instead: every retry would get it
    // again, so the history says `Protocol` naming the upgrade, `Detached` follows, and no retry is armed.
    auto const check = [](CompileCacheWire::ErrorCode code, std::string_view remedy) {
        Rig rig;
        ScriptedSubscription stream { { Stream({ GrantFrame(CompileCacheWire::LiveSubject::Cache),
                                                 CacheReadingFrame(ReadingOpened(1), 1),
                                                 RefusalFrame(code, "no") }) } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        LiveEventSource source { std::move(parts) };
        rig.Settle();

        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        auto const refused = NextDue(rig, source);
        CHECK(FailedWith(refused, Outcome::Protocol));
        CHECK(NoteOf(refused).contains(remedy));
        CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
        CHECK(FinishedItself(NextDue(rig, source)));
        CHECK(rig.reactor.pendingTimers() == 0);
        CHECK(stream.Opens() == 1);

        CloseAndDrain(rig, source);
    };

    SECTION("another wire version")
    {
        check(CompileCacheWire::ErrorCode::UnsupportedVersion, "upgrade them together");
    }

    SECTION("a node older than subscriptions")
    {
        check(CompileCacheWire::UnimplementedVerb, "upgrade it");
    }
}

TEST_CASE("a node that does not count this machine a member names the flag that admits it", "[cli][live][source][refusal]")
{
    // WHAT DISTINGUISHES: `NotAMember` is about the caller like `Unauthenticated`, so it ends a session that has
    // read nothing -- but its remedy is on the NODE, and a note naming a credential flag would send the operator
    // to the wrong machine.
    Rig rig;
    ScriptedSubscription stream { { Stream(
        { RefusalFrame(CompileCacheWire::ErrorCode::NotAMember, "10.0.0.7 is not a member of this fleet") }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    auto const refused = NextDue(rig, source);
    CHECK(FailedWith(refused, Outcome::Refused));
    CHECK(NoteOf(refused).contains("--fleet-member"));
    CHECK_FALSE(NoteOf(refused).contains("token-file"));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(FinishedItself(NextDue(rig, source)));

    CloseAndDrain(rig, source);
}

TEST_CASE("the dashboard credential is presented on a fleet subscription and on no other",
          "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: the same parts carry a credential for both subjects. A node checks it for `fleet` alone,
    // so a cache or node request that carried it would hand the secret to whatever answers `--addr` for nothing.
    auto const requested = [](CompileCacheWire::LiveSubject subject) {
        Rig rig;
        ScriptedSubscription stream { { Stream({}) } };
        auto parts = rig.Parts();
        parts.subscription = &stream;
        parts.subject = subject;
        parts.dashboardToken = "dash-secret";
        LiveEventSource source { std::move(parts) };
        Pump(rig, 1);
        REQUIRE(stream.Requests().size() == 1);
        auto const token = stream.Requests().front().dashboardToken;
        CloseAndDrain(rig, source);
        return token;
    };

    CHECK(requested(CompileCacheWire::LiveSubject::Fleet) == "dash-secret");
    CHECK(requested(CompileCacheWire::LiveSubject::Cache).empty());
    CHECK(requested(CompileCacheWire::LiveSubject::Node).empty());
}

TEST_CASE("a frame this client cannot read ends the session Protocol", "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: a grant in another build's layout. Retrying cannot succeed -- the node will grant the
    // same layout every time -- so the history says `Protocol` naming the layout, `Detached` follows, and no
    // retry is armed. A source that treated it like a lost stream would redial every interval forever.
    Rig rig;
    ScriptedSubscription stream { { Stream({ PushFrame(CompileCacheWire::EncodeLiveSubscribed(
        CompileCacheWire::LiveSubscribedFields { .subject = CompileCacheWire::LiveSubject::Cache,
                                                 .grantedCadenceMillis = 2000,
                                                 .statsLayout = StatsReadingLayout ^ std::uint64_t { 1 },
                                                 .endpoint = "rig.test:6674" })) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    auto const unreadable = NextDue(rig, source);
    CHECK(FailedWith(unreadable, Outcome::Protocol));
    CHECK(NoteOf(unreadable).starts_with(std::format("{}: ", RigAddress())));
    CHECK(NoteOf(unreadable).contains("layout"));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(FinishedItself(NextDue(rig, source)));
    CHECK(rig.reactor.pendingTimers() == 0);
    CHECK(stream.Opens() == 1);

    CloseAndDrain(rig, source);
}

TEST_CASE("a node reading carries the node's status from the frame it arrived in", "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: two frames, two statuses. A source that kept the first status, or read it from
    // anywhere but the frame, draws a status block of the past on the second sample.
    auto const first = ReadingOpened(1);
    auto const second = ReadingOpened(2);
    Rig rig;
    ScriptedSubscription stream { { Stream({ GrantFrame(CompileCacheWire::LiveSubject::Node),
                                             NodeReadingFrame(first, NodeStatusNamed("status-1"), 1),
                                             NodeReadingFrame(second, NodeStatusNamed("status-2"), 2) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    parts.subject = CompileCacheWire::LiveSubject::Node;
    LiveEventSource source { std::move(parts) };
    Pump(rig, 4);

    REQUIRE(stream.Requests().size() == 1);
    CHECK(stream.Requests().front().subject == CompileCacheWire::LiveSubject::Node);
    auto const one = NextDue(rig, source);
    REQUIRE(KindOf(one) == DashboardEventKind::Sample);
    CHECK(Unwrap(one).reading == std::optional<StatsReading> { first });
    REQUIRE(Unwrap(one).nodeStatus.has_value());
    CHECK(Unwrap(Unwrap(one).nodeStatus).version == "status-1");
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    auto const two = NextDue(rig, source);
    REQUIRE(KindOf(two) == DashboardEventKind::Sample);
    CHECK(Unwrap(two).reading == std::optional<StatsReading> { second });
    REQUIRE(Unwrap(two).nodeStatus.has_value());
    CHECK(Unwrap(Unwrap(two).nodeStatus).version == "status-2");

    CloseAndDrain(rig, source);
}

TEST_CASE("a fleet reading carries the leader's document whole and no stats reading", "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: the document is handed over byte for byte, as the reader parses it; a source that
    // left it behind -- or delivered it as a stats reading -- gives the fleet reader nothing to read.
    auto const text = std::string { "# kpi\nkpi\tvalue\tunit\tof\nmachines\t12\t\t\n" };
    Rig rig;
    ScriptedSubscription stream { { Stream(
        { GrantFrame(CompileCacheWire::LiveSubject::Fleet), FleetDocumentFrame(text, 1) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    parts.subject = CompileCacheWire::LiveSubject::Fleet;
    LiveEventSource source { std::move(parts) };
    Pump(rig, 3);

    auto const sample = NextDue(rig, source);
    REQUIRE(KindOf(sample) == DashboardEventKind::Sample);
    CHECK(Unwrap(sample).document == std::optional<std::string> { text });
    CHECK_FALSE(Unwrap(sample).reading.has_value());
    CHECK(Unwrap(sample).where == "127.0.0.1:6674");
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    CloseAndDrain(rig, source);
}

TEST_CASE("events and gaps on a stream are not samples: only its readings are", "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: an event and a gap arrive before the reading. A source that turned either into a
    // sample -- or owed a frame for it -- queues something the waiting read below receives; this one queues
    // one `Sample` and one `Tick` for the one reading, and then nothing while the next read is out.
    Rig rig;
    ScriptedSubscription stream { { Stream({
        GrantFrame(CompileCacheWire::LiveSubject::Cache),
        PushFrame(CompileCacheWire::EncodeLiveEvent(CompileCacheWire::LiveEventFields {
            .kind = CompileCacheWire::LiveEventKind::MemberJoined, .detail = "build-09" })),
        PushFrame(CompileCacheWire::EncodeLiveGap(
            CompileCacheWire::LiveGapFields { .dropped = 3, .firstTick = 1, .lastTick = 3 })),
        CacheReadingFrame(ReadingOpened(1), 4),
    }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };
    Pump(rig, 5);
    REQUIRE(stream.Reads() == 4);

    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    auto after = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &after);
    rig.reactor.submit(task.handle());
    rig.reactor.drain();
    CHECK_FALSE(after.has_value());

    CloseAndDrain(rig, source);
}

TEST_CASE("a resize is followed by a tick, so the frame is redrawn at the new size", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    Pump(rig, 3);
    (void) NextDue(rig, source);
    (void) NextDue(rig, source);

    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40 });
    auto const resize = NextDue(rig, source);
    CHECK(KindOf(resize) == DashboardEventKind::Resize);
    CHECK((resize.has_value() && resize->columns == 120 && resize->rows == 40));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    // A key changes nothing a frame is drawn from, so it earns no tick.
    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" });
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Key);
    auto after = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &after);
    rig.reactor.submit(task.handle());
    rig.reactor.drain();
    CHECK_FALSE(after.has_value());

    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal that goes away ends the session", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.submit(run.handle());
    rig.Settle();
    CHECK_FALSE(exit.has_value());

    rig.terminal->Close();
    rig.Settle();
    CHECK(StopOf(exit) == DashboardStop::SourceDetached);

    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal's presenter goes before its events, and a frame after the terminal went away is dropped",
          "[cli][live][source]")
{
    // A sample and its tick can be queued ahead of the terminal's `Detached`: the loop draws them
    // after the events are gone and the operator's own screen is back. That frame must not land.
    Rig rig;
    auto presented = PresenterRecord { .events = &rig.terminalRelease };
    auto parts = rig.SpokenParts();
    parts.frames = std::make_unique<PresenterRecord::Sink>(&presented);
    LiveEventSource source { std::move(parts) };
    auto* const frames = source.Frames();
    REQUIRE(frames != nullptr);
    rig.Settle();

    frames->Present("while the terminal is there");
    CHECK(presented.frames == 1);

    rig.terminal->GoAway();
    rig.Settle();
    REQUIRE(rig.terminalRelease.released);
    CHECK(presented.released);
    CHECK_FALSE(presented.afterEvents);

    frames->Present("after it went away");
    CHECK(presented.frames == 1);
    CHECK(presented.last == "while the terminal is there");

    CloseAndDrain(rig, source);
}

TEST_CASE("a frame's images reach the terminal's presenter through the source, and none after the terminal went away",
          "[cli][live][source]")
{
    // The loop presents through `Frames()`, never through the presenter itself, so an image placed in
    // a frame is drawn only if the source hands the presenter the FRAME. Handing it the rows alone
    // still draws every row, which is why the placement is what this case reads.
    Rig rig;
    auto presented = PresenterRecord { .events = &rig.terminalRelease };
    auto parts = rig.SpokenParts();
    parts.frames = std::make_unique<PresenterRecord::Sink>(&presented);
    LiveEventSource source { std::move(parts) };
    auto* const frames = source.Frames();
    REQUIRE(frames != nullptr);
    rig.Settle();

    auto const placed = DashboardFrame {
        .text = "rows",
        .placements = { FramePlacement { .row = 4, .column = 3, .cellsWide = 8, .cellsHigh = 2, .sixel = "#0!8~" } },
    };
    frames->PresentPlaced(placed);
    CHECK(presented.frames == 1);
    CHECK(presented.last == "rows");
    REQUIRE(presented.placements.size() == 1);
    CHECK(presented.placements.front().row == 4);
    CHECK(presented.placements.front().column == 3);
    CHECK(presented.placements.front().sixel == "#0!8~");

    rig.terminal->GoAway();
    rig.Settle();
    REQUIRE(presented.released);

    frames->PresentPlaced(DashboardFrame { .text = "after it went away", .placements = {} });
    CHECK(presented.frames == 1);
    CHECK(presented.last == "rows");

    CloseAndDrain(rig, source);
}

TEST_CASE("a source with no presenter hands the loop none", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    CHECK(source.Frames() == nullptr);
    CloseAndDrain(rig, source);
}

TEST_CASE("a run with no terminal takes its whole sample budget from one stream", "[cli][live][source][subscription]")
{
    // WHAT DISTINGUISHES: four readings on one stream and a budget of three. The run ends on its budget with
    // three readings taken, over ONE dial: a source that re-subscribed per sample -- the polling shape this
    // replaced -- dials three times.
    Rig rig;
    ScriptedSubscription stream { { CacheStream(
        { ReadingOpened(1), ReadingOpened(2), ReadingOpened(3), ReadingOpened(4) }) } };
    auto parts = rig.Parts();
    parts.subscription = &stream;
    LiveEventSource source { std::move(parts) };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits { .samples = 3 }, &exit);
    rig.reactor.submit(run.handle());
    rig.Settle();

    REQUIRE(exit.has_value());
    CHECK(StopOf(exit) == DashboardStop::SampleBudget);
    CHECK(Unwrap(exit).model.samples == 3);
    CHECK(stream.Opens() == 1);

    CloseAndDrain(rig, source);
}

TEST_CASE("a stop request after a sample ends a session with no terminal as answered", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.submit(run.handle());
    rig.Settle();
    CHECK(rig.subscription.Opens() == 1);
    CHECK_FALSE(exit.has_value());

    rig.stop->Fire();
    rig.reactor.drain();
    // Released when its watch ended, which is what restores the disposition -- not when the
    // source goes.
    CHECK(rig.stopReleased);

    // Quit, and a session that had a reading to show: the operator ended a run that
    // worked, which is exit 0 rather than a failure they have to explain to a script.
    CHECK(StopOf(exit) == DashboardStop::Quit);
    CHECK((exit.has_value() && ExitCodeOf(exit->outcome) == 0));

    CloseAndDrain(rig, source);
}

TEST_CASE("a stop request arrives as StopRequested and never as a keystroke", "[cli][live][source]")
{
    // The loop quits on both, so a case driving the loop cannot tell them apart: this reads the
    // event itself. A `Key` would carry bytes nobody typed.
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };
    Pump(rig, 3);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    rig.stop->Fire();
    rig.reactor.drain();
    auto const stop = NextDue(rig, source);
    CHECK(KindOf(stop) == DashboardEventKind::StopRequested);
    CHECK((stop.has_value() && stop->keys.empty()));

    CloseAndDrain(rig, source);
}

TEST_CASE("a stop request before any sample does not end a session as answered", "[cli][live][source]")
{
    // The control for the case above: a stop is not an answer by itself. Quit before a
    // reading arrived is a session that showed nothing, and exit 0 would claim otherwise.
    Rig rig;
    auto parts = rig.StoppableParts();
    rig.stop->Fire();
    LiveEventSource source { std::move(parts) };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.submit(run.handle());
    rig.reactor.drain();

    CHECK(StopOf(exit) == DashboardStop::Quit);
    CHECK((exit.has_value() && ExitCodeOf(exit->outcome) != 0));
    CHECK(rig.subscription.Opens() == 0);

    CloseAndDrain(rig, source);
}

TEST_CASE("a session with a stop signal keeps reading until a stop arrives and drains once closed", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.submit(run.handle());
    rig.Settle();
    rig.clock.advance(Interval);
    rig.Settle();

    // Nothing fired: the watch is waiting, not ending the session and not asking twice, while the
    // stream is subscribed again after it went silent.
    CHECK_FALSE(exit.has_value());
    CHECK(rig.subscription.Opens() == 2);
    CHECK(rig.stop->Waits() == 1);
    CHECK_FALSE(rig.stopReleased);

    // And a close answers that wait, so the drain does not hang on a thread nobody woke, and
    // the signal is released for it.
    CloseAndDrain(rig, source);
    CHECK(rig.stopReleased);
    CHECK(StopOf(exit) == DashboardStop::SourceDetached);
}

TEST_CASE("a stop watch that fails ends the session saying why", "[cli][live][source]")
{
    // A session nothing could interrupt with Ctrl-C is not one to keep running.
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };
    Pump(rig, 3);
    (void) NextDue(rig, source);
    (void) NextDue(rig, source);

    rig.stop->Fail();
    auto const ended = NextDue(rig, source);
    CHECK(KindOf(ended) == DashboardEventKind::Detached);
    CHECK((ended.has_value() && ended->note.contains("Ctrl-C")));

    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal is released once nothing reads it and not when a stuck read returns", "[cli][live][source]")
{
    // Releasing the terminal is what restores it. A read that never comes back must not leave an
    // operator's terminal in raw mode behind a process that is about to give up on that read.
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    rig.reactor.drain();
    CHECK(rig.pool.pendingSubmissions() == 1);

    rig.clock.advance(5ms);
    source.Close();
    rig.reactor.drain();
    CHECK(rig.terminalRelease.released);
    CHECK(rig.terminalRelease.closedFirst);
    CHECK(rig.subscription.Leaves() == 1);
    // The dial is still out, and its start -- the epoch, before the clock moved -- is readable from anywhere.
    CHECK(source.ReadOutstandingSince()
          == std::optional<core::platform::SteadyTimePoint> { core::platform::SteadyTimePoint {} });

    rig.Settle();
    CHECK_FALSE(source.ReadOutstandingSince().has_value());
    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal that goes away on its own is closed before it is released", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    rig.Settle();

    // Detached with nobody having closed anything: the forwarder ends and releases the
    // terminal, which its contract says must be closed first -- and nobody else closed it.
    rig.terminal->GoAway();
    rig.reactor.drain();
    CHECK(rig.terminalRelease.released);
    CHECK(rig.terminalRelease.closedFirst);

    CloseAndDrain(rig, source);
}
