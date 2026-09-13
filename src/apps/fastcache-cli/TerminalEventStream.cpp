// SPDX-License-Identifier: Apache-2.0
#include "TerminalEventStream.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <tui/KeyCode.hpp>
#include <tui/Modifier.hpp>
#include <tui/TerminalOutput.hpp>

namespace FastCache::Cli
{

namespace
{
    /// A named key and the bytes a terminal sends for it.
    struct NamedKeyRow
    {
        tui::KeyCode key;
        std::string_view bytes;
    };

    /// The named keys that have a byte spelling, as a table so another key is a row.
    ///
    /// The spellings are the ordinary xterm ones. They only have to be RIGHT for the keys the
    /// dashboard acts on, and `ESC` is the one of those here; the rest are spelled so that a
    /// consumer comparing bytes sees what an operator pressed rather than nothing.
    constexpr auto NamedKeys = std::to_array<NamedKeyRow>({
        { .key = tui::KeyCode::Escape, .bytes = "\x1b" },
        { .key = tui::KeyCode::Enter, .bytes = "\r" },
        { .key = tui::KeyCode::Tab, .bytes = "\t" },
        { .key = tui::KeyCode::Backspace, .bytes = "\x7f" },
        { .key = tui::KeyCode::Up, .bytes = "\x1b[A" },
        { .key = tui::KeyCode::Down, .bytes = "\x1b[B" },
        { .key = tui::KeyCode::Right, .bytes = "\x1b[C" },
        { .key = tui::KeyCode::Left, .bytes = "\x1b[D" },
        { .key = tui::KeyCode::Home, .bytes = "\x1b[H" },
        { .key = tui::KeyCode::End, .bytes = "\x1b[F" },
        { .key = tui::KeyCode::Insert, .bytes = "\x1b[2~" },
        { .key = tui::KeyCode::Delete, .bytes = "\x1b[3~" },
        { .key = tui::KeyCode::PageUp, .bytes = "\x1b[5~" },
        { .key = tui::KeyCode::PageDown, .bytes = "\x1b[6~" },
    });

    /// How long a wait lasts right after input arrived.
    ///
    /// endo decides that a lone `ESC` is the Escape key, rather than the start of a sequence,
    /// only when a wait TIMES OUT with nothing after it. A wait that blocks indefinitely never
    /// times out, so the first wait after any input is this short one and the next is unbounded
    /// again. The figure is the one endo's parser documents for this decision.
    constexpr auto EscapeDecisionMs = 50;

    /// Whether @p modifiers includes @p wanted.
    [[nodiscard]] constexpr bool Has(tui::Modifier modifiers, tui::Modifier wanted) noexcept
    {
        return (modifiers & wanted) == wanted;
    }

    /// @return @p codepoint as UTF-8, or empty when it is not a Unicode scalar value.
    [[nodiscard]] std::string EncodeUtf8(char32_t codepoint)
    {
        auto const value = static_cast<std::uint32_t>(codepoint);
        auto bytes = std::string {};
        auto const push = [&bytes](std::uint32_t byte) {
            bytes.push_back(static_cast<char>(byte));
        };
        if (value < 0x80U)
            push(value);
        else if (value < 0x800U)
        {
            push(0xC0U | (value >> 6U));
            push(0x80U | (value & 0x3FU));
        }
        else if (value < 0x10000U)
        {
            if (value >= 0xD800U && value <= 0xDFFFU)
                return {};
            push(0xE0U | (value >> 12U));
            push(0x80U | ((value >> 6U) & 0x3FU));
            push(0x80U | (value & 0x3FU));
        }
        else if (value <= 0x10FFFFU)
        {
            push(0xF0U | (value >> 18U));
            push(0x80U | ((value >> 12U) & 0x3FU));
            push(0x80U | ((value >> 6U) & 0x3FU));
            push(0x80U | (value & 0x3FU));
        }
        return bytes;
    }

