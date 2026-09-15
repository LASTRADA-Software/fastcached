// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "TerminalEventStream.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/BoundedWait.hpp>
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
/// It blocks on purpose, for `TakeFrame`'s reason: a source that returned at once would let a
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
/// Bounded on the MONOTONIC clock, through the tree's one test wait, so a stream that never resumes is a
/// red naming what it waited for rather than a ctest timeout naming nothing. Thirty seconds, the bound
/// these cases were written with.
/// @param reactor The reactor resumptions come back through.
/// @param done The condition waited for.
/// @param what What it means if it never becomes true.
template <std::predicate Condition>
void DrainUntil(TestReactor& reactor, Condition done, char const* what)
{
    // Out of the REQUIRE: its macro spells the expression twice, so a move inside it reads as a use after move.
    auto const reached = FastCache::Testing::DrainUntil(
        reactor,
        what,
        std::move(done),
        [&reactor] { return std::format("{} submission(s) pending on the reactor", reactor.PendingSubmissions()); },
        std::chrono::seconds { 30 });
    REQUIRE(reached);
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
    std::atomic<int> rereads { 0 };
    std::thread::id acquiredOn {};
    std::thread::id askedOn {};

    /// Everything written to the device, `Write` and `RestoreNow`'s leading bytes alike, in order.
    [[nodiscard]] std::string Written() const
    {
        auto const lock = std::scoped_lock { mutex };
        return written;
    }

    /// Append @p bytes to what was written. Callable from any thread.
    void Append(std::string_view bytes)
    {
        auto const lock = std::scoped_lock { mutex };
        written.append(bytes);
    }

    mutable std::mutex mutex;
    std::string written;
};

/// How often @p needle occurs in @p haystack, without overlaps.
[[nodiscard]] std::size_t Occurrences(std::string_view haystack, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto at = haystack.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = haystack.find(needle, at + needle.size());
    }
    return count;
}

/// How a `FakeDevice` answers each step of a start.
struct DeviceScript
{
    bool refuseAcquire { false };
    bool throwOnAsk { false };
    bool throwOnEncoding { false };
    SixelAnswer sixel { SixelAnswer::Advertised };
    SynchronizedOutputAnswer synchronizedOutput { SynchronizedOutputAnswer::Supported };
    TerminalTextEncoding encoding { TerminalTextEncoding::Utf8 };
    ColourAnswer colour { ColourAnswer::Suppressed };

    /// What the acquisition measured, and what each re-read after a resize answers.
    std::optional<CellPixelSize> cellPixels { CellPixelSize { .width = 10, .height = 20 } };
    std::optional<CellPixelSize> reread { CellPixelSize { .width = 10, .height = 20 } };

    /// Where the device's events come from when set; a scripted source that never blocks when not.
    BlockingEventSource* blocking { nullptr };

    /// What the scripted source's one wait delivers, when `blocking` is not set.
    std::vector<tui::InputEvent> input {};
};

