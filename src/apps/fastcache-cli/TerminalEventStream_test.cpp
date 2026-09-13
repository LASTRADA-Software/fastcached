// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "TerminalEventStream.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <tests/Unwrap.hpp>
#include <tui/InputEvent.hpp>
#include <tui/runtime/testing/MockEventSource.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::Unwrap;

namespace
{

/// An endo event source whose wait BLOCKS until it is woken, and records where it ran.
///
/// It blocks on purpose, for `TakeSample`'s reason: a source that returned at once would let a
/// stream that never left the reactor, or a `Close()` that never reached the wait, finish before
/// anything could observe the difference.
class BlockingEventSource final: public tui::runtime::EventSource
{
  public:
    [[nodiscard]] tui::runtime::WaitOutcome wait(int /*timeoutMs*/) override
    {
        auto lock = std::unique_lock { _mutex };
        _waitedOn = std::this_thread::get_id();
        _entered.store(true, std::memory_order_release);
        _woken.wait(lock, [this] { return _wake; });
        _wake = false;
        auto outcome = tui::runtime::WaitOutcome {};
        outcome.agentReady = true;
        return outcome;
    }

    [[nodiscard]] tui::runtime::FdToken attach(endo::platform::NativeHandle /*fd*/,
                                               tui::runtime::FdInterest /*interest*/) override
    {
        return {};
    }

    void detach(tui::runtime::FdToken /*token*/) override {}

    /// Release the parked wait.
    void Wake()
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _wake = true;
        }
        _woken.notify_all();
    }

    /// @return Whether a wait has parked.
    [[nodiscard]] bool Entered() const noexcept
    {
        return _entered.load(std::memory_order_acquire);
    }

    /// @return The thread the last wait ran on. Read only once the stream has resumed.
    [[nodiscard]] std::thread::id WaitedOn() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _waitedOn;
    }

  private:
    std::atomic<bool> _entered { false };
    std::thread::id _waitedOn {};
    mutable std::mutex _mutex;
    std::condition_variable _woken;
    bool _wake { false };
};

/// Wakes @p source when a case ends, however it ends.
///
/// Declared AFTER the stream, so it runs first on the way out. A case that fails while the pool
/// thread is parked in the wait would otherwise destroy the source under that thread and then join
/// the pool forever -- a red turned into a hang that names nothing.
struct WakeOnExit
{
    explicit WakeOnExit(BlockingEventSource& wakes):
        source { wakes }
    {
    }

    BlockingEventSource& source;

    WakeOnExit(WakeOnExit const&) = delete;
    WakeOnExit& operator=(WakeOnExit const&) = delete;
    WakeOnExit(WakeOnExit&&) = delete;
    WakeOnExit& operator=(WakeOnExit&&) = delete;
    ~WakeOnExit()
    {
        source.Wake();
    }
};

/// Drain @p reactor until @p done, failing by name if it never happens.
///
/// Bounded on the MONOTONIC clock, so a stream that never resumes -- a `Close()` that did not
/// reach the parked wait -- is a red naming what it waited for rather than a ctest timeout
/// naming nothing.
/// @param reactor The reactor resumptions come back through.
/// @param done The condition waited for.
/// @param what What it means if it never becomes true.
template <typename Condition>
void DrainUntil(TestReactor& reactor, Condition done, char const* what)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            FAIL(what);
        reactor.Drain();
        std::this_thread::yield();
    }
}

/// Take one event, writing it and the thread it was delivered on where the caller can read them.
///
/// Pointers and a named coroutine, for the reason the production signatures use them.
/// @param source The stream.
/// @param out Where to put the event.
/// @param deliveredOn Where to record the thread the event was delivered on.
/// @return The task to submit.
[[nodiscard]] Task<void> NextInto(IDashboardEventSource* source,
                                  std::optional<DashboardEvent>* out,
                                  std::thread::id* deliveredOn)
{
    auto event = co_await source->Next();
    *deliveredOn = std::this_thread::get_id();
    *out = std::move(event);
}

