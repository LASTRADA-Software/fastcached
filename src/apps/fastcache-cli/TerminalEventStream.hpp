// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "TerminalCapabilities.hpp"
#include "TerminalEvents.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <tui/InputEvent.hpp>
#include <tui/Terminal.hpp>
#include <tui/runtime/EventSource.hpp>

namespace FastCache::Cli
{

/// @file TerminalEventStream.hpp
/// The half of `TerminalEvents` that a test can drive: an `IDashboardEventSource` over any
/// endo `EventSource`, plus the translation from endo's decoded input to dashboard events.
///
/// **This header names endo's types, and `TerminalEvents.hpp` does not.** A caller composing
/// the dashboard includes that one and meets only the dashboard's vocabulary. Only
/// `TerminalEvents.cpp` and this stream's own test include this, so the two projects still
/// meet in one adapter -- it is simply split so the part with no terminal in it can be tested
/// with endo's scripted source rather than a real one.
///
/// It reaches `tui/runtime/EventSource.hpp` and `tui/InputEvent.hpp`, neither of which reaches
/// `coro/` or `platform/Clock.hpp`, so `ctest -R vendor-vocabulary` needs no row for it.

/// The bytes a keystroke would have arrived as, or empty when it has no byte spelling here.
///
/// `DashboardEvent::keys` carries bytes, and endo hands over a DECODED key, so this spells it
/// back: a control chord as its control byte, Alt as an `ESC` prefix, a named key from a
/// table, and anything else as the UTF-8 of its codepoint. A key with none of those (a
/// function key, a lock key) answers empty and is not delivered, because the dashboard acts
/// only on keys it can name and an empty `Key` would be a keystroke with nothing in it.
/// @param key The decoded key.
/// @return Its bytes, or empty.
[[nodiscard]] std::string KeyBytes(tui::KeyEvent const& key);

/// The dashboard events one endo wait produced, in the order it produced them.
///
/// Keys and resizes are translated; everything else endo decodes (mouse, paste, focus) is not
/// dashboard input and is dropped. An `interrupted` wait means the terminal's input is gone
/// (EOF, hang-up, a failed wait), so it ends the stream with `Detached` after whatever came
/// before it.
/// @param outcome What the wait returned.
/// @return The events, possibly none.
[[nodiscard]] std::vector<DashboardEvent> ToDashboardEvents(tui::runtime::WaitOutcome const& outcome);

/// What endo's DA1 answer says about Sixel, in the dashboard's vocabulary.
///
/// A reply without attribute 4 and no reply at all are different facts, and both draw no Sixel.
/// @param attributes The DA1 reply, or why there is none.
/// @return The answer.
[[nodiscard]] SixelAnswer ToSixelAnswer(
    std::expected<tui::DeviceAttributesReport, tui::QueryUnanswered> const& attributes) noexcept;

/// What a terminal event stream is built from.
struct TerminalStreamParts
{
    /// Where input is waited for. Its `wait` BLOCKS, so it is only ever called from `pool`.
    tui::runtime::EventSource* source { nullptr };
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
};

/// An `IDashboardEventSource` over @p parts.
///
/// Its first event is a `Resize` carrying the geometry at open. Each later `Next()` hops to the
/// pool, waits there for readiness, hops back and delivers what the wait produced, one event
/// per call. After `Close()` or the end of input it answers `Detached`, and keeps answering it.
///
/// **Lifetime.** A `Next()` parked in the wait holds the stream. Close it and let that `Next()`
/// resume before destroying the stream, which is the order `RunDashboard` already follows.
/// @param parts The source, executors, wake hook and initial geometry. The pointers must be
///        non-null and outlive the result.
/// @return The stream.
[[nodiscard]] std::unique_ptr<IDashboardEventSource> MakeTerminalEventStream(TerminalStreamParts parts);

/// The terminal-facing half `StartTerminal` acquires, as a seam.
///
/// Production is endo's `Terminal` with its event source and a wakeup. A test substitutes one that
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

    /// @return What the environment says the terminal draws. Called on the pool.
    [[nodiscard]] virtual TerminalTextEncoding Encoding() = 0;

    /// @return The geometry, once acquired.
    [[nodiscard]] virtual int Columns() const = 0;
    [[nodiscard]] virtual int Rows() const = 0;

    /// @return Where the acquired terminal's events are waited for.
    [[nodiscard]] virtual tui::runtime::EventSource& Events() = 0;

    /// Wake a wait parked in `Events()`. Callable from any thread.
    virtual void Wake() = 0;

    /// Put back whatever `Acquire` changed, however far it got. Idempotent. Called on the thread
    /// that owns the events, once nothing waits in `Events()`.
    virtual void Restore() noexcept = 0;

    /// Put the terminal's modes back NOW, from any thread, while a wait in `Events()` may be parked.
    ///
    /// Touches process-wide terminal state and the output handle only, never anything `Events()`
    /// reads. Reached through `StartedTerminal::restore`, which calls it at most once and never
    /// after the device's teardown has begun.
    virtual void RestoreNow() noexcept = 0;
};

/// `StartTerminal` over any device: the two-hop, the restore guard and the record.
///
/// Hops to @p pool, acquires the device and asks it for Sixel and the encoding, hops back to
/// @p resumeOn and only then returns. On every exit but success -- a refused acquisition, or an
/// exception after the terminal was already in raw mode -- the device is restored before the result
/// is handed back. On success the device moves into the returned events, which restore it when they
/// are destroyed.
/// @param device The device to start; consumed.
/// @param pool Where the blocking steps run.
/// @param resumeOn Where the start and the events resume.
/// @return The started terminal, or why it could not be acquired.
[[nodiscard]] Task<std::expected<StartedTerminal, std::string>> StartTerminalDevice(std::unique_ptr<ITerminalDevice> device,
                                                                                    IExecutor* pool,
                                                                                    IExecutor* resumeOn);

} // namespace FastCache::Cli