/// A terminal device that records where each step ran and can fail at each one.
///
/// Its failures are the ones a real terminal cannot be made to produce on demand: refusing the
/// acquisition, and throwing AFTER it, while the terminal would already be in raw mode.
class FakeDevice final: public ITerminalDevice
{
  public:
    FakeDevice(DeviceScript script, DeviceRecord* record):
        _script { std::move(script) },
        _record { record }
    {
        if (!_script.input.empty())
            _source.pushEvents(_script.input);
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

    [[nodiscard]] SynchronizedOutputAnswer AskSynchronizedOutput() override
    {
        return _script.synchronizedOutput;
    }

    [[nodiscard]] std::optional<CellPixelSize> AskCellPixels() override
    {
        return _script.cellPixels;
    }

    [[nodiscard]] std::optional<CellPixelSize> RereadCellPixels() override
    {
        _record->rereads.fetch_add(1, std::memory_order_acq_rel);
        return _script.reread;
    }

    [[nodiscard]] TerminalTextEncoding Encoding() override
    {
        if (_script.throwOnEncoding)
            throw std::runtime_error("the environment could not be read");
        return _script.encoding;
    }

    [[nodiscard]] ColourAnswer AskColour() override
    {
        return _script.colour;
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

    void Write(std::string_view bytes) noexcept override
    {
        _record->Append(bytes);
    }

    void Restore() noexcept override
    {
        _record->restores.fetch_add(1, std::memory_order_acq_rel);
    }

    void RestoreNow(std::string_view leading) noexcept override
    {
        _record->Append(leading);
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
[[nodiscard]] StartOutcome StartFake(DeviceScript const& script, DeviceRecord& record, IExecutor& pool, TestReactor& reactor)
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

TEST_CASE("presentation bytes are endo's own spellings", "[cli][dashboard][terminal]")
{
    // Captured from endo's TerminalOutput rather than restated, so this asserts the capture caught
    // what each call writes, in the order the steps call them -- a capture that flushed nothing
    // would leave every string empty and every presenter case below comparing empty strings.
    auto const& screen = ScreenBytes();
    CHECK(screen.enter == "\x1b[?1049h\x1b[?25l");
    CHECK(screen.leave == "\x1b[?25h\x1b[?1049l");
    CHECK(screen.syncBegin == "\x1b[?2026h");
    CHECK(screen.syncEnd == "\x1b[?2026l");
    // A frame: the screen erased from its last row down, then each row placed and erased before it
    // is written.
    CHECK(FrameBytes("ab\ncd", false) == "\x1b[2;1H\x1b[J\x1b[1;1H\x1b[Kab\x1b[2;1H\x1b[Kcd");
    CHECK(FrameBytes("", false) == "\x1b[1;1H\x1b[J");
}

namespace
{
/// A terminal screen as the frame presenter cannot assume it: a LINE FEED moves the cursor down
/// and does NOT return it to the first column, and autowrap is off, so a character written in the
/// last column overwrites that column. That is a Windows console with DISABLE_NEWLINE_AUTO_RETURN
/// (endo's raw mode) or a POSIX tty with output post-processing off, and it is where rows joined
/// by line feeds all landed in the last column.
///
/// Reads UTF-8 one code point per cell -- every glyph the panels draw is one cell wide -- and the
/// sequences the presenter writes: `CSI <row>;<col> H`, `CSI K`, `CSI J`, private modes, which
/// change nothing drawn here, and a Sixel image (`DCS ... q <body> ST`). Anything else is recorded,
/// so a presenter writing a sequence this model does not know fails the case rather than drawing
/// nothing silently.
///
/// **An image covers cells, as on a terminal that draws Sixel.** It is drawn from the cursor over as
/// many cells as its raster attributes' pixels fill at this screen's cell size, rounded up. Erasing a
/// cell or writing a character on it removes the image there. That is what makes an image a frame
/// wrote before its rows invisible, and one a later frame did not write gone.
class NoAutoReturnScreen
{
  public:
    /// An image as it was drawn: the cell it started at, 0-based, and its body.
    struct DrawnImage
    {
        std::size_t row { 0 };
        std::size_t column { 0 };
        std::string body;
    };

    NoAutoReturnScreen(std::size_t columns, std::size_t rows, CellPixelSize cell = { .width = 10, .height = 20 }):
        _columns { columns },
        _cell { cell },
        _cells(rows, std::vector<std::string>(columns, " ")),
        _imaged(rows, std::vector<bool>(columns, false))
    {
    }

    /// Interpret @p bytes as the terminal would.
    void Feed(std::string_view bytes)
    {
        while (!bytes.empty())
        {
            if (bytes.starts_with("\x1bP"))
            {
                auto const terminator = bytes.find("\x1b\\");
                REQUIRE(terminator != std::string_view::npos);
                auto const sequence = bytes.substr(2, terminator - 2);
                auto const introducer = sequence.find('q');
                REQUIRE(introducer != std::string_view::npos);
                Image(sequence.substr(introducer + 1));
                bytes.remove_prefix(terminator + 2);
                continue;
            }
            if (bytes.starts_with("\x1b["))
            {
                bytes.remove_prefix(2);
                auto const terminator =
                    bytes.find_first_of("@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~");
                REQUIRE(terminator != std::string_view::npos);
                Control(bytes.substr(0, terminator), bytes[terminator]);
                bytes.remove_prefix(terminator + 1);
                continue;
            }
            if (bytes.front() == '\n')
            {
                LineFeed();
                bytes.remove_prefix(1);
                continue;
            }
            if (bytes.front() == '\r')
            {
                _column = 0;
                bytes.remove_prefix(1);
                continue;
            }
            auto const length = Utf8SequenceLength(bytes);
            REQUIRE(length > 0);
            _cells[_row][_column] = std::string { bytes.substr(0, length) };
            _imaged[_row][_column] = false;
            if (_column + 1 < _columns)
                ++_column;
            bytes.remove_prefix(length);
        }
    }

    /// @return Row @p row as drawn, every cell included.
    [[nodiscard]] std::string Row(std::size_t row) const
    {
        auto text = std::string {};
        for (auto const& cell: _cells[row])
            text += cell;
        return text;
    }

    /// @return Each cell an image covers now, as `(row, column)`, 0-based, row by row.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> ImagedCells() const
    {
        auto cells = std::vector<std::pair<std::size_t, std::size_t>> {};
        for (auto const row: std::views::iota(std::size_t { 0 }, _imaged.size()))
            for (auto const column: std::views::iota(std::size_t { 0 }, _columns))
                if (_imaged[row][column])
                    cells.emplace_back(row, column);
        return cells;
    }

    /// @return Every image drawn so far, in the order drawn, whether or not it is still on screen.
    [[nodiscard]] std::vector<DrawnImage> const& Drawn() const noexcept
    {
        return _drawn;
    }

    /// @return The sequences this model did not know, each as `<parameters><final>`.
    [[nodiscard]] std::vector<std::string> const& Unknown() const noexcept
    {
        return _unknown;
    }

  private:
    void Control(std::string_view parameters, char terminator)
    {
        if (parameters.starts_with('?') && (terminator == 'h' || terminator == 'l'))
            return;
        if (terminator == 'H')
        {
            auto const separator = parameters.find(';');
            auto const row = separator == std::string_view::npos ? parameters : parameters.substr(0, separator);
            auto const column = separator == std::string_view::npos ? std::string_view {} : parameters.substr(separator + 1);
            _row = std::min(Ordinal(row), _cells.size()) - 1;
            _column = std::min(Ordinal(column), _columns) - 1;
            return;
        }
        if (terminator == 'K' && parameters.empty())
        {
            for (auto const column: std::views::iota(_column, _columns))
            {
                _cells[_row][column] = " ";
                _imaged[_row][column] = false;
            }
            return;
        }
        if (terminator == 'J' && parameters.empty())
        {
            Control({}, 'K');
            for (auto const row: std::views::iota(_row + 1, _cells.size()))
            {
                _cells[row].assign(_columns, " ");
                _imaged[row].assign(_columns, false);
            }
            return;
        }
        _unknown.push_back(std::string { parameters } + terminator);
    }

    void LineFeed()
    {
        if (_row + 1 < _cells.size())
        {
            ++_row;
            return;
        }
        _cells.erase(_cells.begin());
        _cells.emplace_back(_columns, " ");
        _imaged.erase(_imaged.begin());
        _imaged.emplace_back(_columns, false);
    }

    /// Draw a Sixel image from the cursor. The body must open with raster attributes
    /// (`"<pan>;<pad>;<width>;<height>`), which is what says how many pixels it covers.
    void Image(std::string_view body)
    {
        REQUIRE(body.starts_with('"'));
        auto numbers = std::vector<std::size_t> { 0 };
        for (auto const character: body.substr(1))
        {
            if (character == ';')
                numbers.push_back(0);
            else if (character >= '0' && character <= '9')
                numbers.back() = (numbers.back() * 10) + static_cast<std::size_t>(character - '0');
            else
                break;
        }
        REQUIRE(numbers.size() == 4);
        auto const across = (numbers[2] + _cell.width - 1) / _cell.width;
        auto const down = (numbers[3] + _cell.height - 1) / _cell.height;
        for (auto const row: std::views::iota(_row, std::min(_row + down, _imaged.size())))
            for (auto const column: std::views::iota(_column, std::min(_column + across, _columns)))
                _imaged[row][column] = true;
        _drawn.push_back(DrawnImage { .row = _row, .column = _column, .body = std::string { body } });
    }

    /// @return A 1-based CSI parameter, where empty and 0 both mean 1.
    [[nodiscard]] static std::size_t Ordinal(std::string_view digits)
    {
        auto value = std::size_t { 0 };
        for (auto const digit: digits)
            value = (value * 10) + static_cast<std::size_t>(digit - '0');
        return std::max<std::size_t>(value, 1);
    }

    std::size_t _columns;
    CellPixelSize _cell;
    std::vector<std::vector<std::string>> _cells;
    std::vector<std::vector<bool>> _imaged;
    std::size_t _row { 0 };
    std::size_t _column { 0 };
    std::vector<std::string> _unknown;
    std::vector<DrawnImage> _drawn;
};

/// @return The cells of the rectangle @p high by @p wide from @p top, @p left, 0-based, row by row:
///         `NoAutoReturnScreen::ImagedCells` for an image covering exactly that.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> Rectangle(std::size_t top,
                                                                         std::size_t left,
                                                                         std::size_t high,
                                                                         std::size_t wide)
{
    auto cells = std::vector<std::pair<std::size_t, std::size_t>> {};
    for (auto const row: std::views::iota(top, top + high))
        for (auto const column: std::views::iota(left, left + wide))
            cells.emplace_back(row, column);
    return cells;
}
} // namespace

TEST_CASE("full-width frame rows each start in the first column on a terminal whose line feed does not return",
          "[cli][dashboard][terminal]")
{
    // The defect measured in a 120x40 ConPTY: rows joined by line feeds, each exactly the frame's
    // width, so after the first the cursor sits in the last column and every later row lands there.
    // The frame is as wide as the screen, which is the case that exposes it -- and the one where an
    // erase written after a row would take the row's last cell. Synchronized or not changes nothing
    // drawn.
    auto const frame = std::string { "\u250c\u2500 node \u2500\u2500\u2510\n\u2502 up 3s   "
                                     "\u2502\n\u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518" };
    for (auto const synchronized: { false, true })
    {
        auto screen = NoAutoReturnScreen { 11, 5 };
        screen.Feed(FrameBytes(frame, synchronized));

        CHECK(screen.Row(0) == "\u250c\u2500 node \u2500\u2500\u2510");
        CHECK(screen.Row(1) == "\u2502 up 3s   \u2502");
        CHECK(screen.Row(2) == "\u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518");
        CHECK(screen.Row(3) == "           ");
        CHECK(screen.Row(4) == "           ");
        CHECK(screen.Unknown().empty());
    }
}

TEST_CASE("a frame shorter than the one before it leaves nothing of the earlier frame on screen",
          "[cli][dashboard][terminal]")
{
    // A resize or a panel losing a line draws fewer rows; the rows below, and the tail of a row that got
    // shorter, must not keep the previous frame's text.
    auto screen = NoAutoReturnScreen { 8, 5 };
    screen.Feed(FrameBytes("AAAAAAAA\nBBBBBBBB\nCCCCCCCC\nDDDDDDDD", false));
    screen.Feed(FrameBytes("xy\nzzzzzzzz", false));

    CHECK(screen.Row(0) == "xy      ");
    CHECK(screen.Row(1) == "zzzzzzzz");
    CHECK(screen.Row(2) == "        ");
    CHECK(screen.Row(3) == "        ");
    CHECK(screen.Row(4) == "        ");
    CHECK(screen.Unknown().empty());
}

TEST_CASE("a frame as tall as the screen that ends in a newline keeps its bottom row", "[cli][dashboard][terminal]")
{
    // The final newline ends the last row. Read as an empty row below it, it would be placed on a
    // row the screen does not have -- clamped to the bottom one -- and erase it.
    auto screen = NoAutoReturnScreen { 4, 3 };
    screen.Feed(FrameBytes("top \nmid \nbot \n", false));

    CHECK(screen.Row(0) == "top ");
    CHECK(screen.Row(1) == "mid ");
    CHECK(screen.Row(2) == "bot ");
    CHECK(screen.Unknown().empty());
}

TEST_CASE("a DECRQM answer for mode 2026 keeps its meaning as a synchronized-output answer", "[cli][dashboard][terminal]")
{
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::Set) == SynchronizedOutputAnswer::Supported);
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::Reset) == SynchronizedOutputAnswer::Supported);
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::NotRecognized) == SynchronizedOutputAnswer::NotSupported);
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::PermanentlySet) == SynchronizedOutputAnswer::NotSupported);
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::PermanentlyReset) == SynchronizedOutputAnswer::NotSupported);
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::NoReply) == SynchronizedOutputAnswer::NoReply);
    CHECK(ToSynchronizedOutputAnswer(tui::DecModeStatus::NotAsked) == SynchronizedOutputAnswer::NotAsked);
}