/// Drive one `Next()` to completion on @p reactor.
/// @return The event.
[[nodiscard]] DashboardEvent TakeNext(IDashboardEventSource& source, TestReactor& reactor)
{
    auto result = std::optional<DashboardEvent> {};
    auto deliveredOn = std::thread::id {};
    auto task = NextInto(&source, &result, &deliveredOn);
    reactor.Submit(task.Native());
    DrainUntil(reactor, [&result] { return result.has_value(); }, "a Next() never resumed");
    return Unwrap(result);
}

/// A key event carrying @p codepoint.
[[nodiscard]] tui::KeyEvent Key(char32_t codepoint, tui::Modifier modifiers = tui::Modifier::None)
{
    return tui::KeyEvent { .key = static_cast<tui::KeyCode>(codepoint), .modifiers = modifiers, .codepoint = codepoint };
}

/// What a `FakeDevice` was asked, readable after the device itself is gone.
///
/// A start that fails destroys its device, so what the case asserts on cannot live inside it.
struct DeviceRecord
{
    std::atomic<int> restores { 0 };
    std::atomic<int> restoredNow { 0 };
    std::thread::id acquiredOn {};
    std::thread::id askedOn {};
};

/// How a `FakeDevice` answers each step of a start.
struct DeviceScript
{
    bool refuseAcquire { false };
    bool throwOnAsk { false };
    SixelAnswer sixel { SixelAnswer::Advertised };
    TerminalTextEncoding encoding { TerminalTextEncoding::Utf8 };

    /// Where the device's events come from when set; a scripted source that never blocks when not.
    BlockingEventSource* blocking { nullptr };
};

/// A terminal device that records where each step ran and can fail at each one.
///
/// Its failures are the ones a real terminal cannot be made to produce on demand: refusing the
/// acquisition, and throwing AFTER it, while the terminal would already be in raw mode.
class FakeDevice final: public ITerminalDevice
{
  public:
    FakeDevice(DeviceScript script, DeviceRecord* record):
        _script { script },
        _record { record }
    {
    }

    [[nodiscard]] std::expected<void, std::string> Acquire() override
    {
        _record->acquiredOn = std::this_thread::get_id();
        if (_script.refuseAcquire)
            return std::unexpected(std::string { "no terminal here" });
        return {};
    }

    [[nodiscard]] SixelAnswer AskSixel() override
    {
        _record->askedOn = std::this_thread::get_id();
        if (_script.throwOnAsk)
            throw std::runtime_error("the reply could not be read");
        return _script.sixel;
    }

    [[nodiscard]] TerminalTextEncoding Encoding() override
    {
        return _script.encoding;
    }

    [[nodiscard]] int Columns() const override
    {
        return 132;
    }

    [[nodiscard]] int Rows() const override
    {
        return 43;
    }

    [[nodiscard]] tui::runtime::EventSource& Events() override
    {
        if (_script.blocking != nullptr)
            return *_script.blocking;
        return _source;
    }

    void Wake() override
    {
        if (_script.blocking != nullptr)
            _script.blocking->Wake();
    }

    void Restore() noexcept override
    {
        _record->restores.fetch_add(1, std::memory_order_acq_rel);
    }

    void RestoreNow() noexcept override
    {
        _record->restoredNow.fetch_add(1, std::memory_order_acq_rel);
    }

  private:
    DeviceScript _script;
    DeviceRecord* _record;
    tui::runtime::testing::MockEventSource _source;
};

/// A start as the awaiting coroutine saw it.
struct StartOutcome
{
    bool delivered { false };
    std::expected<StartedTerminal, std::string> started { std::unexpected(std::string { "never delivered" }) };
    std::thread::id deliveredOn {};

