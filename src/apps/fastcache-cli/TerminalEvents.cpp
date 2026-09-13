// SPDX-License-Identifier: Apache-2.0
#include "TerminalEventStream.hpp"
#include "TerminalEvents.hpp"

#include <FastCache/Platform/Terminal.hpp>

#include <exception>
#include <utility>

#include <platform/Wakeup.hpp>
#include <tui/Terminal.hpp>
#include <tui/runtime/TerminalEventSource.hpp>

namespace FastCache::Cli
{

namespace
{
    /// endo's `Terminal`, the wakeup `Close()` signals, and the event source waiting on both.
    ///
    /// Member ORDER is the lifetime: the source borrows the terminal and the wakeup, so it is
    /// declared after them and destroyed before them.
    ///
    /// Constructing it touches no terminal. `tui::Terminal`'s constructor allocates and nothing more;
    /// raw mode, protocols and queries all happen in `initialize()`, which only `Acquire()` calls.
    class EndoTerminalDevice final: public ITerminalDevice
    {
      public:
        EndoTerminalDevice():
            _source { _terminal, &_wakeup }
        {
        }

        EndoTerminalDevice(EndoTerminalDevice const&) = delete;
        EndoTerminalDevice(EndoTerminalDevice&&) = delete;
        EndoTerminalDevice& operator=(EndoTerminalDevice const&) = delete;
        EndoTerminalDevice& operator=(EndoTerminalDevice&&) = delete;

        ~EndoTerminalDevice() override
        {
            Restore();
        }

        [[nodiscard]] std::expected<void, std::string> Acquire() override
        {
            if (!StandardStreamsAreInteractive())
                return std::unexpected(std::string { "standard input and output are not both an interactive terminal" });
            if (auto opened = _terminal.initialize(); !opened)
                return std::unexpected("the terminal could not be opened: " + opened.error());
            return {};
        }

        [[nodiscard]] SixelAnswer AskSixel() override
        {
            return ToSixelAnswer(_terminal.queryDeviceAttributes());
        }

        [[nodiscard]] TerminalTextEncoding Encoding() override
        {
            return DetectTerminalTextEncoding();
        }

        [[nodiscard]] int Columns() const override
        {
            return _terminal.columns();
        }

        [[nodiscard]] int Rows() const override
        {
            return _terminal.rows();
        }

        [[nodiscard]] tui::runtime::EventSource& Events() override
        {
            return _source;
        }

        void Wake() override
        {
            _wakeup.signal();
        }

        void Restore() noexcept override
        {
            // `Terminal::shutdown` undoes a completed `initialize()` (protocols, raw mode, the SIGWINCH
            // handler) and does nothing when it never completed. The input's own `shutdown` then covers
            // an `initialize()` that got as far as raw mode and no further. Both are idempotent.
            _terminal.shutdown();
            _terminal.input().shutdown();
        }

      private:
        tui::Terminal _terminal;
        endo::platform::Wakeup _wakeup;
        tui::runtime::TerminalEventSource _source;
    };
} // namespace

struct UnstartedTerminal::Parts
{
    std::unique_ptr<ITerminalDevice> device;
    IExecutor* pool { nullptr };
    IExecutor* resumeOn { nullptr };
};

/// The one door to an `UnstartedTerminal`'s parts, so the type offers its holders nothing to call.
struct UnstartedTerminalAccess
{
    [[nodiscard]] static UnstartedTerminal::Parts& Of(UnstartedTerminal& terminal) noexcept
    {
        return *terminal._parts;
    }
};

UnstartedTerminal::UnstartedTerminal(std::unique_ptr<Parts> parts) noexcept:
    _parts { std::move(parts) }
{
}

UnstartedTerminal::~UnstartedTerminal() = default;

std::expected<std::unique_ptr<UnstartedTerminal>, std::string> MakeTerminalEvents(IExecutor* pool, IExecutor* resumeOn)
{
    try
    {
        auto parts = std::make_unique<UnstartedTerminal::Parts>();
        parts->device = std::make_unique<EndoTerminalDevice>();
        parts->pool = pool;
        parts->resumeOn = resumeOn;
        return std::make_unique<UnstartedTerminal>(std::move(parts));
    }
    catch (std::exception const& failure)
    {
        // endo's `Wakeup` throws when the OS will not hand out an eventfd, pipe or event.
        return std::unexpected(std::string { "the terminal's wakeup could not be created: " } + failure.what());
    }
}

Task<std::expected<StartedTerminal, std::string>> StartTerminal(std::unique_ptr<UnstartedTerminal> terminal)
{
    auto& parts = UnstartedTerminalAccess::Of(*terminal);
    co_return co_await StartTerminalDevice(std::move(parts.device), parts.pool, parts.resumeOn);
}

} // namespace FastCache::Cli