TEST_CASE("a started terminal enters the alternate screen once and its destruction leaves it once",
          "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto const& screen = ScreenBytes();

    auto outcome = StartFake({}, record, pool, reactor);
    REQUIRE(outcome.started.has_value());
    CHECK(Occurrences(record.Written(), screen.enter) == 1);
    CHECK(Occurrences(record.Written(), screen.leave) == 0);

    outcome.started->frames.reset();
    outcome.started->events.reset();
    auto const written = record.Written();
    CHECK(Occurrences(written, screen.enter) == 1);
    CHECK(Occurrences(written, screen.leave) == 1);
    CHECK(written.find(screen.enter) < written.find(screen.leave));
    CHECK(record.restores.load() == 1);
}

TEST_CASE("a start that fails after entering the alternate screen leaves it before the failure is delivered",
          "[cli][dashboard][terminal]")
{
    // The encoding is read after the screen is entered, so its failure is the halfway point.
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto const& screen = ScreenBytes();

    auto const outcome = StartFake({ .throwOnEncoding = true }, record, pool, reactor);

    REQUIRE_FALSE(outcome.started.has_value());
    CHECK(outcome.started.error() == "the terminal failed while being started: the environment could not be read");
    auto const written = record.Written();
    CHECK(Occurrences(written, screen.enter) == 1);
    CHECK(Occurrences(written, screen.leave) == 1);
    CHECK(outcome.restoresWhenDelivered == 1);
}

