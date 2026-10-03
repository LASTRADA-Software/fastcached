// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/StopPending.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache
{

/// Declared, not included: `ServiceHostOptions` holds only a pointer to one, and every includer of
/// this header would otherwise pull in the hub's threading headers.
class IHostEventSink;

/// Abstract host for the daemon body. Three implementations:
///   - ForegroundHost:       run body() inline (development, tests)
///   - PosixDaemonHost:      double-fork+setsid+stdio redirect+pidfile, run body() in the child
///   - ServiceHost:          register with a service manager (`IServiceControlManager`; the
///                           SCM on Windows), run body() inside its service main
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

    /// Report a start this process refuses before any body can run, the way this host's
    /// supervisor reads a failed start, instead of `Run`.
    ///
    /// **The default reports nothing and is right for two of the three hosts**: in the
    /// foreground the exit code IS the report, and a POSIX daemon has not forked yet -- its
    /// supervisor reads the exit code and the refusal was said on the terminal. The SCM is the
    /// exception: it waits for a service it started to connect, and a process that exits before
    /// connecting is reported as error 1053, *did not respond in a timely fashion*, with the
    /// refusal nowhere an operator running `sc start` looks. So the service host connects and
    /// reports the stop with @p exitCode as the service-specific code (`ServiceStatusPlan.hpp`).
    /// @param exitCode Why the start is refused, as the process's exit code; never zero.
    /// @return The process exit code: @p exitCode.
    [[nodiscard]] virtual int Refuse(int exitCode)
    {
        return exitCode;
    }
};

/// Refuse a start: say why through @p logger, then report it through @p host.
///
/// **One call per refusal, and the order is the point**: a refusal logged and then returned from
/// `main` without the host is the defect `IDaemonHost::Refuse` closes, so both halves are one
/// operation rather than two lines a new refusal can copy half of.
/// @param host The host this process runs under -- chosen BEFORE the configuration is judged.
/// @param logger Where the reason goes: the event log under a service, the terminal otherwise.
/// @param reason Why this process will not start.
/// @param exitCode The exit code that says so; never zero.
/// @return The process exit code.
[[nodiscard]] inline int RefuseStart(IDaemonHost& host, ILogger& logger, std::string_view reason, int exitCode)
{
    logger.Log(LogLevel::Error, reason);
    return host.Refuse(exitCode);
}

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
///
/// **The working directory is the CALLER's to state, and there is no default.**
/// Daemonizing means leaving the invocation directory — a daemon that keeps it
/// holds a filesystem busy and cannot be unmounted — and `/` is the classical
/// answer, right for a process that executes nothing. It is wrong for one that
/// SPAWNS A COMPILER: the compile node builds
/// `-fdebug-prefix-map=<its own directory>=<what the client asked for>`, a
/// prefix-map rule appends the unmatched tail, and `<from>` = `/` therefore
/// rewrites every absolute path in the object — `/usr/include/stdio.h` becomes
/// `.usr/include/stdio.h`, under a cache key that is correct
/// ([#784](https://github.com/LASTRADA-Software/fastcached/issues/784),
/// [#674](https://github.com/LASTRADA-Software/fastcached/issues/674)).
///
/// So the two binaries answer differently and each says which it is, rather than
/// one of them inheriting an answer chosen for the other. No default parameter:
/// a third daemonizing binary must decide, and a compile error is the only thing
/// that makes it.
///
/// @param pidfile Path to pidfile (may be empty). Written BEFORE the chdir, so a
///        relative one lands where the operator ran the command rather than
///        wherever this host was told to move to.
/// @param workingDirectory The directory to chdir into. Must be absolute and must
///        exist; `/` is used when the chdir fails, which is the historic value and
///        keeps a daemon that cannot reach its directory running rather than
///        parked on a mount point it cannot release.
/// @return Owning host or nullptr.
[[nodiscard]] std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string const& pidfile,
                                                               std::string const& workingDirectory);

/// What the Windows service host needs beyond the service's name.
struct ServiceHostOptions
{
    StopPendingPlan stop {}; ///< What a stop reports while the body winds down.

    /// Where power events (suspend, resume) are delivered, or null to not accept them. Must
    /// outlive `Run`. A hint only: see `HostEvent` for why nothing may depend on one arriving.
    IHostEventSink* hostEvents { nullptr };
};

/// Construct a Windows Service host registered with the SCM. Returns
/// nullptr on non-Windows platforms.
/// @param serviceName Service name as registered with SCM.
/// @param options How a stop is reported, and where power events go.
/// @return Owning host or nullptr.
[[nodiscard]] std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string const& serviceName,
                                                                  ServiceHostOptions options);

} // namespace FastCache
