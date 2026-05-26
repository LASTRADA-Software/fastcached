// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache
{

/// Abstract host for the daemon body. Three implementations:
///   - ForegroundHost:       run body() inline (development, tests)
///   - PosixDaemonHost:      double-fork+setsid+stdio redirect+pidfile, run body() in the child
///   - WindowsServiceHost:   register with SCM, run body() inside ServiceMain
///
/// The body function returns a process exit code.
class IDaemonHost
{
  public:
    using Body = std::function<int()>;

    IDaemonHost() = default;
    IDaemonHost(IDaemonHost const&) = delete;
    IDaemonHost(IDaemonHost&&) = delete;
    IDaemonHost& operator=(IDaemonHost const&) = delete;
    IDaemonHost& operator=(IDaemonHost&&) = delete;
    virtual ~IDaemonHost() = default;

    /// Set up the hosting context (forking, SCM registration, stdio
    /// redirection, etc.) and invoke body. Returns the body's exit code.
    /// @param body Daemon entry; must be self-contained.
    /// @return Process exit code.
    [[nodiscard]] virtual int Run(Body body) = 0;

    /// Best-effort request that the hosted process terminate at the next
    /// graceful checkpoint. Foreground host raises an internal stop flag;
    /// POSIX host does nothing (SIGTERM does the work); SCM host
    /// transitions the service to STOP_PENDING.
    virtual void RequestStop() noexcept {}
};

/// Foreground host: runs the body inline. Used by `fastcached` when neither
/// `--daemon` nor a Windows service registration is in play, and by tests.
class ForegroundHost final: public IDaemonHost
{
  public:
    int Run(Body body) override
    {
        if (!body)
            return 0;
        return body();
    }
};

/// Construct a POSIX daemon host (double-fork, setsid, stdio /dev/null,
/// optional pidfile). Returns nullptr on platforms where it's not
/// supported (Windows).
/// @param pidfile Path to pidfile (may be empty).
/// @return Owning host or nullptr.
[[nodiscard]] std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string const& pidfile);

/// Construct a Windows Service host registered with the SCM. Returns
/// nullptr on non-Windows platforms.
/// @param serviceName Service name as registered with SCM.
/// @return Owning host or nullptr.
[[nodiscard]] std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string const& serviceName);

} // namespace FastCache
