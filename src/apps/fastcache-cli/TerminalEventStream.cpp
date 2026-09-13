// SPDX-License-Identifier: Apache-2.0
#include "TerminalEventStream.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <string_view>
#include <utility>
#include <variant>

#include <tui/KeyCode.hpp>
#include <tui/Modifier.hpp>

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
            _wake { std::move(parts.wake) }
        {
            _ready.push_back(
                DashboardEvent { .kind = DashboardEventKind::Resize, .columns = parts.columns, .rows = parts.rows });
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
                co_await ResumeOn { *_resumeOn };

                // A short wait that came back empty has had its chance to settle a lone ESC.
                _inputJustArrived = !(timeoutMs >= 0 && outcome.events.empty());
                for (auto& event: ToDashboardEvents(outcome))
                    _ready.push_back(std::move(event));
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
        tui::runtime::EventSource* _source;
        IExecutor* _pool;
        IExecutor* _resumeOn;
        std::function<void()> _wake;
        std::deque<DashboardEvent> _ready;
        std::string _ended;
        std::atomic<bool> _closed { false };
        bool _inputJustArrived { false };
    };
} // namespace

namespace
{
    /// The events of a started terminal, owning the device they are read from.
    ///
    /// Member ORDER is the lifetime: the stream borrows the device's event source, so the device is
    /// declared first and destroyed last, and the destructor restores it once nothing reads it.
    ///
    /// Constructing one cannot throw: the stream is built BEFORE the device is handed over, so no
    /// failure can leave the device owned by a half-built object whose destructor never runs.
    class StartedTerminalEvents final: public IDashboardEventSource
    {
      public:
        StartedTerminalEvents(std::unique_ptr<ITerminalDevice> device,
                              std::unique_ptr<IDashboardEventSource> stream) noexcept:
            _device { std::move(device) },
            _stream { std::move(stream) }
        {
        }

        StartedTerminalEvents(StartedTerminalEvents const&) = delete;
        StartedTerminalEvents(StartedTerminalEvents&&) = delete;
        StartedTerminalEvents& operator=(StartedTerminalEvents const&) = delete;
        StartedTerminalEvents& operator=(StartedTerminalEvents&&) = delete;

        ~StartedTerminalEvents() override
        {
            _stream.reset();
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
        std::unique_ptr<ITerminalDevice> _device;
        std::unique_ptr<IDashboardEventSource> _stream;
    };

    /// Restores a device when a start leaves by any path but success.
    ///
    /// A guard rather than a restore call on each failure path, because the path that matters most
    /// is the one nobody writes a call on: an exception thrown after raw mode was already entered.
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
            if (!_kept)
                _device->Restore();
        }

        /// Ownership moved on, and whoever holds it restores the device now.
        void Keep() noexcept
        {
            _kept = true;
        }

      private:
        ITerminalDevice* _device;
        bool _kept { false };
    };
} // namespace

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

std::vector<DashboardEvent> ToDashboardEvents(tui::runtime::WaitOutcome const& outcome)
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
            events.push_back(
                DashboardEvent { .kind = DashboardEventKind::Resize, .columns = resize->columns, .rows = resize->rows });
        }
    }
    if (outcome.interrupted)
        events.push_back(DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the terminal's input closed" });
    return events;
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
    // after the hop back.
    co_await ResumeOn { *pool };
    try
    {
        if (auto acquired = device->Acquire(); !acquired)
            capabilities = std::unexpected(std::move(acquired).error());
        else
            capabilities = TerminalCapabilities { .sixel = device->AskSixel(), .encoding = device->Encoding() };
    }
    catch (std::exception const& failure)
    {
        capabilities = std::unexpected(std::string { "the terminal failed while being started: " } + failure.what());
    }
    co_await ResumeOn { *resumeOn };

    if (!capabilities.has_value())
        co_return std::unexpected(std::move(capabilities).error());

    // Everything that can throw runs while the guard still restores: the stream, then the allocation.
    // The constructor moves two pointers and cannot, so the device is handed over and the guard stood
    // down with nothing between them. The guard's pointer names the device object, never the owner,
    // so it stays valid across the move.
    auto stream = MakeTerminalEventStream(TerminalStreamParts {
        .source = &device->Events(),
        .pool = pool,
        .resumeOn = resumeOn,
        .wake = [raw = device.get()] { raw->Wake(); },
        .columns = device->Columns(),
        .rows = device->Rows(),
    });
    auto events = std::make_unique<StartedTerminalEvents>(std::move(device), std::move(stream));
    guard.Keep();
    co_return StartedTerminal { .capabilities = *capabilities, .events = std::move(events) };
}

} // namespace FastCache::Cli