    /// How many restores had happened when the awaiting coroutine resumed: "restored before the
    /// result was handed back" is this being 1, where a restore left to a later destructor reads 0.
    int restoresWhenDelivered { -1 };
};

/// Await a start of @p device, recording what the awaiter saw on resumption.
[[nodiscard]] Task<void> StartInto(
    std::unique_ptr<ITerminalDevice> device, IExecutor* pool, IExecutor* resumeOn, DeviceRecord* record, StartOutcome* out)
{
    auto started = co_await StartTerminalDevice(std::move(device), pool, resumeOn);
    out->deliveredOn = std::this_thread::get_id();
    out->restoresWhenDelivered = record->restores.load(std::memory_order_acquire);
    out->started = std::move(started);
    out->delivered = true;
}

/// Start a `FakeDevice` following @p script, driving @p reactor until the start is delivered.
[[nodiscard]] StartOutcome StartFake(DeviceScript script, DeviceRecord& record, IExecutor& pool, TestReactor& reactor)
{
    auto outcome = StartOutcome {};
    auto task = StartInto(std::make_unique<FakeDevice>(script, &record), &pool, &reactor, &record, &outcome);
    reactor.Submit(task.Native());
    DrainUntil(reactor, [&outcome] { return outcome.delivered; }, "a terminal start was never delivered");
    return outcome;
}

} // namespace

TEST_CASE("the first terminal event is the geometry at open, before any wait", "[cli][dashboard][terminal]")
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    auto stream = MakeTerminalEventStream(
        { .source = &source, .pool = &pool, .resumeOn = &reactor, .wake = {}, .columns = 120, .rows = 40 });

    auto const first = TakeNext(*stream, reactor);

    CHECK(first.kind == DashboardEventKind::Resize);
    CHECK(first.columns == 120);
    CHECK(first.rows == 40);
    CHECK(source.waitCount() == 0);
}

TEST_CASE("keys and resizes from one wait arrive in order, and other input is not dashboard input",
          "[cli][dashboard][terminal]")
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    source.pushEvents({ Key(U'q'), tui::MouseEvent {}, tui::ResizeEvent { .columns = 80, .rows = 24 } });
    auto stream = MakeTerminalEventStream(
        { .source = &source, .pool = &pool, .resumeOn = &reactor, .wake = {}, .columns = 100, .rows = 30 });

    std::ignore = TakeNext(*stream, reactor);
    auto const key = TakeNext(*stream, reactor);
    auto const resize = TakeNext(*stream, reactor);

    CHECK(key.kind == DashboardEventKind::Key);
    CHECK(key.keys == "q");
    CHECK(resize.kind == DashboardEventKind::Resize);
    CHECK(resize.columns == 80);
    CHECK(resize.rows == 24);
    CHECK(source.waitCount() == 1);
}

TEST_CASE("the terminal wait runs on the pool and its event is delivered back on the reactor", "[cli][dashboard][terminal]")
{
    // WHAT DISTINGUISHES: the thread identities. An event arriving proves nothing, since a
    // stream that waited inline on the reactor would deliver the same one.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = BlockingEventSource {};
    auto stream = MakeTerminalEventStream({ .source = &source,
                                            .pool = &pool,
                                            .resumeOn = &reactor,
                                            .wake = [&source] { source.Wake(); },
                                            .columns = 80,
                                            .rows = 24 });
    auto const wakeOnExit = WakeOnExit { source };
    std::ignore = TakeNext(*stream, reactor);

    auto const driverThread = std::this_thread::get_id();
    auto result = std::optional<DashboardEvent> {};
    auto deliveredOn = std::thread::id {};
    auto task = NextInto(stream.get(), &result, &deliveredOn);
    reactor.Submit(task.Native());
    reactor.Drain();

    // Parked in the wait on the pool, and the reactor has run out of work while it is: the loop
    // is FREE while the terminal is quiet.
    DrainUntil(reactor, [&source] { return source.Entered(); }, "the stream never parked in the terminal wait");
    CHECK(!result.has_value());

    stream->Close();
    DrainUntil(
        reactor,
        [&result] { return result.has_value(); },
        "the outstanding Next() never resumed after Close(): the close did not reach the parked wait");

    CHECK(source.WaitedOn() != driverThread);
    CHECK(deliveredOn == driverThread);
    CHECK(Unwrap(result).kind == DashboardEventKind::Detached);
}