TEST_CASE("a start that fails before entering the alternate screen writes neither entering nor leaving",
          "[cli][dashboard][terminal]")
{
    // The control on the case above: a guard that left the screen unconditionally would pass that one
    // and write a leave here, onto a screen nobody entered.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto refusedRecord = DeviceRecord {};
    auto const refused = StartFake({ .refuseAcquire = true }, refusedRecord, pool, reactor);
    REQUIRE_FALSE(refused.started.has_value());
    CHECK(refusedRecord.Written().empty());

    auto thrownRecord = DeviceRecord {};
    auto const thrown = StartFake({ .throwOnAsk = true }, thrownRecord, pool, reactor);
    REQUIRE_FALSE(thrown.started.has_value());
    CHECK(thrownRecord.Written().empty());
    CHECK(thrownRecord.restores.load() == 1);
}

TEST_CASE("restoring a started terminal now leaves the alternate screen, and a later destruction does not leave it again",
          "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto const& screen = ScreenBytes();

    auto outcome = StartFake({}, record, pool, reactor);
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;

    started.restore->RestoreNow();
    started.restore->RestoreNow();
    auto const afterRestore = record.Written();
    CHECK(Occurrences(afterRestore, screen.leave) == 1);
    // Synchronized output is ended BEFORE the screen is left, so a frame the restore interrupted
    // cannot keep the terminal holding its output.
    CHECK(afterRestore.rfind(screen.syncEnd) < afterRestore.rfind(screen.leave));

    started.frames.reset();
    started.events.reset();
    CHECK(Occurrences(record.Written(), screen.enter) == 1);
    CHECK(Occurrences(record.Written(), screen.leave) == 1);
    CHECK(record.restores.load() == 1);
}

TEST_CASE("a started terminal destroyed before a restore-now leaves the alternate screen exactly once",
          "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto const& screen = ScreenBytes();

    auto outcome = StartFake({}, record, pool, reactor);
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;

    started.frames.reset();
    started.events.reset();
    started.restore->RestoreNow();
    CHECK(Occurrences(record.Written(), screen.enter) == 1);
    CHECK(Occurrences(record.Written(), screen.leave) == 1);
    CHECK(record.restoredNow.load() == 0);
}

TEST_CASE("a frame on a terminal that reported synchronized output is bracketed in it", "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto const& screen = ScreenBytes();

    auto outcome = StartFake({ .synchronizedOutput = SynchronizedOutputAnswer::Supported }, record, pool, reactor);
    REQUIRE(outcome.started.has_value());
    REQUIRE(outcome.started->frames != nullptr);
    CHECK(outcome.started->capabilities.synchronizedOutput == SynchronizedOutputAnswer::Supported);

    auto const before = record.Written().size();
    outcome.started->frames->Present("PANELS");
    auto const frame = record.Written().substr(before);

    CHECK(frame == FrameBytes("PANELS", true));
    CHECK(frame.starts_with(screen.syncBegin));
    CHECK(frame.ends_with(screen.syncEnd));
}