    /// The stream itself. See `MakeTerminalEventStream`.
    class TerminalEventStream final: public IDashboardEventSource
    {
      public:
        explicit TerminalEventStream(TerminalStreamParts parts):
            _source { parts.source },
            _pool { parts.pool },
            _resumeOn { parts.resumeOn },
            _wake { std::move(parts.wake) },
            _rereadCellPixels { std::move(parts.rereadCellPixels) }
        {
            _ready.push_back(DashboardEvent { .kind = DashboardEventKind::Resize,
                                              .columns = parts.columns,
                                              .rows = parts.rows,
                                              .cellPixels = parts.cellPixels });
        }

        [[nodiscard]] Task<DashboardEvent> Next() override
        {
            while (true)
            {
                // Closed first: after `Close()` the answer is `Detached`, even with events still
                // queued, because the caller has stopped reading them.
                if (_closed.load(std::memory_order_acquire))
                    co_return DashboardEvent { .kind = DashboardEventKind::Detached,
                                               .note = "the dashboard closed its terminal" };
                if (!_ready.empty())
                {
                    auto event = std::move(_ready.front());
                    _ready.pop_front();
                    if (event.kind == DashboardEventKind::Detached)
                        _ended = event.note;
                    co_return event;
                }
                if (!_ended.empty())
                    co_return DashboardEvent { .kind = DashboardEventKind::Detached, .note = _ended };

                auto const timeoutMs = _inputJustArrived ? EscapeDecisionMs : -1;

                // The wait blocks, so it runs on the pool, and the result is used only after the
                // hop back: the stream's state belongs to the thread `Next()` is awaited on.
                co_await ResumeOn { *_pool };
                auto outcome = _source->wait(timeoutMs);
                // Still on the pool, before the next wait: the query reads its reply from the input
                // this wait just finished reading, and hands back anything else it reads there.
                auto const reread =
                    _rereadCellPixels && std::ranges::any_of(outcome.events, [](tui::InputEvent const& input) {
                        return std::holds_alternative<tui::ResizeEvent>(input);
                    });
                auto const measured = reread ? _rereadCellPixels() : std::nullopt;
                co_await ResumeOn { *_resumeOn };

                // A short wait that came back empty has had its chance to settle a lone ESC.
                _inputJustArrived = !(timeoutMs >= 0 && outcome.events.empty());
                for (auto& event: ToDashboardEvents(outcome, measured))
                    Enqueue(std::move(event));
            }
        }

        void Close() noexcept override
        {
            if (_closed.exchange(true, std::memory_order_acq_rel))
                return;
            if (_wake)
                _wake();
        }

      private:
        /// Queue @p event, coalescing a resize into one still waiting at the back.
        void Enqueue(DashboardEvent event)
        {
            if (event.kind == DashboardEventKind::Resize && !_ready.empty()
                && _ready.back().kind == DashboardEventKind::Resize)
            {
                _ready.back() = std::move(event);
                return;
            }
            _ready.push_back(std::move(event));
        }

        tui::runtime::EventSource* _source;
        IExecutor* _pool;
        IExecutor* _resumeOn;
        std::function<void()> _wake;
        std::function<std::optional<CellPixelSize>()> _rereadCellPixels;
        std::deque<DashboardEvent> _ready;
        std::string _ended;
        std::atomic<bool> _closed { false };
        bool _inputJustArrived { false };
    };
} // namespace

namespace
{
    /// Begin and end synchronized output (DEC mode 2026).
    ///
    /// The only presentation bytes spelled in this project. endo writes these solely from inside
    /// `SyncGuard`, directly to a native handle, so unlike the rest of `TerminalScreenBytes` there is
    /// no buffered call to capture them from.
    constexpr auto SyncBegin = std::string_view { "\x1b[?2026h" };
    constexpr auto SyncEnd = std::string_view { "\x1b[?2026l" };

    /// `CAN`: abandons an escape sequence a frame was halfway through, so what follows is not read
    /// as its tail.
    constexpr auto Cancel = std::string_view { "\x18" };

    /// An endo `TerminalOutput` whose destination is a string, so a sequence endo spells can be read
    /// back as bytes rather than restated.
    class CapturedOutput final: public tui::TerminalOutput
    {
      public:
        /// @return What was flushed so far.
        [[nodiscard]] std::string Take()
        {
            flush();
            return std::move(_captured);
        }

      protected:
        void writeToDestination(std::string_view bytes) override
        {
            _captured.append(bytes);
        }

