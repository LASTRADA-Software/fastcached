// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "DashboardFrame.hpp"
#include "TerminalCapabilities.hpp"
#include "TerminalEvents.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/net/IoBackend.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/runtime/InputSource.hpp>

namespace FastCache::Cli
{

/// @file TerminalEventStream.hpp
/// The half of `TerminalEvents` that a test can drive: an `IDashboardEventSource` over any
/// `ITerminalInputWait`, the production wait over core-cpp's `core::tui` input and a
/// `core::net::IoBackend`, and the translation from decoded input to dashboard events.
///
/// **This header names core-cpp's terminal types, and `TerminalEvents.hpp` does not.** A caller
/// composing the dashboard includes that one and meets only the dashboard's vocabulary. Only
/// `TerminalEvents.cpp` and this stream's own test include this, so the two projects still meet
/// in one adapter -- it is simply split so the part with no terminal in it can be tested with a
/// scripted input source rather than a real one.

/// The bytes a keystroke would have arrived as, or empty when it has no byte spelling here.
///
/// `DashboardEvent::keys` carries bytes, and core-cpp hands over a DECODED key, so this spells it
/// back: a control chord as its control byte, Alt as an `ESC` prefix, a named key from a
/// table, and anything else as the UTF-8 of its codepoint. A key with none of those (a
/// function key, a lock key) answers empty and is not delivered, because the dashboard acts
/// only on keys it can name and an empty `Key` would be a keystroke with nothing in it.
/// @param key The decoded key.
/// @return Its bytes, or empty.
[[nodiscard]] std::string KeyBytes(core::tui::KeyEvent const& key);

/// What one wait for terminal input produced.
struct TerminalWaitOutcome
{
    /// The decoded input and resizes, in the order they arrived. Protocol reports are already
    /// consumed: a query's reply never surfaces here as input.
    std::vector<core::tui::InputEvent> events {};
    /// The terminal's input is gone -- a hang-up with nothing left to read, or a wait the backend
    /// could not perform. Nothing after this wait will deliver input.
    bool inputClosed { false };
};

/// Where the stream waits for terminal input: what endo's `EventSource::wait` was to it before
/// core-cpp moved all waiting into `core::net`.
///
/// `Wait` BLOCKS, which is why it has a seam of its own rather than being an `EventLoop` flow: the
/// stream runs it on a pool thread, and the thread `Next()` resumes on never waits on a terminal.
class ITerminalInputWait
{
  public:
    ITerminalInputWait() = default;
    ITerminalInputWait(ITerminalInputWait const&) = delete;
    ITerminalInputWait(ITerminalInputWait&&) = delete;
    ITerminalInputWait& operator=(ITerminalInputWait const&) = delete;
    ITerminalInputWait& operator=(ITerminalInputWait&&) = delete;
    virtual ~ITerminalInputWait() = default;

    /// Wait for input, a resize or `Wake()`, and decode what arrived.
    ///
    /// Input a terminal query read but did not consume is delivered first, without waiting. A wait
    /// with a timeout that ends with nothing ready completes a partial escape sequence: that is how a
    /// lone `ESC` becomes the Escape key rather than the start of a sequence nobody is typing.
    /// @param timeoutMs How long to wait in milliseconds, or a negative value for as long as it takes.
    /// @return What arrived; empty after a timeout or a wake with nothing ready.
    [[nodiscard]] virtual TerminalWaitOutcome Wait(int timeoutMs) = 0;