TEST_CASE("a frame on a terminal that did not report synchronized output is never bracketed in it",
          "[cli][dashboard][terminal]")
{
    // Every answer but Supported, each its own start. The guard between the DECRQM answer and the
    // bytes is the whole of this case: a presenter that bracketed unconditionally passes the case
    // above and fails every row here.
    auto const& screen = ScreenBytes();
    for (auto const answer:
         { SynchronizedOutputAnswer::NotSupported, SynchronizedOutputAnswer::NoReply, SynchronizedOutputAnswer::NotAsked })
    {
        auto record = DeviceRecord {};
        auto clock = ManualClock {};
        auto reactor = TestReactor { clock };
        auto pool = ThreadPoolExecutor { 1 };

        auto outcome = StartFake({ .synchronizedOutput = answer }, record, pool, reactor);
        REQUIRE(outcome.started.has_value());
        REQUIRE(outcome.started->frames != nullptr);

        auto const before = record.Written().size();
        outcome.started->frames->Present("PANELS");
        auto const frame = record.Written().substr(before);

        CHECK(frame == FrameBytes("PANELS", false));
        CHECK(Occurrences(record.Written(), screen.syncBegin) == 0);
    }
}

TEST_CASE("a burst of resizes is delivered as the latest geometry, and a key between them keeps them apart",
          "[cli][dashboard][terminal]")
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    source.pushEvents({ tui::ResizeEvent { .columns = 81, .rows = 25 },
                        tui::ResizeEvent { .columns = 90, .rows = 30 },
                        tui::ResizeEvent { .columns = 100, .rows = 35 } });
    source.pushEvents(
        { tui::ResizeEvent { .columns = 110, .rows = 40 }, Key(U'a'), tui::ResizeEvent { .columns = 120, .rows = 45 } });
    auto stream = MakeTerminalEventStream(
        { .source = &source, .pool = &pool, .resumeOn = &reactor, .wake = {}, .columns = 80, .rows = 24 });

    auto const opened = TakeNext(*stream, reactor);
    auto const burst = TakeNext(*stream, reactor);
    auto const beforeKey = TakeNext(*stream, reactor);
    auto const key = TakeNext(*stream, reactor);
    auto const afterKey = TakeNext(*stream, reactor);

    CHECK(opened.kind == DashboardEventKind::Resize);
    CHECK(opened.columns == 80);
    CHECK(burst.kind == DashboardEventKind::Resize);
    CHECK(burst.columns == 100);
    CHECK(burst.rows == 35);
    CHECK(beforeKey.kind == DashboardEventKind::Resize);
    CHECK(beforeKey.columns == 110);
    CHECK(key.kind == DashboardEventKind::Key);
    CHECK(afterKey.kind == DashboardEventKind::Resize);
    CHECK(afterKey.columns == 120);
    CHECK(afterKey.rows == 45);
    CHECK(source.waitCount() == 2);
}

namespace
{
/// A 40 by 40 pixel Sixel body, raster attributes first as `ISixelEncoder` writes them: four cells
/// across and two down at a 10 by 20 pixel cell.
constexpr auto FortyPixelChart = std::string_view { "\"1;1;40;40#0;2;100;0;0#0~~~~-~~~~" };

/// A three-row frame whose rows 2 and 3 leave columns 4 to 7 blank, and an image placed over exactly
/// those cells.
[[nodiscard]] DashboardFrame ChartFrame()
{
    return DashboardFrame {
        .text = "fleet   load\ncpu     rate\nmem     used",
        .placements = { FramePlacement {
            .row = 2, .column = 4, .cellsWide = 4, .cellsHigh = 2, .sixel = std::string { FortyPixelChart } } },
    };
}

/// @return @p size as `(width, height)`, or `(0, 0)` for none: comparable in a `CHECK`, and a swapped
///         width and height reads as different.
[[nodiscard]] std::pair<std::size_t, std::size_t> Pixels(std::optional<CellPixelSize> const& size)
{
    return size.has_value() ? std::pair { size->width, size->height } : std::pair<std::size_t, std::size_t> { 0, 0 };
}
} // namespace

TEST_CASE("an image placed in a frame is drawn on its cells after the rows, inside the frame's synchronized output",
          "[cli][dashboard][terminal]")
{
    // WHAT DISTINGUISHES: the cells the screen shows the image on. An image written before the rows is
    // taken off again by the erase of the row it sits in, and one written without placing the cursor
    // lands where the last row left it; both still write the image's bytes somewhere.
    auto const frame = ChartFrame();
    auto const& screen = ScreenBytes();
    for (auto const synchronized: { false, true })
    {
        auto const bytes = FrameBytes(frame, synchronized);
        auto terminal = NoAutoReturnScreen { 12, 4 };
        terminal.Feed(bytes);

        CHECK(terminal.ImagedCells() == Rectangle(1, 3, 2, 4));
        REQUIRE(terminal.Drawn().size() == 1);
        CHECK(terminal.Drawn().front().body == FortyPixelChart);
        CHECK(terminal.Row(0) == "fleet   load");
        CHECK(terminal.Row(1) == "cpu     rate");
        CHECK(terminal.Row(2) == "mem     used");
        CHECK(terminal.Unknown().empty());

        // The rows exactly as a frame with no image, then the cursor placed on the image's first cell and
        // the image framed by endo, and only then the end of the bracket.
        auto expected = std::string { synchronized ? screen.syncBegin : "" };
        expected.append(FrameBytes(frame.text, false));
        expected.append("\x1b[2;4H\x1bP0;1q");
        expected.append(FortyPixelChart);
        expected.append("\x1b\\");
        expected.append(synchronized ? screen.syncEnd : "");
        CHECK(bytes == expected);
    }
}