      private:
        std::string _captured;
    };

    /// @return The bytes `RestoreNow` writes before the input's resets on a started terminal: abandon
    /// a half-written sequence, end synchronized output, then leave the screen. In that order, and
    /// the screen BEFORE the input protocols: the keyboard protocol keeps a separate stack per
    /// screen, and its pop must land on the screen its push did.
    [[nodiscard]] std::string RestoreNowLeading()
    {
        auto leading = std::string { Cancel };
        leading.append(SyncEnd);
        leading.append(ScreenBytes().leave);
        return leading;
    }

    /// `StartedTerminal::restore`: at most one `RestoreNow` reaches the device, and none once the
    /// device's teardown has begun.
    ///
    /// A mutex rather than an atomic flag, because a flag cannot keep the DEVICE alive: a call from
    /// another thread that has passed the check must finish before the events' destructor tears the
    /// device down. The lock is held across the device call, and `Detach` takes it, so that
    /// destructor waits.
    ///
    /// It also decides who leaves the alternate screen. The start entered it; exactly one of this
    /// handle's `RestoreNow` and the events' destructor leaves it, whichever comes first.
    class TerminalRestoreHandle final: public ITerminalRestore
    {
      public:
        TerminalRestoreHandle(ITerminalDevice* device, std::string leading) noexcept:
            _leading { std::move(leading) },
            _device { device }
        {
        }

        void RestoreNow() noexcept override
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_device == nullptr)
                return;
            _device->RestoreNow(_leading);
            // The idempotence: nothing reaches the device through this handle again, and the
            // screen has been left.
            _device = nullptr;
        }

        /// The device is being torn down. After this returns, no call reaches the device through this
        /// handle, and none is still inside it.
        /// @return Whether the screen is still entered -- no `RestoreNow` got there first -- so the
        ///         caller leaves it.
        [[nodiscard]] bool Detach() noexcept
        {
            auto const lock = std::scoped_lock { _mutex };
            auto const stillEntered = _device != nullptr;
            _device = nullptr;
            return stillEntered;
        }

