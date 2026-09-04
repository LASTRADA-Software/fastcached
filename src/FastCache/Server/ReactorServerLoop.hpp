// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/ExpiryReaper.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IAdmissionControl.hpp>
#include <FastCache/Protocol/SessionContext.hpp>
#include <FastCache/Server/ReadinessAnnouncer.hpp>
#include <FastCache/Server/Server.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

class TlsContext; // defined in Net/TlsContext.hpp; only used when TLS is built in

/// Options for the reactor-driven server loop. Bundled so we can pass the
/// same options across the three platform-specific implementations.
struct ReactorServerOptions
{
    /// Every listener endpoint to bring up. Must be non-empty.
    std::vector<BindConfig> binds {};
    std::size_t maxConnections { 0 }; ///< 0 = unlimited.
    int listenBacklog { 511 };        ///< ::listen() backlog depth.

    /// Number of independent reactors to run, each single-threaded with its
    /// own connections (no coroutine migration). 1 is the classic single-loop
    /// server. >1 scales across cores: on Windows a single acceptor hands
    /// sockets round-robin to N IOCP reactors; on POSIX N listeners share the
    /// port via SO_REUSEPORT. With >1, the storage every connection reaches
    /// must be thread-safe (the caller wraps it in a ShardedStorage).
    unsigned reactorThreads { 1 };

    /// Pin each reactor thread to a distinct CPU core (reactor *i* → core
    /// *i % online_cpus*). Keeps a worker's hot state resident in one core's
    /// caches instead of migrating across cores. Best-effort: ignored when the
    /// platform doesn't support pinning. Only meaningful with reactorThreads>1.
    bool pinReactorsToCpu { false };

    /// Per-server session context forwarded to every connection (auth policy
    /// and other optional collaborators). Default-constructed = auth disabled.
    /// The referenced objects must outlive the server run.
    SessionContext session {};

    /// When true, every connection prefixes its log lines with the client IP
    /// (the accepted socket's peer address). Maps `Config::logSource`.
    bool logSource { false };

    /// TLS context for terminating TLS on accepted connections, or nullptr for
    /// plaintext. Only honoured in TLS-enabled builds; must outlive the run.
    TlsContext* tlsContext { nullptr };

    /// Pacing and ceilings for the active expiry cycle.
    ///
    /// A zero `interval` turns the cycle off, leaving expiry entirely
    /// access-driven -- which is what the daemon did before it had one, and
    /// which means a key that lapses and is never touched again is neither
    /// reclaimed nor reported.
    ExpiryReaperOptions expiry {};

    /// Clock the reactors drive and read, or nullptr to let the loop own a
    /// plain `SteadyClock`.
    ///
    /// Pass the same clock the `CacheEngine` was built with to make it a
    /// `CachedClock`: the reactor refreshes it once per loop iteration, and
    /// every command served in between reads a stored value instead of calling
    /// `QueryPerformanceCounter`. Only the loop iterates, so only the loop can
    /// refresh it — which is why the two have to be the same object rather than
    /// two `SteadyClock` instances as they were before.
    ///
    /// Must outlive the run.
    IClock* clock { nullptr };
};

/// Run the reactor-driven server loop using the platform's native
/// reactor (IocpReactor / EpollReactor / KqueueReactor). Returns 0 on
/// clean shutdown, non-zero on bind failure.
///
/// The loop watches DaemonControls::Instance() for stop requests, so a
/// SIGINT/SIGTERM/SCM Stop will tear it down cleanly. Connection
/// metrics are routed through `metrics` (nullable); admission is gated
/// by `admission` (nullable; nullptr means unbounded).
///
/// @return Process exit code (0 on clean shutdown).
int RunReactorServer(ReactorServerOptions const& options,
                     CacheEngine& engine,
                     ILogger& logger,
                     IAdmissionControl* admission = nullptr,
                     IMetricsSink* metrics = nullptr);

namespace Detail
{

