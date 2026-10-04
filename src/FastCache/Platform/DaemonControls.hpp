// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>

namespace FastCache
{

/// Process-wide control flags consulted by the running daemon and set by
/// the platform layer (POSIX signal handlers, Windows SCM control handler,
/// Ctrl+C handler). This is a small, deliberate, process-global state —
/// signal/handler thread safety mandates `std::atomic` and very small
/// scope. Tests do not exercise these directly; they go through
/// ISignalSource.
class DaemonControls
{
  public:
    /// @return the single process-wide instance.
    [[nodiscard]] static DaemonControls& Instance() noexcept;

    /// Mark that the daemon should shut down. Idempotent; safe from any thread.
    void RequestStop() noexcept
    {
        _stop.store(true, std::memory_order_release);
    }

    /// @return true once Stop has been requested.
    [[nodiscard]] bool StopRequested() const noexcept
    {
        return _stop.load(std::memory_order_acquire);
    }

    /// Mark that the daemon should reload its config. Safe from any thread.
    void RequestReload() noexcept
    {
        _reload.store(true, std::memory_order_release);
    }

    /// @return true exactly once per reload request (atomically clears the flag).
    [[nodiscard]] bool TakeReloadRequest() noexcept
    {
        return _reload.exchange(false, std::memory_order_acq_rel);
    }

    /// Mark that the daemon SERVES: every listener it opens is accepting. Idempotent; safe from any
    /// thread. Called where the readiness line is logged, never earlier -- a service host told to
    /// (`ServiceReadiness::BodySignals`) reports RUNNING on it, which is what `net start` answers.
    void MarkServing() noexcept
    {
        _serving.store(true, std::memory_order_release);
    }

    /// @return true once the daemon has said it serves.
    [[nodiscard]] bool Serving() const noexcept
    {
        return _serving.load(std::memory_order_acquire);
    }

    /// Reset every flag. Tests / restart scenarios only.
    void Reset() noexcept
    {
        _stop.store(false, std::memory_order_release);
        _reload.store(false, std::memory_order_release);
        _serving.store(false, std::memory_order_release);
    }

    /// Direct atomic access for callers that want to share the bool with
    /// signal-safe code that takes a `std::atomic<bool>&`.
    [[nodiscard]] std::atomic<bool>& StopFlag() noexcept
    {
        return _stop;
    }

  private:
    std::atomic<bool> _stop { false };
    std::atomic<bool> _reload { false };
    std::atomic<bool> _serving { false };
};

} // namespace FastCache