      private:
        std::string _leading;
        std::mutex _mutex;
        ITerminalDevice* _device;
    };

    /// The frames of a started terminal: one `Write` per `Present`, spelled by `FrameBytes`.
    ///
    /// Shares the device with the events rather than borrowing it, so a frame presented after the
    /// events are gone -- which the contract forbids -- reaches a torn-down terminal rather than freed
    /// memory.
    class TerminalFramePresenter final: public IFrameSink
    {
      public:
        TerminalFramePresenter(std::shared_ptr<ITerminalDevice> device, bool synchronized) noexcept:
            _device { std::move(device) },
            _synchronized { synchronized }
        {
        }

        void Present(std::string_view frame) override
        {
            _device->Write(FrameBytes(frame, _synchronized));
        }

        void PresentPlaced(DashboardFrame const& frame) override
        {
            _device->Write(FrameBytes(frame, _synchronized));
        }

      private:
        std::shared_ptr<ITerminalDevice> _device;
        bool _synchronized;
    };

    /// The events of a started terminal, sharing the device they are read from.
    ///
    /// Member ORDER is the lifetime: the stream borrows the device's event source, so the device is
    /// declared first and destroyed last, and the destructor leaves the screen and restores the
    /// device once nothing reads it.
    ///
    /// Constructing one cannot throw: the stream is built BEFORE the device is handed over, so no
    /// failure can leave the device held by a half-built object whose destructor never runs.
    class StartedTerminalEvents final: public IDashboardEventSource
    {
      public:
        StartedTerminalEvents(std::shared_ptr<ITerminalDevice> device,
                              std::unique_ptr<IDashboardEventSource> stream,
                              std::shared_ptr<TerminalRestoreHandle> restore) noexcept:
            _device { std::move(device) },
            _stream { std::move(stream) },
            _restore { std::move(restore) }
        {
        }

        StartedTerminalEvents(StartedTerminalEvents const&) = delete;
        StartedTerminalEvents(StartedTerminalEvents&&) = delete;
        StartedTerminalEvents& operator=(StartedTerminalEvents const&) = delete;
        StartedTerminalEvents& operator=(StartedTerminalEvents&&) = delete;

        ~StartedTerminalEvents() override
        {
            _stream.reset();
            // Before the teardown, so a restore-now racing it from another thread either finished
            // already -- and left the screen -- or never reaches the device.
            if (_restore->Detach())
                _device->Write(ScreenBytes().leave);
            _device->Restore();
        }

        [[nodiscard]] Task<DashboardEvent> Next() override
        {
            return _stream->Next();
        }

        void Close() noexcept override
        {
            _stream->Close();
        }

      private:
        std::shared_ptr<ITerminalDevice> _device;
        std::unique_ptr<IDashboardEventSource> _stream;
        std::shared_ptr<TerminalRestoreHandle> _restore;
    };

    /// Leaves the screen and restores a device when a start leaves by any path but success.
    ///
    /// A guard rather than a restore call on each failure path, because the path that matters most
    /// is the one nobody writes a call on: an exception thrown after raw mode, or the alternate
    /// screen, was already entered.
    class RestoreUnlessKept
    {
      public:
        explicit RestoreUnlessKept(ITerminalDevice* device) noexcept:
            _device { device }
        {
        }

        RestoreUnlessKept(RestoreUnlessKept const&) = delete;
        RestoreUnlessKept(RestoreUnlessKept&&) = delete;
        RestoreUnlessKept& operator=(RestoreUnlessKept const&) = delete;
        RestoreUnlessKept& operator=(RestoreUnlessKept&&) = delete;

        ~RestoreUnlessKept()
        {
            if (_kept)
                return;
            if (_screenEntered)
                _device->Write(ScreenBytes().leave);
            _device->Restore();
        }

        /// The alternate screen is about to be entered, so a failure from here on leaves it. Marked
        /// BEFORE the write: a write that fails halfway may still have entered it, and leaving a
        /// screen that was never entered costs a cursor move where the opposite costs the shell.
        void ScreenEntering() noexcept
        {
            _screenEntered = true;
        }

        /// Ownership moved on, and whoever holds it leaves the screen and restores the device now.
        void Keep() noexcept
        {
            _kept = true;
        }

      private:
        ITerminalDevice* _device;
        bool _screenEntered { false };
        bool _kept { false };
    };
} // namespace

TerminalScreenBytes const& ScreenBytes()
{
    static auto const bytes = [] {
        auto const spell = [](void (*step)(tui::TerminalOutput&)) {
            auto output = CapturedOutput {};
            step(output);
            return output.Take();
        };
        return TerminalScreenBytes {
            .enter = spell([](tui::TerminalOutput& output) {
                output.enterAltScreen();
                output.hideCursor();
            }),
            .leave = spell([](tui::TerminalOutput& output) {
                output.showCursor();
                output.leaveAltScreen();
            }),
            .syncBegin = std::string { SyncBegin },
            .syncEnd = std::string { SyncEnd },
        };
    }();
    return bytes;
}

bool PresentsSynchronized(TerminalCapabilities const& capabilities) noexcept
{
    return capabilities.synchronizedOutput == SynchronizedOutputAnswer::Supported;
}

namespace
{
    /// `FrameBytes`, with or without images: the rows, then @p placements, in one bracket.
    [[nodiscard]] std::string RowsThenPlacements(std::string_view frame,
                                                 std::span<FramePlacement const> placements,
                                                 bool synchronized)
    {
        if (frame.ends_with('\n'))
            frame.remove_suffix(1);

        auto output = CapturedOutput {};
        if (synchronized)
            output.writeRaw(SyncBegin);

        // Everything from the frame's last row down is erased FIRST, and each row is erased before it is
        // written, never after: an erase starts AT the cursor, and a row as wide as the screen leaves the
        // cursor on its own last cell, so erasing after writing it would take that cell with it. Placing
        // the clear below the frame instead would put it on a row the screen may not have, clamped to the
        // bottom one.
        auto const rows = frame.empty() ? 0 : std::ranges::count(frame, '\n') + 1;
        output.moveTo(static_cast<int>(std::max<std::ptrdiff_t>(rows, 1)), 1);
        output.clearToEndOfDisplay();
        auto row = 1;
        for (auto const line: frame | std::views::split('\n'))
        {
            output.moveTo(row, 1);
            output.clearToEndOfLine();
            output.writeRaw(std::string_view { line.begin(), line.end() });
            ++row;
        }

        // After every row, or a row's erase and text would take the image off its cells again. Each
        // from its own position: a Sixel image is drawn from the cursor, and where the cursor is left
        // afterwards is the terminal's Sixel scrolling mode, not anything this frame decided.
        for (auto const& placement: placements)
        {
            output.moveTo(static_cast<int>(placement.row), static_cast<int>(placement.column));
            output.writeSixel(placement.sixel);
        }

        if (synchronized)
            output.writeRaw(SyncEnd);
        return output.Take();
    }
} // namespace