    /// Verify that every TLS-flagged bind in `options.binds` has a non-null
    /// `tlsContext`. Returns EXIT_SUCCESS on a clean configuration,
    /// EXIT_FAILURE (already logged at Fatal) on a TLS-flagged bind with no
    /// context — the latter would otherwise silently fall through to plaintext.
    ///
    /// Exposed for testing. The Run* entry points all call this as their
    /// first step, so a unit test exercises the same code path used in
    /// production. Lives in `Detail` so callers don't accidentally pick it
    /// up as part of the public API.
    /// @param options Server options to validate.
    /// @param logger  Logger receiving the diagnostic on failure.
    /// @return EXIT_SUCCESS / EXIT_FAILURE.
    [[nodiscard]] int VerifyTlsContextForTlsBinds(ReactorServerOptions const& options, ILogger& logger);

    /// Start this daemon's active expiry cycle on `reactor`.
    ///
    /// **One per daemon, not one per reactor.** The sweep takes each shard's
    /// exclusive lock in turn, so a second cycle would contend with the first
    /// for no gain -- reactor 0 gets it because it is the one whose loop the
    /// calling thread runs, which bounds the cycle's lifetime by the same
    /// scope.
    ///
    /// The returned owner must be declared AFTER the reactor at every call
    /// site, so it is destroyed first and takes its coroutine frame back off a
    /// timer wheel that still exists. `ExpiryReaper::Stop` explains why that
    /// matters.
    ///
    /// Exposed here, like the verifier above, because the thing worth asserting
    /// is that the daemon starts the cycle **at all**: the whole of issue #162
    /// was a `PurgeExpired` nothing called, and a reaper nothing constructs is
    /// that bug again with more code in it.
    /// @param reactor Reactor to run the cycle on.
    /// @param engine  Whose storage chain is swept -- the notifying decorator
    ///                included, or reclaimed keys are never published.
    /// @param logger  Where the cycle reports itself, at Debug.
    /// @param options Server options; only `expiry` is read.
    /// @param metrics Counter sink, or nullptr.
    /// @return The running cycle. Stops when destroyed.
    [[nodiscard]] std::unique_ptr<ExpiryReaper> StartExpiryCycle(
        IReactor& reactor, CacheEngine& engine, ILogger& logger, ReactorServerOptions const& options, IMetricsSink* metrics);

    /// Start every accept loop in `servers` and report the ones that armed.
    ///
    /// This is where "the acceptors are running" stops being an assumption. Each
    /// `Server::Run()` is driven by a detached coroutine whose first statement is
    /// the listener's `Accept()`, so it is registered with the reactor by the time
    /// the call returns -- and `Server::IsAccepting()` is what says so. A loop that
    /// ended immediately instead (a listener already closed) is logged and is **not**
    /// counted towards readiness, which is the point: the readiness line has to name
    /// acceptors that exist.
    ///
    /// Exposed for the reason `StartExpiryCycle` is: the thing worth asserting is
    /// that the daemon announces itself ready **after** doing this, and a helper
    /// nothing outside the translation unit can reach cannot be asserted about.
    /// @param servers Accept loops to start; every element must be non-null.
    /// @param group Names this group of loops in the per-acceptor Debug line, e.g.
    ///        `reactor 0`.
    /// @param announcer Receives one `AcceptorArmed` per loop that armed, and emits
    ///        the readiness line when the last one across every group does.
    /// @param logger Where a loop that did not arm is reported.
    ///
    /// Returns nothing on purpose: how many armed is `ReadinessAnnouncer`'s
    /// `ArmedCount()`, and a second answer to one question is a second thing to be
    /// wrong. A count returned here would also be read by nobody, which is what
    /// `NodeSurfaceRow`'s deleted `HostOrigin` column is recorded for -- it
    /// documented rather than drove.
    void ArmAcceptLoops(std::span<std::unique_ptr<Server> const> servers,
                        std::string_view group,
                        ReadinessAnnouncer& announcer,
                        ILogger& logger);

} // namespace Detail

} // namespace FastCache