    /// End a `Wait` parked on another thread, or make the next one return at once. Any thread.
    virtual void Wake() noexcept = 0;
};

/// The production `ITerminalInputWait`: @p input's handles, watched by @p backend.
///
/// The input handle and the resize handle -- where there is one -- are attached once, here, so
/// @p input must already be initialized: an uninitialized terminal has no handles to give. A hang-up
/// on the input handle with nothing readable is `inputClosed`, never a readable handle that reads
/// nothing, which would wake every wait at once for ever.
/// @param input What to watch and decode (not owned; outlives the result).
/// @param backend The readiness backend the wait blocks in; owned by the result.
/// @return The wait, or why the backend refused a handle.
[[nodiscard]] std::expected<std::unique_ptr<ITerminalInputWait>, std::string> MakeTerminalInputWait(
    core::tui::runtime::InputSource& input, std::unique_ptr<core::net::IoBackend> backend);

/// The dashboard events one wait produced, in the order it produced them.
///
/// Keys and resizes are translated; everything else the terminal decodes (mouse, paste, focus) is
/// not dashboard input and is dropped. A wait whose input closed means the terminal is gone, so it
/// ends the stream with `Detached` after whatever came before it.
/// @param outcome What the wait returned.
/// @param cellPixels The cell size every resize in @p outcome carries: what was read after this wait.
/// @return The events, possibly none.
[[nodiscard]] std::vector<DashboardEvent> ToDashboardEvents(TerminalWaitOutcome const& outcome,
                                                            std::optional<CellPixelSize> cellPixels);

/// What core-cpp's answer to `CSI 16 t` says about the cell size, in the dashboard's vocabulary.
///
/// **Only a size both of whose sides are positive is one.** A reply of `CSI 6 ; 0 ; 0 t` is a terminal
/// saying it does not know, and an image sized from it would be empty; it is nullopt exactly as a
/// terminal that stayed silent is, because the Sixel rung is decided on having a size, never on why
/// there is none.
/// @param answer core-cpp's `queryCellSize` answer: width then height in pixels, or why there is none.
/// @return The size, or nullopt.
[[nodiscard]] std::optional<CellPixelSize> ToCellPixelSize(
    std::expected<std::pair<int, int>, core::tui::QueryUnanswered> const& answer) noexcept;

/// What core-cpp's DA1 answer says about Sixel, in the dashboard's vocabulary.
///
/// A reply without attribute 4 and no reply at all are different facts, and both draw no Sixel.
/// @param attributes The DA1 reply, or why there is none.
/// @return The answer.
[[nodiscard]] SixelAnswer ToSixelAnswer(
    std::expected<core::tui::DeviceAttributesReport, core::tui::QueryUnanswered> const& attributes) noexcept;

/// What core-cpp's DECRQM answer for mode 2026 says about synchronized output.
///
/// Recognised and switchable -- set or reset -- is `Supported`. Not recognised, or permanently set
/// or reset, is `NotSupported`: a mode that cannot be switched is not one a frame can bracket. A
/// platform arm that cannot send the query is `NotAsked`, as is a terminal with no input to read.
/// @param status The DECRQM answer.
/// @return The answer.
[[nodiscard]] SynchronizedOutputAnswer ToSynchronizedOutputAnswer(core::tui::DecModeStatus status) noexcept;

/// The bytes presentation is spelled with, composed once.
///
/// **Spelled by core-cpp, not here.** Each is captured from core-cpp's own `TerminalOutput` -- its
/// `enterAltScreen`, `hideCursor`, `showCursor`, `leaveAltScreen` -- through the
/// `writeToDestination` hook it provides for exactly that, so no sequence core-cpp already writes is
/// restated in this project. `FrameBytes` spells a frame the same way. The two exceptions are
/// synchronized output's begin and end: core-cpp writes those only from inside `SyncGuard`, straight to
/// a native handle, so there is nothing to capture, and they are spelled once in
/// `TerminalEventStream.cpp`.
struct TerminalScreenBytes
{
    std::string enter;     ///< Enter the alternate screen, then hide the cursor.
    std::string leave;     ///< Show the cursor, then leave the alternate screen.
    std::string syncBegin; ///< Begin synchronized output (DEC mode 2026).
    std::string syncEnd;   ///< End synchronized output.
};

/// @return The bytes, composed on first use.
[[nodiscard]] TerminalScreenBytes const& ScreenBytes();

/// Whether a frame for a terminal with @p capabilities is bracketed in synchronized output.
///
/// The one guard between a DECRQM answer and a `CSI ? 2026 h` on the wire: only a terminal that
/// answered `Supported` gets one, so a terminal that never answered is drawn unsynchronized and
/// nothing waits on it.
/// @param capabilities The record the start produced.
/// @return True for `SynchronizedOutputAnswer::Supported` alone.
[[nodiscard]] bool PresentsSynchronized(TerminalCapabilities const& capabilities) noexcept;

/// One frame as it goes on the wire: each row placed at its own position, optionally bracketed.
///
/// **Every row is positioned absolutely -- `CSI <row>;1H`, `CSI K`, the row -- after the screen is
/// cleared from the frame's last row down. A frame's `\n` never reaches the terminal.** A line feed moves the
/// cursor DOWN; whether it also returns to the first column is the terminal's newline mode, a
/// Windows console's DISABLE_NEWLINE_AUTO_RETURN (which core-cpp's raw mode sets) and a POSIX tty's
/// output post-processing, none of it this process's to rely on. Measured in a 120x40 ConPTY: rows
/// joined by bare line feeds after a full-width row all landed in the last column, and the screen
/// showed the top border and one column. Positioning each row is also immune to a pending wrap and
/// to a row whose width was miscounted, since the next row starts where it says regardless.
///
/// Erasing comes BEFORE writing, because an erase starts at the cursor and a full-width row leaves
/// the cursor on its own last cell: `row, CSI K` would erase that cell.
///
/// A frame's final `\n`, if it has one, ends the last row rather than starting an empty one below
/// it, which on a screen exactly as tall as the frame would erase the bottom row.
/// @param frame The composed frame, rows separated by `\n`.
/// @param synchronized Whether to bracket it in synchronized output.
/// @return The bytes.
[[nodiscard]] std::string FrameBytes(std::string_view frame, bool synchronized);

/// One frame with its images, as it goes on the wire: the text rows exactly as `FrameBytes` writes
/// them, then each image at its own cell, inside the same synchronized-output bracket.
///
/// **The images go on AFTER every row.** A row is erased and written over its whole width, and on a
/// terminal that draws Sixel an erase or a character on a cell removes the image there, so an image
/// written first would be taken off again by the row it sits in. Each is written as `CSI <row>;<column> H`
/// and then the image, framed by core-cpp's `writeSixel`, because a Sixel image is drawn from the cursor.
///
/// **A later frame without an image leaves none of it behind, with nothing remembered here.** Every
/// frame erases each of its rows and everything below its last before writing, so the cells an earlier
/// image covered are repainted by whatever the new frame puts there, text or blank.
///
/// **Its spans are not dressed**: this is the plain frame, which the capability-record overload below
/// dresses where colour is allowed.
/// @param frame The composed frame and the images placed over it.
/// @param synchronized Whether to bracket it in synchronized output.
/// @return The bytes.
[[nodiscard]] std::string FrameBytes(DashboardFrame const& frame, bool synchronized);

/// One frame as a terminal with @p capabilities is sent it: `FrameBytes`, bracketed exactly when
/// `PresentsSynchronized`, and with each `FrameSpan` dressed from `TonePalette` exactly when the record
/// allows colour. Where it does not, the bytes are the plain frame's: not one SGR sequence, and the grid
/// the same.
/// @param frame The composed frame, the images placed over it and the runs it dresses.
/// @param capabilities The terminal's record.
/// @return The bytes.
[[nodiscard]] std::string FrameBytes(DashboardFrame const& frame, TerminalCapabilities const& capabilities);

/// What a terminal event stream is built from.
struct TerminalStreamParts
{
    /// Where input is waited for. Its `Wait` BLOCKS, so it is only ever called from `pool`.
    ITerminalInputWait* source { nullptr };
    /// Where the blocking wait runs. Give it a thread of its own: a wait parks there for as long
    /// as the operator types nothing, and a sampler sharing a one-thread pool would starve.
    IExecutor* pool { nullptr };
    /// Where `Next()` resumes before it returns an event.
    IExecutor* resumeOn { nullptr };
    /// Wakes a wait parked in `source`, so `Close()` ends it rather than waiting for a key.
    std::function<void()> wake;
    /// The geometry at open, delivered as the first event.
    int columns { 0 };
    int rows { 0 };
    /// The cell size at open, carried by that first event.
    std::optional<CellPixelSize> cellPixels {};
    /// Asks the terminal for its cell size again. BLOCKS for up to a query's deadline, so it runs on
    /// `pool`, after a wait that delivered a resize and before the next wait. Empty means the cell size
    /// is never asked again and every later resize carries none.
    std::function<std::optional<CellPixelSize>()> rereadCellPixels {};
};

/// An `IDashboardEventSource` over @p parts.
///
/// Its first event is a `Resize` carrying the geometry at open. Each later `Next()` hops to the
/// pool, waits there for readiness, hops back and delivers what the wait produced, one event
/// per call. After `Close()` or the end of input it answers `Detached`, and keeps answering it.
///
/// **Resizes coalesce.** A resize that arrives while the last event still waiting to be read is a
/// resize REPLACES it, so a window being dragged -- dozens of geometry changes -- queues one frame's
/// worth rather than dozens, and what is delivered is the latest geometry. A resize separated from
/// an earlier one by any other event is delivered separately, so the order of keys and resizes is
/// kept.
///
/// **A resize carries the cell size read AFTER it.** A font change resizes the grid and the cells
/// together, so a size remembered from before the resize could be the wrong one. After a wait that
/// delivered a resize, `rereadCellPixels` asks the terminal again, on the pool and before the next
/// wait, so the query's reply is read by the same thread that reads the input, with nothing parked
/// beside it. One question per wait, however many resizes it delivered. With no `rereadCellPixels`,
/// every resize after the first carries none.
///
/// **Lifetime.** A `Next()` parked in the wait holds the stream. Close it and let that `Next()`
/// resume before destroying the stream, which is the order `RunDashboard` already follows.
/// @param parts The source, executors, wake hook and initial geometry. The pointers must be
///        non-null and outlive the result.
/// @return The stream.
[[nodiscard]] std::unique_ptr<IDashboardEventSource> MakeTerminalEventStream(TerminalStreamParts parts);

/// The terminal-facing half `StartTerminal` acquires, as a seam.
///
/// Production is core-cpp's `Terminal` with `MakeTerminalInputWait` over it. A test substitutes one that
/// records which thread it was asked on and can fail at each step, which is the only way to watch a
/// start that fails HALFWAY restore the terminal without having a terminal to break.
class ITerminalDevice
{
  public:
    ITerminalDevice() = default;
    ITerminalDevice(ITerminalDevice const&) = delete;
    ITerminalDevice(ITerminalDevice&&) = delete;
    ITerminalDevice& operator=(ITerminalDevice const&) = delete;
    ITerminalDevice& operator=(ITerminalDevice&&) = delete;
    virtual ~ITerminalDevice() = default;

