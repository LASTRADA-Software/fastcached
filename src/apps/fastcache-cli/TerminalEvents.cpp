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

#include <core/net/IoBackend.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalProtocols.hpp>
#include <core/tui/runtime/TerminalInputSource.hpp>

namespace FastCache::Cli
{

namespace
{
    /// The input's resets `RestoreNow` writes after the screen's, before putting the modes back.
    ///
    /// The input protocols core-cpp's `Terminal` pushes -- the union of what its POSIX and Windows arms
    /// enable, since switching off a mode that was never on is harmless -- spelled through core-cpp's own
    /// constants rather than restated. What the SCREEN switched on (synchronized output, the cursor,
    /// the alternate screen) is not here: the restore handle composes those, because it knows whether
    /// the screen is still entered, and passes them first.
    constexpr auto InputResetSequences = std::to_array<std::string_view>({
        core::tui::protocols::DisableFocusTracking,
        core::tui::protocols::DisableColorSchemeNotify,
        core::tui::protocols::DisableBracketedPaste,
        core::tui::protocols::DisableAnyMotionTracking,
        core::tui::protocols::DisablePassiveMouseTracking,
        core::tui::protocols::DisableWin32InputMode,
        core::tui::protocols::DisableCsiU,
    });

    /// The DEC private mode synchronized output is asked about as.
    constexpr auto SynchronizedOutputMode = 2026;

    /// core-cpp's `Terminal`, its handles and decoder as an input source, and the wait over them.
    ///
    /// Member ORDER is the lifetime: the input source borrows the terminal and the wait borrows the
    /// input source, so each is declared after what it borrows and destroyed before it. The wait is
    /// made in `Acquire()`, because it attaches the terminal's handles once and an uninitialized
    /// terminal has none -- on Windows the console handles are opened by `initialize()`.
    ///
    /// Constructing it touches no terminal. `core::tui::Terminal`'s constructor allocates and nothing more;
    /// raw mode, protocols and queries all happen in `initialize()`, which only `Acquire()` calls.
    class TuiTerminalDevice final: public ITerminalDevice
    {
      public:
        explicit TuiTerminalDevice(UsageColor colour):
            _colour { colour }
        {
        }

        TuiTerminalDevice(TuiTerminalDevice const&) = delete;
        TuiTerminalDevice(TuiTerminalDevice&&) = delete;
        TuiTerminalDevice& operator=(TuiTerminalDevice const&) = delete;
        TuiTerminalDevice& operator=(TuiTerminalDevice&&) = delete;

        ~TuiTerminalDevice() override
        {
            Restore();
        }

        [[nodiscard]] std::expected<void, std::string> Acquire() override
        {
            if (!StandardStreamsAreInteractive())
                return std::unexpected(std::string { "standard input and output are not both an interactive terminal" });
            // BEFORE `initialize()` changes anything: these are the modes `RestoreNow` puts back.
            _saved = SavedTerminalModes::Capture();
            for (auto const sequence: InputResetSequences)
                _resets.append(sequence);
            if (auto opened = _terminal.initialize(); !opened)
                return std::unexpected("the terminal could not be opened: " + opened.error());
            auto wait = MakeTerminalInputWait(_input, core::net::makeDefaultBackend());
            if (!wait)
                return std::unexpected("the terminal's input could not be watched: " + wait.error());
            _wait = std::move(wait).value();
            return {};
        }

        [[nodiscard]] SixelAnswer AskSixel() override
        {
            return ToSixelAnswer(_terminal.queryDeviceAttributes());
        }

        [[nodiscard]] SynchronizedOutputAnswer AskSynchronizedOutput() override
        {
            return ToSynchronizedOutputAnswer(_terminal.queryDecMode(SynchronizedOutputMode));
        }

        [[nodiscard]] std::optional<CellPixelSize> AskCellPixels() override
        {
            // Not a second `CSI 16 t`: `initialize()` already asked it, through the same raw-mode read
            // and deadline as every core-cpp query, and keeps the answer -- zero on each side when the
            // terminal did not give one. Asking again would charge a silent terminal a second deadline
            // at every start for the same answer.
            return ToCellPixelSize(std::pair { _terminal.cellPixelWidth(), _terminal.cellPixelHeight() });
        }

        [[nodiscard]] std::optional<CellPixelSize> RereadCellPixels() override
        {
            return ToCellPixelSize(_terminal.queryCellSize());
        }

