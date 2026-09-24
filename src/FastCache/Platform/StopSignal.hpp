// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <core/async/IExecutor.hpp>
#include <core/async/Task.hpp>

namespace FastCache
{

/// @file StopSignal.hpp
/// An operator's stop request -- Ctrl-C at a terminal -- as something a coroutine AWAITS.
///
/// **Awaited, never polled.** `ISignalSource` answers the daemons' question with a flag,
/// and a reactor coroutine can only observe a flag by waking to look at it, which is a
/// loop of sleeps however short. So this seam blocks where blocking is allowed -- a
/// thread of the caller's choosing, waiting on the signal and on a cancellation at once --
/// and hands the resumption back to the caller's executor, the same two hops a stats
/// sample takes.

/// Why a `Stopped()` wait ended.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted.
enum class StopWake : std::uint8_t
{
    Stopped,   ///< The operator asked to stop.
    Cancelled, ///< `Cancel()` was called first.
    Failed,    ///< The wait itself failed; nothing can say whether a stop will be heard.
    Last,
};

/// A stop request, awaited.
///
/// Both outcomes are STICKY: once a stop has been requested or the signal cancelled, every
/// later `Stopped()` answers at once. So a stop that lands before anybody waits is not
/// lost, and a cancel that lands before the wait is not either.
class IStopSignal
{
  public:
    IStopSignal() = default;
    IStopSignal(IStopSignal const&) = delete;
    IStopSignal(IStopSignal&&) = delete;
    IStopSignal& operator=(IStopSignal const&) = delete;
    IStopSignal& operator=(IStopSignal&&) = delete;
    virtual ~IStopSignal() = default;

    /// Suspend until a stop is requested or `Cancel()` is called, whichever is first.
    ///
    /// Pointers rather than references because this is a coroutine. Neither may be null,
    /// and a wait still outstanding must be cancelled AND have resumed before the signal is
    /// destroyed.
    /// @param waiter Where the blocking wait runs. **Not a pool that does other work**: the
    ///        wait holds its thread for as long as the session runs, so a one-thread sample
    ///        pool lent to it would never sample again.
    /// @param resumeOn Where the caller is resumed with the answer.
    /// @return Why the wait ended.
    [[nodiscard]] virtual core::async::Task<StopWake> Stopped(core::async::IExecutor* waiter,
                                                              core::async::IExecutor* resumeOn) = 0;

    /// End an outstanding or future `Stopped()` with `Cancelled`.
    ///
    /// Safe from any thread, idempotent, and it does not wait for the waiter to resume.
    virtual void Cancel() noexcept = 0;
};

/// Catch this process's interactive stop request for as long as the result lives.
///
/// SIGINT on POSIX, Ctrl-C and Ctrl-Break at a Windows console. **The previous disposition
/// is restored when the result is destroyed**, on every path out: a command-line tool that
/// left SIGINT redirected after it was done would change how its host process dies, and
/// nothing about the symptom would point here.
///
/// One per process at a time, because a signal disposition is process-wide: a second is
/// refused while the first lives rather than silently replacing it.
///
/// **Three outcomes, and the second is not a failure.**
///   - A signal that fires on the operator's stop request.
///   - On POSIX, when this process INHERITED SIGINT as ignored -- a background job, `nohup` --
///     a signal that installs nothing and never fires: only `Cancel()` ends its wait. The caller
///     asked for Ctrl-C to be ignored, and it is; catching it would let a Ctrl-C meant for the
///     foreground end this process, which is the changed death the rule above forbids. Windows
///     has no call that reads an inherited ignore back, so it has no such arm.
///   - Why nothing could be installed.
/// @return The signal, or why it could not be installed.
[[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> InstallStopSignal();

/// The descriptor (POSIX) or handle value (Windows) the stop handler writes to, or -1 before any
/// install has created it.
///
/// **Created once and never closed**, because a handler already running when a signal is
/// uninstalled may still write to it. Exposed so a test can hold that property to what it claims
/// -- reused by the next install, and still open after an uninstall -- and nothing else needs it.
/// @return The handler-facing end, or -1.
[[nodiscard]] std::intptr_t StopSignalHandlerEnd() noexcept;

} // namespace FastCache