TEST_CASE("a frame drawn without an earlier frame's image leaves none of that image on screen", "[cli][dashboard][terminal]")
{
    auto const placed = ChartFrame();

    // The control: a frame that places the image again shows it once, where it was. Without it, an empty
    // screen below could be a model that never keeps an image rather than a frame that removed one.
    auto again = NoAutoReturnScreen { 12, 4 };
    again.Feed(FrameBytes(placed, false));
    again.Feed(FrameBytes(placed, false));
    CHECK(again.ImagedCells() == Rectangle(1, 3, 2, 4));

    // The same text without the image: each covered row is erased and written again.
    auto same = NoAutoReturnScreen { 12, 4 };
    same.Feed(FrameBytes(placed, false));
    REQUIRE(same.ImagedCells() == Rectangle(1, 3, 2, 4));
    same.Feed(FrameBytes(DashboardFrame { .text = placed.text, .placements = {} }, false));
    CHECK(same.ImagedCells().empty());
    CHECK(same.Row(1) == "cpu     rate");
    CHECK(same.Row(2) == "mem     used");

    // Rows that end before the image's first column write no character on its cells, so only the erase
    // of each row removes it.
    auto narrower = NoAutoReturnScreen { 12, 4 };
    narrower.Feed(FrameBytes(placed, false));
    REQUIRE(narrower.ImagedCells() == Rectangle(1, 3, 2, 4));
    narrower.Feed(FrameBytes(DashboardFrame { .text = "fleet   load\ncpu\nmem", .placements = {} }, false));
    CHECK(narrower.ImagedCells().empty());
    CHECK(narrower.Row(1) == "cpu         ");

    // A shorter frame whose only row is above the image: the erase below the frame's last row takes it.
    auto shorter = NoAutoReturnScreen { 12, 4 };
    shorter.Feed(FrameBytes(placed, true));
    REQUIRE(shorter.ImagedCells() == Rectangle(1, 3, 2, 4));
    shorter.Feed(FrameBytes(DashboardFrame { .text = "fleet", .placements = {} }, true));
    CHECK(shorter.ImagedCells().empty());
    CHECK(shorter.Row(0) == "fleet       ");
    CHECK(shorter.Unknown().empty());
}

TEST_CASE("a started terminal's frames draw the images placed in them", "[cli][dashboard][terminal]")
{
    // The loop reaches a sink through `PresentPlaced`. A presenter that wrote only the frame's text would
    // pass every FrameBytes case above and never draw an image.
    for (auto const answer: { SynchronizedOutputAnswer::Supported, SynchronizedOutputAnswer::NoReply })
    {
        auto record = DeviceRecord {};
        auto clock = ManualClock {};
        auto reactor = TestReactor { clock };
        auto pool = ThreadPoolExecutor { 1 };

        auto outcome = StartFake({ .synchronizedOutput = answer }, record, pool, reactor);
        REQUIRE(outcome.started.has_value());
        REQUIRE(outcome.started->frames != nullptr);

        auto const frame = ChartFrame();
        auto const before = record.Written().size();
        outcome.started->frames->PresentPlaced(frame);

        CHECK(record.Written().substr(before) == FrameBytes(frame, answer == SynchronizedOutputAnswer::Supported));
    }
}

TEST_CASE("a CSI 16 t answer, and each way of having none, keeps its meaning as a cell size", "[cli][dashboard][terminal]")
{
    // endo answers width first; the reply on the wire is height first, and endo's own query test pins
    // that `CSI 6 ; 20 ; 10 t` answers ten wide and twenty high.
    using Answer = std::expected<std::pair<int, int>, tui::QueryUnanswered>;

    CHECK(Pixels(ToCellPixelSize(Answer { std::pair { 10, 20 } })) == std::pair<std::size_t, std::size_t> { 10, 20 });
    // A terminal answering zero is saying it does not know, and a size of zero draws nothing.
    CHECK_FALSE(ToCellPixelSize(Answer { std::pair { 0, 0 } }).has_value());
    CHECK_FALSE(ToCellPixelSize(Answer { std::pair { 10, 0 } }).has_value());
    CHECK_FALSE(ToCellPixelSize(Answer { std::pair { 0, 20 } }).has_value());
    CHECK_FALSE(ToCellPixelSize(Answer { std::pair { -10, 20 } }).has_value());
    CHECK_FALSE(ToCellPixelSize(Answer { std::unexpected(tui::QueryUnanswered::NoReply) }).has_value());
    CHECK_FALSE(ToCellPixelSize(Answer { std::unexpected(tui::QueryUnanswered::NotAsked) }).has_value());
}