TEST_CASE("closing the terminal stream wakes a parked wait, and it stays closed", "[cli][dashboard][terminal]")
{
    // The positive control is `Entered()`: the case only means something once the wait is seen
    // PARKED before the close, or a close that never reached the wait would pass as well.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = BlockingEventSource {};
    auto stream = MakeTerminalEventStream({ .source = &source,
                                            .pool = &pool,
                                            .resumeOn = &reactor,
                                            .wake = [&source] { source.Wake(); },
                                            .columns = 80,
                                            .rows = 24 });
    auto const wakeOnExit = WakeOnExit { source };
    std::ignore = TakeNext(*stream, reactor);

    auto result = std::optional<DashboardEvent> {};
    auto deliveredOn = std::thread::id {};
    auto task = NextInto(stream.get(), &result, &deliveredOn);
    reactor.Submit(task.Native());
    reactor.Drain();
    DrainUntil(reactor, [&source] { return source.Entered(); }, "the stream never parked in the terminal wait");

    stream->Close();
    stream->Close();
    DrainUntil(
        reactor,
        [&result] { return result.has_value(); },
        "the outstanding Next() never resumed after Close(): the close did not reach the parked wait");

    CHECK(Unwrap(result).kind == DashboardEventKind::Detached);
    CHECK(Unwrap(result).note == "the dashboard closed its terminal");
    CHECK(TakeNext(*stream, reactor).kind == DashboardEventKind::Detached);
}

TEST_CASE("terminal input that closes ends the stream, and it stays ended", "[cli][dashboard][terminal]")
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    source.pushEvents({ Key(U'a') });
    source.pushInterrupt();
    auto stream = MakeTerminalEventStream(
        { .source = &source, .pool = &pool, .resumeOn = &reactor, .wake = {}, .columns = 80, .rows = 24 });

    std::ignore = TakeNext(*stream, reactor);
    CHECK(TakeNext(*stream, reactor).keys == "a");
    auto const ended = TakeNext(*stream, reactor);
    auto const again = TakeNext(*stream, reactor);

    CHECK(ended.kind == DashboardEventKind::Detached);
    CHECK(ended.note == "the terminal's input closed");
    CHECK(again.kind == DashboardEventKind::Detached);
    CHECK(source.waitCount() == 2);
}

TEST_CASE("the wait after terminal input is short, so a lone ESC is settled rather than held", "[cli][dashboard][terminal]")
{
    // endo decides a lone ESC is the Escape key only when a wait TIMES OUT. An unbounded wait
    // never does, so a stream that always waited with -1 would hold an ESC until the next key.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    source.pushEvents({ Key(U'a') });
    source.pushTimeout();
    source.pushEvents({ Key(U'b') });
    auto stream = MakeTerminalEventStream(
        { .source = &source, .pool = &pool, .resumeOn = &reactor, .wake = {}, .columns = 80, .rows = 24 });

    std::ignore = TakeNext(*stream, reactor); // geometry
    std::ignore = TakeNext(*stream, reactor); // a, from an unbounded wait
    std::ignore = TakeNext(*stream, reactor); // b, after a short wait that timed out and an unbounded one

    CHECK(source.recordedTimeouts() == std::vector<int> { -1, 50, -1 });
}