std::string FrameBytes(std::string_view frame, bool synchronized)
{
    return RowsThenPlacements(frame, {}, synchronized);
}

std::string FrameBytes(DashboardFrame const& frame, bool synchronized)
{
    return RowsThenPlacements(frame.text, frame.placements, synchronized);
}

SynchronizedOutputAnswer ToSynchronizedOutputAnswer(tui::DecModeStatus status) noexcept
{
    // A switch with no default, so a status endo adds is a compiler warning here rather than a value
    // quietly read as one of these.
    switch (status)
    {
        case tui::DecModeStatus::Set:
        case tui::DecModeStatus::Reset:
            return SynchronizedOutputAnswer::Supported;
        case tui::DecModeStatus::NotRecognized:
        case tui::DecModeStatus::PermanentlySet:
        case tui::DecModeStatus::PermanentlyReset:
            return SynchronizedOutputAnswer::NotSupported;
        case tui::DecModeStatus::NoReply:
            return SynchronizedOutputAnswer::NoReply;
        case tui::DecModeStatus::NotAsked:
            return SynchronizedOutputAnswer::NotAsked;
    }
    return SynchronizedOutputAnswer::NotAsked;
}

std::string KeyBytes(tui::KeyEvent const& key)
{
    auto bytes = std::string {};
    if (Has(key.modifiers, tui::Modifier::Ctrl) && key.codepoint >= U'@' && key.codepoint <= U'z')
    {
        bytes.push_back(static_cast<char>(static_cast<std::uint32_t>(key.codepoint) & 0x1FU));
        return bytes;
    }

    if (auto const* row = FindOrNull(NamedKeys, key.key, &NamedKeyRow::key))
        bytes = row->bytes;
    else if (key.codepoint != 0)
        bytes = EncodeUtf8(key.codepoint);

    if (!bytes.empty() && Has(key.modifiers, tui::Modifier::Alt))
        bytes.insert(bytes.begin(), '\x1b');
    return bytes;
}