        [[nodiscard]] TerminalTextEncoding Encoding() override
        {
            return DetectTerminalTextEncoding();
        }

        [[nodiscard]] ColourAnswer AskColour() override
        {
            return _colour == UsageColor::Colored ? ColourAnswer::Supported : ColourAnswer::Suppressed;
        }

        [[nodiscard]] int Columns() const override
        {
            return _terminal.columns();
        }

        [[nodiscard]] int Rows() const override
        {
            return _terminal.rows();
        }

        [[nodiscard]] ITerminalInputWait& Events() override
        {
            return *_wait;
        }

        void Wake() override
        {
            if (_wait)
                _wait->Wake();
        }

        void Write(std::string_view bytes) noexcept override
        {
            _terminal.output().writeRaw(bytes);
            _terminal.output().flush();
        }

        void Restore() noexcept override
        {
            // After a `RestoreNow`, core-cpp's teardown below sends its input resets a second time. Every
            // one of them lands where the terminal already is except the keyboard protocol's POP, which
            // would pop an entry the dashboard never pushed -- a shell's own. So the entry `RestoreNow`
            // popped is pushed back first, for this pop to take. Once: the exchange makes the device's
            // own destructor, calling this again after core-cpp has shut down, push nothing.
            if (_restoredNow.exchange(false, std::memory_order_acq_rel))
            {
                _terminal.output().writeRaw(core::tui::protocols::EnableCsiU);
                _terminal.output().flush();
            }
            // The wait first: it has the terminal's handles attached to its backend, and `shutdown`
            // closes the resize pipe among them. Nothing waits in it by now -- this runs once the
            // events are closed and their last `Next()` has resumed.
            _wait.reset();
            // `Terminal::shutdown` undoes a completed `initialize()` (protocols, raw mode, the SIGWINCH
            // handler) and does nothing when it never completed. The input's own `shutdown` then covers
            // an `initialize()` that got as far as raw mode and no further. Both are idempotent.
            _terminal.shutdown();
            _terminal.input().shutdown();
        }

        void RestoreNow(std::string_view leading) noexcept override
        {
            // Not `shutdown()`: that closes the resize pipe the parked `poll` is waiting on and writes
            // core-cpp's state unlocked. The saved modes are this device's own, written once in `Acquire`
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
            auto const pieces = std::array<std::string_view, 2> { leading, _resets };
            _saved->Apply(pieces);
            _restoredNow.store(true, std::memory_order_release);
        }

      private:
        core::tui::Terminal _terminal;
        core::tui::runtime::TerminalInputSource _input { _terminal };
        std::unique_ptr<ITerminalInputWait> _wait;
        std::optional<SavedTerminalModes> _saved;
        std::string _resets;
        std::atomic<bool> _restoredNow { false };
        UsageColor _colour; ///< `--color` as this program resolved it: what `AskColour` answers.
    };
} // namespace

struct UnstartedTerminal::Parts
{
    std::unique_ptr<ITerminalDevice> device;
    core::async::IExecutor* pool { nullptr };
    core::async::IExecutor* resumeOn { nullptr };
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

std::expected<std::unique_ptr<UnstartedTerminal>, std::string> MakeTerminalEvents(core::async::IExecutor* pool,
                                                                                  core::async::IExecutor* resumeOn,
                                                                                  UsageColor colour)
{
    try
    {
        auto parts = std::make_unique<UnstartedTerminal::Parts>();
        parts->device = std::make_unique<TuiTerminalDevice>(colour);
        parts->pool = pool;
        parts->resumeOn = resumeOn;
        return std::make_unique<UnstartedTerminal>(std::move(parts));
    }
    catch (std::exception const& failure)
    {
        // Allocation is all that can fail here: the terminal and its readiness backend are made
        // when the terminal is started.
        return std::unexpected(std::string { "the terminal's wakeup could not be created: " } + failure.what());
    }
}

core::async::Task<std::expected<StartedTerminal, std::string>> StartTerminal(std::unique_ptr<UnstartedTerminal> terminal)
{
    auto& parts = UnstartedTerminalAccess::Of(*terminal);
    co_return co_await StartTerminalDevice(std::move(parts.device), parts.pool, parts.resumeOn);
}

} // namespace FastCache::Cli