    /// Acquire the terminal: refuse without one, then raw mode and the terminal's own startup
    /// queries. BLOCKS; called on the pool.
    /// @return Nothing, or why the terminal could not be acquired.
    [[nodiscard]] virtual std::expected<void, std::string> Acquire() = 0;

    /// Ask the acquired terminal whether it draws Sixel (DA1). BLOCKS; called on the pool.
    /// @return The answer, `NoReply` included.
    [[nodiscard]] virtual SixelAnswer AskSixel() = 0;

    /// Ask the acquired terminal whether it supports synchronized output (DECRQM for mode 2026).
    /// BLOCKS; called on the pool.
    /// @return The answer, `NoReply` included.
    [[nodiscard]] virtual SynchronizedOutputAnswer AskSynchronizedOutput() = 0;

    /// How many pixels a cell of the acquired terminal measures, as it answered `CSI 16 t` while being
    /// acquired: under the same deadline, and through the same raw-mode read, as DA1. Called on the pool.
    /// @return The size, or nullopt when the terminal did not report one.
    [[nodiscard]] virtual std::optional<CellPixelSize> AskCellPixels() = 0;

    /// Ask the acquired terminal for its cell size again (`CSI 16 t`), now. BLOCKS for up to the query
    /// deadline; called on the pool, between two waits in `Events()`, after a resize.
    /// @return The size, or nullopt when the terminal did not report one.
    [[nodiscard]] virtual std::optional<CellPixelSize> RereadCellPixels() = 0;