TEST_CASE("a terminal that reports its cell size starts on the Sixel rung and is asked again after each resize",
          "[cli][dashboard][terminal]")
{
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto outcome = StartFake({ .cellPixels = CellPixelSize { .width = 10, .height = 20 },
                               .reread = CellPixelSize { .width = 9, .height = 18 },
                               .input = { tui::ResizeEvent { .columns = 100, .rows = 30 } } },
                             record,
                             pool,
                             reactor);
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;

    CHECK(Pixels(started.capabilities.cellPixels) == std::pair<std::size_t, std::size_t> { 10, 20 });
    CHECK(ChooseRenderRung(started.capabilities) == RenderRung::Sixel);

    auto const opened = TakeNext(*started.events, reactor);
    CHECK(Pixels(opened.cellPixels) == std::pair<std::size_t, std::size_t> { 10, 20 });
    CHECK(record.rereads.load() == 0);

    // The size the font change left, not the one the start measured.
    auto const resized = TakeNext(*started.events, reactor);
    CHECK(resized.kind == DashboardEventKind::Resize);
    CHECK(resized.columns == 100);
    CHECK(Pixels(resized.cellPixels) == std::pair<std::size_t, std::size_t> { 9, 18 });
    CHECK(record.rereads.load() == 1);
}

TEST_CASE("a terminal that does not report its cell size is not the Sixel rung and is never asked again",
          "[cli][dashboard][terminal]")
{
    // It advertised Sixel, so the missing size is the only thing keeping it off the Sixel rung.
    auto record = DeviceRecord {};
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto outcome = StartFake({ .sixel = SixelAnswer::Advertised,
                               .cellPixels = std::nullopt,
                               .reread = CellPixelSize { .width = 10, .height = 20 },
                               .input = { tui::ResizeEvent { .columns = 100, .rows = 30 } } },
                             record,
                             pool,
                             reactor);
    REQUIRE(outcome.started.has_value());
    auto& started = *outcome.started;

    CHECK_FALSE(started.capabilities.cellPixels.has_value());
    CHECK(ChooseRenderRung(started.capabilities) == RenderRung::Unicode);

    auto const opened = TakeNext(*started.events, reactor);
    CHECK_FALSE(opened.cellPixels.has_value());
    auto const resized = TakeNext(*started.events, reactor);
    CHECK(resized.kind == DashboardEventKind::Resize);
    CHECK_FALSE(resized.cellPixels.has_value());
    CHECK(record.rereads.load() == 0);
}

TEST_CASE("the cell size is re-read on the pool once per wait that resized, and a wait without a resize asks nothing",
          "[cli][dashboard][terminal]")
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    source.pushEvents({ tui::ResizeEvent { .columns = 100, .rows = 30 }, tui::ResizeEvent { .columns = 110, .rows = 35 } });
    source.pushEvents({ Key(U'a') });
    auto asked = std::atomic<int> { 0 };
    auto askedOn = std::atomic<std::thread::id> {};
    auto stream = MakeTerminalEventStream({ .source = &source,
                                            .pool = &pool,
                                            .resumeOn = &reactor,
                                            .wake = {},
                                            .columns = 80,
                                            .rows = 24,
                                            .cellPixels = CellPixelSize { .width = 10, .height = 20 },
                                            .rereadCellPixels = [&asked, &askedOn] {
                                                askedOn.store(std::this_thread::get_id());
                                                asked.fetch_add(1);
                                                return std::optional { CellPixelSize { .width = 12, .height = 24 } };
                                            } });
    auto const driverThread = std::this_thread::get_id();

    auto const opened = TakeNext(*stream, reactor);
    auto const resized = TakeNext(*stream, reactor);
    CHECK(Pixels(opened.cellPixels) == std::pair<std::size_t, std::size_t> { 10, 20 });
    CHECK(resized.columns == 110);
    CHECK(Pixels(resized.cellPixels) == std::pair<std::size_t, std::size_t> { 12, 24 });
    CHECK(asked.load() == 1);
    CHECK(askedOn.load() != std::thread::id {});
    CHECK(askedOn.load() != driverThread);

    auto const key = TakeNext(*stream, reactor);
    CHECK(key.kind == DashboardEventKind::Key);
    CHECK(asked.load() == 1);
}

TEST_CASE("a resize whose re-read gets no answer carries no cell size, never the one from before it",
          "[cli][dashboard][terminal]")
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto source = tui::runtime::testing::MockEventSource {};
    source.pushEvents({ tui::ResizeEvent { .columns = 100, .rows = 30 } });
    auto stream = MakeTerminalEventStream({ .source = &source,
                                            .pool = &pool,
                                            .resumeOn = &reactor,
                                            .wake = {},
                                            .columns = 80,
                                            .rows = 24,
                                            .cellPixels = CellPixelSize { .width = 10, .height = 20 },
                                            .rereadCellPixels = [] { return std::optional<CellPixelSize> {}; } });

    auto const opened = TakeNext(*stream, reactor);
    auto const resized = TakeNext(*stream, reactor);
    CHECK(opened.cellPixels.has_value());
    CHECK(resized.kind == DashboardEventKind::Resize);
    CHECK_FALSE(resized.cellPixels.has_value());
}

