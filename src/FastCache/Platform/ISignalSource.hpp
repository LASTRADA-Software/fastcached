// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace FastCache
{

/// Logical events that the platform layer surfaces to the daemon. POSIX
/// signals (SIGTERM/SIGINT/SIGHUP) and Windows SCM controls (Stop,
/// PARAMCHANGE) both map onto these.
enum class SignalEvent : unsigned
{
    Stop = 0,
    Reload = 1,
};

/// Abstract event source. Production: POSIX signals via signal handlers;
/// Windows: SCM control handler. Tests: ScriptedSignalSource that fires
/// events on demand for deterministic verification of the reload
/// pipeline.
class ISignalSource
{
  public:
    ISignalSource() = default;
    ISignalSource(ISignalSource const&) = delete;
    ISignalSource(ISignalSource&&) = delete;
    ISignalSource& operator=(ISignalSource const&) = delete;
    ISignalSource& operator=(ISignalSource&&) = delete;
    virtual ~ISignalSource() = default;

    /// True if a Stop event has been observed since construction.
    [[nodiscard]] virtual bool StopRequested() const noexcept = 0;

    /// Atomically take any pending Reload event (returns true once per
    /// underlying signal, false thereafter until the next signal).
    [[nodiscard]] virtual bool TakeReloadRequest() noexcept = 0;
};

/// In-memory signal source for tests.
class ScriptedSignalSource final: public ISignalSource
{
  public:
    void FireStop() noexcept { _stop = true; }
    void FireReload() noexcept { _reload = true; }

    [[nodiscard]] bool StopRequested() const noexcept override { return _stop; }
    [[nodiscard]] bool TakeReloadRequest() noexcept override
    {
        if (!_reload)
            return false;
        _reload = false;
        return true;
    }

  private:
    bool _stop { false };
    bool _reload { false };
};

} // namespace FastCache