    /// @return What the environment says the terminal draws. Called on the pool.
    [[nodiscard]] virtual TerminalTextEncoding Encoding() = 0;

    /// Whether this terminal's frames are dressed with the palette: `--color` as this program resolved it.
    /// Called on the pool.
    /// @return `Supported` or `Suppressed`.
    [[nodiscard]] virtual ColourAnswer AskColour() = 0;

    /// @return The geometry, once acquired.
    [[nodiscard]] virtual int Columns() const = 0;
    [[nodiscard]] virtual int Rows() const = 0;

    /// @return Where the acquired terminal's events are waited for.
    [[nodiscard]] virtual ITerminalInputWait& Events() = 0;

    /// Wake a wait parked in `Events()`. Callable from any thread.
    virtual void Wake() = 0;

    /// Write @p bytes to the terminal, now. On the pool while starting, and on the thread that
    /// awaits the events after that.
    /// @param bytes The bytes.
    virtual void Write(std::string_view bytes) noexcept = 0;

    /// Put back whatever `Acquire` changed, however far it got. Idempotent. Called on the thread
    /// that owns the events, once nothing waits in `Events()`.
    virtual void Restore() noexcept = 0;

    /// Put the terminal's modes back NOW, from any thread, while a wait in `Events()` may be parked.
    ///
    /// Writes @p leading first -- the screen's own resets, which the caller composes -- then the
    /// input's, then puts the modes back. Touches process-wide terminal state and the output
    /// handle only, never anything `Events()` reads. Reached through `StartedTerminal::restore`,
    /// which calls it at most once and never after the device's teardown has begun.
    /// @param leading Bytes to write before the input's resets.
    virtual void RestoreNow(std::string_view leading) noexcept = 0;
};

/// `StartTerminal` over any device: the two-hop, the restore guard, the screen and the record.
///
/// Hops to @p pool, acquires the device, asks it for Sixel, synchronized output and its cell size,
/// enters the alternate screen, reads the encoding, hops back to @p resumeOn and only then returns.
/// The events ask for the cell size again after each resize only when the start got one: a terminal
/// that did not answer at start is not asked again, since its rung is not Sixel and each unanswered
/// question costs a deadline.
///
/// On every exit but success -- a refused acquisition, or an exception after the terminal was already
/// in raw mode or on the alternate screen -- the screen is left if it was entered and the device is
/// restored, before the result is handed back. On success the device is shared by the returned
/// events, which leave the screen and restore the device when they are destroyed, and the frame
/// presenter.
/// @param device The device to start; consumed.
/// @param pool Where the blocking steps run.
/// @param resumeOn Where the start and the events resume.
/// @return The started terminal, or why it could not be acquired.
[[nodiscard]] Task<std::expected<StartedTerminal, std::string>> StartTerminalDevice(std::unique_ptr<ITerminalDevice> device,
                                                                                    IExecutor* pool,
                                                                                    IExecutor* resumeOn);

} // namespace FastCache::Cli
