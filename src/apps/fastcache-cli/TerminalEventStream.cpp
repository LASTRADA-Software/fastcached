// SPDX-License-Identifier: Apache-2.0
#include "TerminalEventStream.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <core/Ranges.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/TerminalOutput.hpp>

namespace FastCache::Cli
{

namespace
{
    /// A named key and the bytes a terminal sends for it.
    struct NamedKeyRow
    {
        core::tui::KeyCode key;
        std::string_view bytes;
    };

    /// The named keys that have a byte spelling, as a table so another key is a row.
    ///
    /// The spellings are the ordinary xterm ones. They only have to be RIGHT for the keys the
    /// dashboard acts on, and `ESC` is the one of those here; the rest are spelled so that a
    /// consumer comparing bytes sees what an operator pressed rather than nothing.
    constexpr auto NamedKeys = std::to_array<NamedKeyRow>({
        { .key = core::tui::KeyCode::Escape, .bytes = "\x1b" },
        { .key = core::tui::KeyCode::Enter, .bytes = "\r" },
        { .key = core::tui::KeyCode::Tab, .bytes = "\t" },
        { .key = core::tui::KeyCode::Backspace, .bytes = "\x7f" },
        { .key = core::tui::KeyCode::Up, .bytes = "\x1b[A" },
        { .key = core::tui::KeyCode::Down, .bytes = "\x1b[B" },
        { .key = core::tui::KeyCode::Right, .bytes = "\x1b[C" },
        { .key = core::tui::KeyCode::Left, .bytes = "\x1b[D" },
        { .key = core::tui::KeyCode::Home, .bytes = "\x1b[H" },
        { .key = core::tui::KeyCode::End, .bytes = "\x1b[F" },
        { .key = core::tui::KeyCode::Insert, .bytes = "\x1b[2~" },
        { .key = core::tui::KeyCode::Delete, .bytes = "\x1b[3~" },
        { .key = core::tui::KeyCode::PageUp, .bytes = "\x1b[5~" },
        { .key = core::tui::KeyCode::PageDown, .bytes = "\x1b[6~" },
    });

    /// How long a wait lasts right after input arrived.
    ///
    /// core-cpp decides that a lone `ESC` is the Escape key, rather than the start of a sequence,
    /// only when a wait TIMES OUT with nothing after it. A wait that blocks indefinitely never
    /// times out, so the first wait after any input is this short one and the next is unbounded
    /// again. The figure is the one core-cpp's parser documents for this decision.
    constexpr auto EscapeDecisionMs = 50;

    /// Whether @p modifiers includes @p wanted.
    [[nodiscard]] constexpr bool Has(core::tui::Modifier modifiers, core::tui::Modifier wanted) noexcept
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

        [[nodiscard]] core::async::Task<DashboardEvent> Next() override
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
                co_await core::async::ResumeOn { *_pool };
                auto outcome = _source->Wait(timeoutMs);
                // Still on the pool, before the next wait: the query reads its reply from the input
                // this wait just finished reading, and hands back anything else it reads there.
                auto const reread =
                    _rereadCellPixels && std::ranges::any_of(outcome.events, [](core::tui::InputEvent const& input) {
                        return std::holds_alternative<core::tui::ResizeEvent>(input);
                    });
                auto const measured = reread ? _rereadCellPixels() : std::nullopt;
                co_await core::async::ResumeOn { *_resumeOn };

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

        ITerminalInputWait* _source;
        core::async::IExecutor* _pool;
        core::async::IExecutor* _resumeOn;
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
    /// The only presentation bytes spelled in this project. core-cpp writes these solely from inside
    /// `SyncGuard`, directly to a native handle, so unlike the rest of `TerminalScreenBytes` there is
    /// no buffered call to capture them from.
    constexpr auto SyncBegin = std::string_view { "\x1b[?2026h" };
    constexpr auto SyncEnd = std::string_view { "\x1b[?2026l" };