std::vector<DashboardEvent> ToDashboardEvents(tui::runtime::WaitOutcome const& outcome,
                                              std::optional<CellPixelSize> cellPixels)
{
    auto events = std::vector<DashboardEvent> {};
    for (auto const& input: outcome.events)
    {
        if (auto const* key = std::get_if<tui::KeyEvent>(&input))
        {
            if (auto bytes = KeyBytes(*key); !bytes.empty())
                events.push_back(DashboardEvent { .kind = DashboardEventKind::Key, .keys = std::move(bytes) });
        }
        else if (auto const* resize = std::get_if<tui::ResizeEvent>(&input))
        {
            events.push_back(DashboardEvent { .kind = DashboardEventKind::Resize,
                                              .columns = resize->columns,
                                              .rows = resize->rows,
                                              .cellPixels = cellPixels });
        }
    }
    if (outcome.interrupted)
        events.push_back(DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the terminal's input closed" });
    return events;
}

std::optional<CellPixelSize> ToCellPixelSize(std::expected<std::pair<int, int>, tui::QueryUnanswered> const& answer) noexcept
{
    if (!answer.has_value())
        return std::nullopt;
    auto const [width, height] = *answer;
    if (width <= 0 || height <= 0)
        return std::nullopt;
    return CellPixelSize { .width = static_cast<std::size_t>(width), .height = static_cast<std::size_t>(height) };
}

SixelAnswer ToSixelAnswer(std::expected<tui::DeviceAttributesReport, tui::QueryUnanswered> const& attributes) noexcept
{
    if (attributes.has_value())
        return tui::advertisesSixel(*attributes) ? SixelAnswer::Advertised : SixelAnswer::NotAdvertised;
    // A switch with no default, so a third reason endo adds is a compiler warning here rather than
    // a value quietly read as one of these two.
    switch (attributes.error())
    {
        case tui::QueryUnanswered::NoReply:
            return SixelAnswer::NoReply;
        case tui::QueryUnanswered::NotAsked:
            return SixelAnswer::NotAsked;
    }
    return SixelAnswer::NotAsked;
}

std::unique_ptr<IDashboardEventSource> MakeTerminalEventStream(TerminalStreamParts parts)
{
    return std::make_unique<TerminalEventStream>(std::move(parts));
}

Task<std::expected<StartedTerminal, std::string>> StartTerminalDevice(std::unique_ptr<ITerminalDevice> device,
                                                                      IExecutor* pool,
                                                                      IExecutor* resumeOn)
{
    // Declared first, so it is the last local destroyed: every way out of this coroutine -- a refused
    // acquisition, an exception after raw mode was entered, the task being destroyed between the
    // hops -- restores the device, unless it was handed on inside the result.
    auto guard = RestoreUnlessKept { device.get() };
    auto capabilities = std::expected<TerminalCapabilities, std::string> {};

    // The acquisition and both queries BLOCK, so they run on the pool; the record is used only
    // after the hop back. The screen is entered last of the terminal-facing steps, once the queries
    // have their answers, and the encoding -- which asks the environment, not the terminal -- after it.
    co_await ResumeOn { *pool };
    try
    {
        if (auto acquired = device->Acquire(); !acquired)
            capabilities = std::unexpected(std::move(acquired).error());
        else
        {
            auto record = TerminalCapabilities {};
            record.sixel = device->AskSixel();
            record.synchronizedOutput = device->AskSynchronizedOutput();
            record.cellPixels = device->AskCellPixels();
            auto const& enter = ScreenBytes().enter;
            guard.ScreenEntering();
            device->Write(enter);
            record.encoding = device->Encoding();
            capabilities = record;
        }
    }
    catch (std::exception const& failure)
    {
        capabilities = std::unexpected(std::string { "the terminal failed while being started: " } + failure.what());
    }
    co_await ResumeOn { *resumeOn };

    if (!capabilities.has_value())
        co_return std::unexpected(std::move(capabilities).error());

    // Everything that can throw runs while the guard still leaves the screen and restores: sharing
    // the device, the stream, the restore handle, the presenter, then the events' allocation. The
    // events' constructor moves three pointers and cannot, so the device is handed over and the guard
    // stood down with nothing between them. The guard's pointer names the device object, never an
    // owner, so it stays valid across every move.
    auto shared = std::shared_ptr<ITerminalDevice> { std::move(device) };
    auto parts = TerminalStreamParts {
        .source = &shared->Events(),
        .pool = pool,
        .resumeOn = resumeOn,
        .wake = [raw = shared.get()] { raw->Wake(); },
        .columns = shared->Columns(),
        .rows = shared->Rows(),
        .cellPixels = capabilities->cellPixels,
        .rereadCellPixels = {},
    };
    // Asked again only of a terminal that answered once: one that did not is not drawn on the Sixel
    // rung, so a size would change nothing, and every resize would cost it the query's deadline.
    if (capabilities->cellPixels.has_value())
        parts.rereadCellPixels = [raw = shared.get()] {
            return raw->RereadCellPixels();
        };
    auto stream = MakeTerminalEventStream(std::move(parts));
    auto restore = std::make_shared<TerminalRestoreHandle>(shared.get(), RestoreNowLeading());
    auto frames = std::make_unique<TerminalFramePresenter>(shared, PresentsSynchronized(*capabilities));
    auto events = std::make_unique<StartedTerminalEvents>(shared, std::move(stream), restore);
    guard.Keep();
    co_return StartedTerminal {
        .capabilities = *capabilities,
        .events = std::move(events),
        .restore = std::move(restore),
        .frames = std::move(frames),
    };
}

} // namespace FastCache::Cli
