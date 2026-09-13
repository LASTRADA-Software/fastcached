// SPDX-License-Identifier: Apache-2.0
#include "TerminalEventStream.hpp"
#include "TerminalEvents.hpp"

#include <FastCache/Platform/Terminal.hpp>

#include <array>
#include <atomic>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <platform/Wakeup.hpp>
#include <tui/Terminal.hpp>
#include <tui/TerminalProtocols.hpp>
#include <tui/runtime/TerminalEventSource.hpp>

namespace FastCache::Cli
{

namespace
{
    /// What `RestoreNow` writes before putting the modes back, in this order.
    ///
    /// First `CAN`, which abandons an escape sequence the dashboard was halfway through writing, so
    /// the rest is not read as its tail. Then the input protocols endo's `Terminal` pushes -- the
    /// union of what its POSIX and Windows arms enable, since switching off a mode that was never on
    /// is harmless -- spelled through endo's own constants rather than restated. Then what a
    /// renderer switches on: synchronized output, a hidden cursor, the alternate screen.
    ///
    /// **Leaving the alternate screen is the one that is not free.** `CSI ? 1049 l` also restores
    /// the cursor saved on entry, so on a terminal that never entered the alternate screen it can
    /// move the cursor. This is for an exit, where a moved cursor beats a shell on the wrong screen.
    constexpr auto RestoreNowSequences = std::to_array<std::string_view>({
        "\x18",
        tui::protocols::DisableFocusTracking,
        tui::protocols::DisableColorSchemeNotify,
        tui::protocols::DisableBracketedPaste,
        tui::protocols::DisableAnyMotionTracking,
        tui::protocols::DisablePassiveMouseTracking,
        tui::protocols::DisableWin32InputMode,
        tui::protocols::DisableCsiU,
        "\x1b[?2026l",
        "\x1b[?25h",
        "\x1b[?1049l",
    });

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
            // BEFORE `initialize()` changes anything: these are the modes `RestoreNow` puts back.
            _saved = SavedTerminalModes::Capture();
            for (auto const sequence: RestoreNowSequences)
                _resets.append(sequence);
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
            // After a `RestoreNow`, endo's teardown below sends its input resets a second time. Every
            // one of them lands where the terminal already is except the keyboard protocol's POP, which
            // would pop an entry the dashboard never pushed -- a shell's own. So the entry `RestoreNow`
            // popped is pushed back first, for this pop to take. Once: the exchange makes the device's
            // own destructor, calling this again after endo has shut down, push nothing.
            if (_restoredNow.exchange(false, std::memory_order_acq_rel))
            {
                _terminal.output().writeRaw(tui::protocols::EnableCsiU);
                _terminal.output().flush();
            }
            // `Terminal::shutdown` undoes a completed `initialize()` (protocols, raw mode, the SIGWINCH
            // handler) and does nothing when it never completed. The input's own `shutdown` then covers
            // an `initialize()` that got as far as raw mode and no further. Both are idempotent.
            _terminal.shutdown();
            _terminal.input().shutdown();
        }

        void RestoreNow() noexcept override
        {
            // Not `shutdown()`: that closes the resize pipe the parked `poll` is waiting on and writes
            // endo's state unlocked. The saved modes are this device's own, written once in `Acquire`
            // on the pool and read only after the start handed the device back.
            //
            // NO LOCK AGAINST A FRAME BEING WRITTEN, deliberately. If the reactor is halfway through
            // writing a frame to the same output handle, these resets can splice into its bytes and a
            // stray fragment of frame may land on screen. The MODES are restored regardless, since
            // those are syscalls rather than bytes, and the leading `CAN` abandons a sequence the
            // frame left open. A lock shared with the frame writer would be worse: the writer this
            // exit exists for may be the one that is stuck, and it would hold that lock forever.
            if (!_saved.has_value())
                return;
            _saved->Apply(_resets);
            _restoredNow.store(true, std::memory_order_release);
        }

      private:
        tui::Terminal _terminal;
        endo::platform::Wakeup _wakeup;
        tui::runtime::TerminalEventSource _source;
        std::optional<SavedTerminalModes> _saved;
        std::string _resets;
        std::atomic<bool> _restoredNow { false };
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