    /// `CAN`: abandons an escape sequence a frame was halfway through, so what follows is not read
    /// as its tail.
    constexpr auto Cancel = std::string_view { "\x18" };

    /// A core-cpp `TerminalOutput` whose destination is a string, so a sequence core-cpp spells can be read
    /// back as bytes rather than restated.
    class CapturedOutput final: public core::tui::TerminalOutput
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

    /// The frames of a started terminal: one `Write` per frame, spelled by `FrameBytes`.
    ///
    /// Shares the device with the events rather than borrowing it, so a frame presented after the
    /// events are gone -- which the contract forbids -- reaches a torn-down terminal rather than freed
    /// memory.
    class TerminalFramePresenter final: public IFrameSink
    {
      public:
        TerminalFramePresenter(std::shared_ptr<ITerminalDevice> device, TerminalCapabilities capabilities) noexcept:
            _device { std::move(device) },
            _capabilities { capabilities }
        {
        }

        void PresentPlaced(DashboardFrame const& frame) override
        {
            _device->Write(FrameBytes(frame, _capabilities));
        }

      private:
        std::shared_ptr<ITerminalDevice> _device;
        TerminalCapabilities _capabilities;
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

        [[nodiscard]] core::async::Task<DashboardEvent> Next() override
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
        auto const spell = [](void (*step)(core::tui::TerminalOutput&)) {
            auto output = CapturedOutput {};
            step(output);
            return output.Take();
        };
        return TerminalScreenBytes {
            .enter = spell([](core::tui::TerminalOutput& output) {
                output.enterAltScreen();
                output.hideCursor();
            }),
            .leave = spell([](core::tui::TerminalOutput& output) {
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
    /// core-cpp's style for a palette row.
    /// @param row The row.
    /// @return The style.
    [[nodiscard]] core::tui::Style StyleOf(TonePaletteRow const& row)
    {
        auto style = core::tui::Style {};
        if (row.colour.has_value())
            style.fg = *row.colour;
        style.bold = row.bold;
        style.dim = row.dim;
        style.inverse = row.inverse;
        return style;
    }

    /// How a frame's dressed runs are written.
    struct Dressing
    {
        std::span<FrameSpan const> spans;   ///< Every span of the frame, in any order.
        TerminalCapabilities const* record; ///< Whose palette answer applies; null writes every run plain.
    };

    /// Write one row: its dressed runs through core-cpp's styled text, the rest as it stands.
    ///
    /// A run that does not lie inside the row, or that overlaps the one before it, is written plain, and so
    /// is every run where the record allows no colour: a span dresses bytes, and never adds, drops or
    /// reorders one.
    /// @param output Where the row goes.
    /// @param line The row's text.
    /// @param dressing The spans and the record.
    /// @param row The row's 1-based number.
    void WriteRow(CapturedOutput& output, std::string_view line, Dressing const& dressing, std::size_t row)
    {
        auto mine = std::vector<FrameSpan> {};
        if (dressing.record != nullptr)
            std::ranges::copy_if(
                dressing.spans, std::back_inserter(mine), [row](FrameSpan const& span) { return span.row == row; });
        std::ranges::sort(mine, {}, &FrameSpan::byte);

        auto written = std::size_t { 0 };
        for (auto const& span: mine)
        {
            auto const* const palette = PaletteFor(span.tone, *dressing.record);
            if (palette == nullptr || span.byte < written || span.length > line.size()
                || span.byte > line.size() - span.length)
                continue;
            output.writeRaw(line.substr(written, span.byte - written));
            output.writeText(line.substr(span.byte, span.length), StyleOf(*palette));
            written = span.byte + span.length;
        }
        output.writeRaw(line.substr(written));
    }

    /// `FrameBytes`, with or without images: the rows, then @p placements, in one bracket.
    [[nodiscard]] std::string RowsThenPlacements(std::string_view frame,
                                                 std::span<FramePlacement const> placements,
                                                 Dressing const& dressing,
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
            WriteRow(output, std::string_view { line.begin(), line.end() }, dressing, static_cast<std::size_t>(row));
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
    return RowsThenPlacements(frame, {}, Dressing { .spans = {}, .record = nullptr }, synchronized);
}

std::string FrameBytes(DashboardFrame const& frame, bool synchronized)
{
    return RowsThenPlacements(frame.text, frame.placements, Dressing { .spans = {}, .record = nullptr }, synchronized);
}

std::string FrameBytes(DashboardFrame const& frame, TerminalCapabilities const& capabilities)
{
    return RowsThenPlacements(frame.text,
                              frame.placements,
                              Dressing { .spans = frame.spans, .record = &capabilities },
                              PresentsSynchronized(capabilities));
}

SynchronizedOutputAnswer ToSynchronizedOutputAnswer(core::tui::DecModeStatus status) noexcept
{
    // A switch with no default, so a status core-cpp adds is a compiler warning here rather than a value
    // quietly read as one of these.
    switch (status)
    {
        case core::tui::DecModeStatus::Set:
        case core::tui::DecModeStatus::Reset:
            return SynchronizedOutputAnswer::Supported;
        case core::tui::DecModeStatus::NotRecognized:
        case core::tui::DecModeStatus::PermanentlySet:
        case core::tui::DecModeStatus::PermanentlyReset:
            return SynchronizedOutputAnswer::NotSupported;
        case core::tui::DecModeStatus::NoReply:
            return SynchronizedOutputAnswer::NoReply;
        case core::tui::DecModeStatus::NotAsked:
            return SynchronizedOutputAnswer::NotAsked;
    }
    return SynchronizedOutputAnswer::NotAsked;
}

std::string KeyBytes(core::tui::KeyEvent const& key)
{
    auto bytes = std::string {};
    if (Has(key.modifiers, core::tui::Modifier::Ctrl) && key.codepoint >= U'@' && key.codepoint <= U'z')
    {
        bytes.push_back(static_cast<char>(static_cast<std::uint32_t>(key.codepoint) & 0x1FU));
        return bytes;
    }

    if (auto const* row = core::findOrNull(NamedKeys, key.key, &NamedKeyRow::key))
        bytes = row->bytes;
    else if (key.codepoint != 0)
        bytes = EncodeUtf8(key.codepoint);

    if (!bytes.empty() && Has(key.modifiers, core::tui::Modifier::Alt))
        bytes.insert(bytes.begin(), '\x1b');
    return bytes;
}

namespace
{
    /// `ITerminalInputWait` over an `IoBackend`: what `MakeTerminalInputWait` returns.
    ///
    /// Every member but the backend's wake is touched only by the thread calling `Wait`, and the
    /// readiness callbacks run inside that call, on that thread -- so the flags they set are plain.
    class BackendTerminalInputWait final: public ITerminalInputWait
    {
      public:
        /// Which of the input source's handles a watch is for.
        enum class Channel : std::uint8_t
        {
            Input,
            Resize,
        };

        BackendTerminalInputWait(core::tui::runtime::InputSource& input,
                                 std::unique_ptr<core::net::IoBackend> backend) noexcept:
            _input { input },
            _backend { std::move(backend) }
        {
        }

        BackendTerminalInputWait(BackendTerminalInputWait const&) = delete;
        BackendTerminalInputWait(BackendTerminalInputWait&&) = delete;
        BackendTerminalInputWait& operator=(BackendTerminalInputWait const&) = delete;
        BackendTerminalInputWait& operator=(BackendTerminalInputWait&&) = delete;

        ~BackendTerminalInputWait() override
        {
            for (auto& watch: _watches)
                if (watch.attached)
                    _backend->detach(watch.handler);
        }

        /// Attach @p handle to the backend as @p channel, watched for reading.
        /// @return Nothing, or why the backend refused the handle.
        [[nodiscard]] std::expected<void, std::string> Watch(Channel channel, core::platform::NativeHandle handle)
        {
            auto& watch = _watches.at(static_cast<std::size_t>(channel));
            watch.handler.handle = handle;
            watch.handler.owner = &watch;
            if (auto attached = _backend->attach(watch.handler); !attached)
                return std::unexpected(attached.error().toString());
            watch.attached = true;
            if (auto armed = _backend->setInterest(watch.handler, core::net::Interest::Read); !armed)
                return std::unexpected(armed.error().toString());
            return {};
        }

        [[nodiscard]] TerminalWaitOutcome Wait(int timeoutMs) override
        {
            // Input a query read and did not consume arrived BEFORE anything still on the handle,
            // so it is delivered first and without waiting.
            auto outcome = TerminalWaitOutcome { .events = _input.takePending() };
            if (outcome.events.empty())
                outcome = WaitForReadiness(timeoutMs);
            std::ignore = _input.consumeReports(outcome.events);
            return outcome;
        }

        void Wake() noexcept override
        {
            _backend->wake();
        }

      private:
        /// One attached handle, and what the backend last reported for it.
        struct Watched
        {
            core::net::ReadinessHandler handler;
            bool attached { false };
            bool readable { false };
            bool failed { false };
        };

        /// The watch a readiness callback is about: each handler's owner is its own `Watched`.
        [[nodiscard]] static Watched& Of(core::net::ReadinessHandler& handler) noexcept
        {
            return *static_cast<Watched*>(handler.owner);
        }

        [[nodiscard]] TerminalWaitOutcome WaitForReadiness(int timeoutMs)
        {
            for (auto& watch: _watches)
            {
                watch.readable = false;
                watch.failed = false;
            }
            auto const timeout =
                timeoutMs < 0 ? std::optional<core::platform::SteadyDuration> {}
                              : std::optional<core::platform::SteadyDuration> { std::chrono::milliseconds { timeoutMs } };
            auto const waited = _backend->wait(timeout);

            auto const& input = _watches.at(static_cast<std::size_t>(Channel::Input));
            auto const& resize = _watches.at(static_cast<std::size_t>(Channel::Resize));
            auto outcome = TerminalWaitOutcome {};
            // A hang-up the backend reported with nothing readable beside it: nothing will ever be
            // read here again, and answering it as readable-and-empty would wake every wait at once.
            if (input.failed && !input.readable)
            {
                outcome.inputClosed = true;
                return outcome;
            }
            if (resize.readable)
                if (auto resized = _input.readResize())
                    outcome.events.push_back(std::move(*resized));
            if (input.readable)
                std::ranges::move(_input.readReady(), std::back_inserter(outcome.events));
            // A bounded wait that ended with nothing ready is the parser's cue: a lone ESC with no
            // continuation after it is the Escape key.
            if (waited.dispatched == 0 && timeoutMs >= 0)
                outcome.events = _input.flushPartial();
            return outcome;
        }

        static void OnReadable(core::net::ReadinessHandler& handler) noexcept
        {
            Of(handler).readable = true;
        }

        static void OnFailed(core::net::ReadinessHandler& handler) noexcept
        {
            Of(handler).failed = true;
        }

        core::tui::runtime::InputSource& _input;
        std::unique_ptr<core::net::IoBackend> _backend;
        /// Indexed by `Channel`.
        std::array<Watched, 2> _watches {
            Watched { .handler = { .onReadable = &OnReadable, .onError = &OnFailed } },
            Watched { .handler = { .onReadable = &OnReadable, .onError = &OnFailed } },
        };
    };
} // namespace

std::expected<std::unique_ptr<ITerminalInputWait>, std::string> MakeTerminalInputWait(
    core::tui::runtime::InputSource& input, std::unique_ptr<core::net::IoBackend> backend)
{
    using Channel = BackendTerminalInputWait::Channel;
    auto wait = std::make_unique<BackendTerminalInputWait>(input, std::move(backend));
    if (auto watched = wait->Watch(Channel::Input, input.inputHandle()); !watched)
        return std::unexpected("the terminal's input: " + watched.error());
    // A source with no resize channel -- a Windows console reports a resize as an input record --
    // answers an invalid handle, and there is nothing to attach.
    if (auto const resize = input.resizeHandle(); resize != core::platform::InvalidHandle)
        if (auto watched = wait->Watch(Channel::Resize, resize); !watched)
            return std::unexpected("the terminal's resize notification: " + watched.error());
    return wait;
}

std::vector<DashboardEvent> ToDashboardEvents(TerminalWaitOutcome const& outcome, std::optional<CellPixelSize> cellPixels)
{
    auto events = std::vector<DashboardEvent> {};
    for (auto const& input: outcome.events)
    {
        if (auto const* key = std::get_if<core::tui::KeyEvent>(&input))
        {
            if (auto bytes = KeyBytes(*key); !bytes.empty())
                events.push_back(DashboardEvent { .kind = DashboardEventKind::Key, .keys = std::move(bytes) });
        }
        else if (auto const* resize = std::get_if<core::tui::ResizeEvent>(&input))
        {
            events.push_back(DashboardEvent { .kind = DashboardEventKind::Resize,
                                              .columns = resize->columns,
                                              .rows = resize->rows,
                                              .cellPixels = cellPixels });
        }
    }
    if (outcome.inputClosed)
        events.push_back(DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the terminal's input closed" });
    return events;
}

std::optional<CellPixelSize> ToCellPixelSize(
    std::expected<std::pair<int, int>, core::tui::QueryUnanswered> const& answer) noexcept
{
    if (!answer.has_value())
        return std::nullopt;
    auto const [width, height] = *answer;
    if (width <= 0 || height <= 0)
        return std::nullopt;
    return CellPixelSize { .width = static_cast<std::size_t>(width), .height = static_cast<std::size_t>(height) };
}

SixelAnswer ToSixelAnswer(
    std::expected<core::tui::DeviceAttributesReport, core::tui::QueryUnanswered> const& attributes) noexcept
{
    if (attributes.has_value())
        return core::tui::advertisesSixel(*attributes) ? SixelAnswer::Advertised : SixelAnswer::NotAdvertised;
    // A switch with no default, so a third reason core-cpp adds is a compiler warning here rather than
    // a value quietly read as one of these two.
    switch (attributes.error())
    {
        case core::tui::QueryUnanswered::NoReply:
            return SixelAnswer::NoReply;
        case core::tui::QueryUnanswered::NotAsked:
            return SixelAnswer::NotAsked;
    }
    return SixelAnswer::NotAsked;
}

std::unique_ptr<IDashboardEventSource> MakeTerminalEventStream(TerminalStreamParts parts)
{
    return std::make_unique<TerminalEventStream>(std::move(parts));
}

core::async::Task<std::expected<StartedTerminal, std::string>> StartTerminalDevice(std::unique_ptr<ITerminalDevice> device,
                                                                                   core::async::IExecutor* pool,
                                                                                   core::async::IExecutor* resumeOn)
{
    // Declared first, so it is the last local destroyed: every way out of this coroutine -- a refused
    // acquisition, an exception after raw mode was entered, the task being destroyed between the
    // hops -- restores the device, unless it was handed on inside the result.
    auto guard = RestoreUnlessKept { device.get() };
    auto capabilities = std::expected<TerminalCapabilities, std::string> {};

    // The acquisition and both queries BLOCK, so they run on the pool; the record is used only
    // after the hop back. The screen is entered last of the terminal-facing steps, once the queries
    // have their answers, and the encoding -- which asks the environment, not the terminal -- after it.
    co_await core::async::ResumeOn { *pool };
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
            record.colour = device->AskColour();
            capabilities = record;
        }
    }
    catch (std::exception const& failure)
    {
        capabilities = std::unexpected(std::string { "the terminal failed while being started: " } + failure.what());
    }
    co_await core::async::ResumeOn { *resumeOn };

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
    auto frames = std::make_unique<TerminalFramePresenter>(shared, *capabilities);
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