namespace
{

/// @p bytes with every SGR sequence (`ESC [ digits and semicolons m`) taken out, and how many there were.
/// @param bytes Presenter output.
/// @return The rest, and the count.
[[nodiscard]] std::pair<std::string, std::size_t> SplitSgr(std::string_view bytes)
{
    auto out = std::pair<std::string, std::size_t> {};
    auto at = std::size_t { 0 };
    while (at < bytes.size())
    {
        if (bytes.substr(at).starts_with("\x1b["))
        {
            auto const end = bytes.find_first_not_of("0123456789;", at + 2);
            if (end != std::string_view::npos && bytes[end] == 'm')
            {
                ++out.second;
                at = end + 1;
                continue;
            }
        }
        out.first.push_back(bytes[at]);
        ++at;
    }
    return out;
}

/// @p bytes with every SGR sequence taken out.
/// @param bytes Presenter output.
/// @return The rest.
[[nodiscard]] std::string WithoutSgr(std::string_view bytes)
{
    return SplitSgr(bytes).first;
}

/// How many SGR sequences @p bytes carries.
/// @param bytes Presenter output.
/// @return The count.
[[nodiscard]] std::size_t SgrCount(std::string_view bytes)
{
    return SplitSgr(bytes).second;
}

/// A frame with a selected tab, a stale age, and a run past the end of its row.
/// @return The frame.
[[nodiscard]] DashboardFrame DressedFrame()
{
    return DashboardFrame {
        .text = "strip  [machines]  workers\nhb  0.90 s",
        .placements = {},
        .spans = { FrameSpan { .row = 1, .byte = 7, .length = 10, .tone = FrameTone::Selected },
                   FrameSpan { .row = 2, .byte = 4, .length = 6, .tone = FrameTone::Stale },
                   FrameSpan { .row = 2, .byte = 9, .length = 6, .tone = FrameTone::Fresh } },
    };
}

} // namespace

TEST_CASE("a dressed run is the same bytes with the palette's style around them, where the record allows colour",
          "[cli][dashboard][terminal][tone]")
{
    // #134 G1, the presenter half. WHAT DISTINGUISHES: with colour allowed, every SGR taken out leaves exactly
    // the plain frame's bytes -- a presenter that rewrote a run, or wrote it twice, fails that -- the tab sits
    // inside inverse video and a reset, the stale age inside its palette colour, and the run past its row's
    // end dresses nothing. With colour suppressed, or never decided, not ONE SGR is written.
    auto const frame = DressedFrame();
    auto const plain = FrameBytes(frame, false);

    auto coloured = TerminalCapabilities {};
    coloured.colour = ColourAnswer::Supported;
    auto const dressed = FrameBytes(frame, coloured);
    CHECK(WithoutSgr(dressed) == plain);
    CHECK(dressed.contains("\x1b[7m[machines]\x1b[m"));
    CHECK(dressed.contains("0.90 s\x1b[m"));
    CHECK(SgrCount(dressed) == 4);

    for (auto const answer: { ColourAnswer::Suppressed, ColourAnswer::NotAsked })
    {
        auto record = TerminalCapabilities {};
        record.colour = answer;
        CHECK(FrameBytes(frame, record) == plain);
        CHECK(SgrCount(FrameBytes(frame, record)) == 0);
    }
}

TEST_CASE("every tone has a palette row, and a record that allows no colour dresses no tone",
          "[cli][dashboard][terminal][tone]")
{
    // One palette, looked up in one place. WHAT DISTINGUISHES: a tone added without a row fails the build at
    // the table (`RowsInEnumeratorOrder`); here, every tone answers a row where colour is allowed and none
    // where it is not, and the selected tone is the one drawn inverse.
    auto coloured = TerminalCapabilities {};
    coloured.colour = ColourAnswer::Supported;
    auto const plain = TerminalCapabilities {};
    for (auto const tone: Enumerators<FrameTone>())
    {
        INFO("tone " << static_cast<std::size_t>(tone));
        REQUIRE(PaletteFor(tone, coloured) != nullptr);
        CHECK(PaletteFor(tone, coloured)->tone == tone);
        CHECK(PaletteFor(tone, plain) == nullptr);
    }
    CHECK(PaletteFor(FrameTone::Selected, coloured)->inverse);
    CHECK(PaletteFor(FrameTone::Last, coloured) == nullptr);
}

TEST_CASE("a started terminal records its colour answer and its frames are dressed by it",
          "[cli][dashboard][terminal][tone]")
{
    // The decision is on the capability record, like the rest of the ladder. WHAT DISTINGUISHES: the device's
    // answer lands on the record, and the presenter the start built dresses a frame exactly as the record's
    // overload does -- a presenter keeping only the synchronized flag writes the plain frame for both answers.
    for (auto const answer: { ColourAnswer::Supported, ColourAnswer::Suppressed })
    {
        auto record = DeviceRecord {};
        auto clock = ManualClock {};
        auto reactor = TestReactor { clock };
        auto pool = ThreadPoolExecutor { 1 };

        auto outcome = StartFake({ .colour = answer }, record, pool, reactor);
        REQUIRE(outcome.started.has_value());
        CHECK(outcome.started->capabilities.colour == answer);

        auto const frame = DressedFrame();
        auto const before = record.Written().size();
        outcome.started->frames->PresentPlaced(frame);
        auto const written = record.Written().substr(before);
        CHECK(written == FrameBytes(frame, outcome.started->capabilities));
        CHECK((SgrCount(written) > 0) == (answer == ColourAnswer::Supported));
    }
}