TEST_CASE("the keys endo decodes spell the bytes the dashboard quits on", "[cli][dashboard][terminal]")
{
    // Asserted through `IsQuitKey`, the consumer, rather than against byte literals alone: the
    // property is that the keys an operator presses to leave still leave once endo has decoded
    // them, and a spelling that matched a literal while the loop compared something else would
    // pass a literal-only case.
    CHECK(IsQuitKey(KeyBytes(Key(U'q'))));
    CHECK(IsQuitKey(KeyBytes(Key(U'Q'))));
    CHECK(IsQuitKey(KeyBytes(Key(U'c', tui::Modifier::Ctrl))));
    CHECK(IsQuitKey(KeyBytes(tui::KeyEvent { .key = tui::KeyCode::Escape })));
    CHECK_FALSE(IsQuitKey(KeyBytes(Key(U'c'))));
}

TEST_CASE("a decoded key is spelled as its bytes, or not at all", "[cli][dashboard][terminal]")
{
    CHECK(KeyBytes(Key(U'x', tui::Modifier::Alt)) == "\x1bx");
    CHECK(KeyBytes(tui::KeyEvent { .key = tui::KeyCode::Up }) == "\x1b[A");
    CHECK(KeyBytes(Key(U'é')) == "\xc3\xa9");
    CHECK(KeyBytes(Key(U'\U0001F600')) == "\xf0\x9f\x98\x80");
    CHECK(KeyBytes(tui::KeyEvent { .key = tui::KeyCode::F5 }).empty());
}

TEST_CASE("a DA1 answer, and each way of having none, keeps its meaning as a Sixel answer", "[cli][dashboard][terminal]")
{
    using Attributes = std::expected<tui::DeviceAttributesReport, tui::QueryUnanswered>;

    CHECK(ToSixelAnswer(Attributes { tui::DeviceAttributesReport { .attributes = { 62, 4, 22 } } })
          == SixelAnswer::Advertised);
    CHECK(ToSixelAnswer(Attributes { tui::DeviceAttributesReport { .attributes = { 62, 22 } } })
          == SixelAnswer::NotAdvertised);
    CHECK(ToSixelAnswer(Attributes { std::unexpected(tui::QueryUnanswered::NoReply) }) == SixelAnswer::NoReply);
    CHECK(ToSixelAnswer(Attributes { std::unexpected(tui::QueryUnanswered::NotAsked) }) == SixelAnswer::NotAsked);
}

TEST_CASE("a terminal is acquired on the pool and its start is delivered back on the reactor", "[cli][dashboard][terminal]")
{
    // WHAT DISTINGUISHES: the thread identities. A start that acquired inline on the reactor, or
    // never hopped back, delivers the same record.
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto const driverThread = std::this_thread::get_id();

    auto outcome = StartFake({}, record, pool, reactor);

    REQUIRE(outcome.started.has_value());
    CHECK(record.acquiredOn != std::thread::id {});
    CHECK(record.acquiredOn != driverThread);
    CHECK(record.askedOn == record.acquiredOn);
    CHECK(outcome.deliveredOn == driverThread);
}

TEST_CASE("a started terminal stays acquired until its events are destroyed, and opens with its geometry",
          "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto outcome = StartFake({}, record, pool, reactor);
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;

    CHECK(started.capabilities.sixel == SixelAnswer::Advertised);
    CHECK(started.capabilities.encoding == TerminalTextEncoding::Utf8);
    auto const first = TakeNext(*started.events, reactor);
    CHECK(first.kind == DashboardEventKind::Resize);
    CHECK(first.columns == 132);
    CHECK(first.rows == 43);
    CHECK(record.restores.load() == 0);

    started.events.reset();
    CHECK(record.restores.load() == 1);
}

TEST_CASE("a terminal that never answers DA1 still starts, and the rung falls back", "[cli][dashboard][terminal]")
{
    // No DA1 reply is an ANSWER about the terminal, not a failure to acquire it: the dashboard
    // draws on the next rung rather than refusing to draw.
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto outcome =
        StartFake({ .sixel = SixelAnswer::NoReply, .encoding = TerminalTextEncoding::Utf8 }, record, pool, reactor);

    REQUIRE(outcome.started.has_value());
    CHECK(outcome.started->capabilities.sixel == SixelAnswer::NoReply);
    CHECK(ChooseRenderRung(outcome.started->capabilities) == RenderRung::Unicode);
    CHECK(record.restores.load() == 0);
}

TEST_CASE("a refused terminal acquisition is restored before the refusal is delivered", "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto const outcome = StartFake({ .refuseAcquire = true }, record, pool, reactor);

    REQUIRE_FALSE(outcome.started.has_value());
    CHECK(outcome.started.error() == "no terminal here");
    CHECK(outcome.restoresWhenDelivered == 1);
    CHECK(record.askedOn == std::thread::id {});
}

TEST_CASE("a terminal start that throws after acquiring is restored and reported as a failed start",
          "[cli][dashboard][terminal]")
{
    // The halfway failure: the terminal is acquired, and the query after it throws. Nothing on
    // this path calls a restore by hand, so only the guard can.
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto const outcome = StartFake({ .throwOnAsk = true }, record, pool, reactor);

    REQUIRE_FALSE(outcome.started.has_value());
    CHECK(outcome.started.error() == "the terminal failed while being started: the reply could not be read");
    CHECK(outcome.restoresWhenDelivered == 1);
    CHECK(record.restores.load() == 1);
}

TEST_CASE("restoring a started terminal now, from another thread while a read is parked, restores it once",
          "[cli][dashboard][terminal]")
{
    // The abandonment path: `main` restores the terminal and ends the process with a terminal
    // read still parked on the pool, from a thread that is neither the pool nor the reactor, and
    // without closing anything first. The events are closed and destroyed here only so the case
    // can go on to show that a later destruction does not restore through this handle again.
    auto source = BlockingEventSource {};
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto outcome = StartFake({ .blocking = &source }, record, pool, reactor);
    auto const wakeOnExit = WakeOnExit { source };
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;
    REQUIRE(started.restore != nullptr);
    std::ignore = TakeNext(*started.events, reactor);

    auto result = std::optional<DashboardEvent> {};
    auto deliveredOn = std::thread::id {};
    auto task = NextInto(started.events.get(), &result, &deliveredOn);
    reactor.Submit(task.Native());
    reactor.Drain();
    DrainUntil(reactor, [&source] { return source.Entered(); }, "the stream never parked in the terminal wait");

    {
        auto restorer = std::thread { [&started] { started.restore->RestoreNow(); } };
        restorer.join();
    }
    CHECK(record.restoredNow.load() == 1);
    CHECK(record.restores.load() == 0);
    CHECK(!result.has_value()); // the parked read was not disturbed

    started.restore->RestoreNow();
    CHECK(record.restoredNow.load() == 1);

    started.events->Close();
    DrainUntil(reactor, [&result] { return result.has_value(); }, "the outstanding Next() never resumed after Close()");
    CHECK(Unwrap(result).kind == DashboardEventKind::Detached);

    started.events.reset();
    CHECK(record.restores.load() == 1);
    CHECK(record.restoredNow.load() == 1);

    // The handle outlives the events it restored for, and a call through it is still safe.
    started.restore->RestoreNow();
    CHECK(record.restoredNow.load() == 1);
}

TEST_CASE("a started terminal whose events were destroyed first is not restored again through its handle",
          "[cli][dashboard][terminal]")
{
    // The other order. Destroying the events restored the terminal and tore the device down, so a
    // restore-now after it must reach nothing -- the device it would reach no longer exists.
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto outcome = StartFake({}, record, pool, reactor);
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;
    REQUIRE(started.restore != nullptr);

    started.events.reset();
    CHECK(record.restores.load() == 1);

    started.restore->RestoreNow();
    started.restore->RestoreNow();
    CHECK(record.restoredNow.load() == 0);
    CHECK(record.restores.load() == 1);
}
