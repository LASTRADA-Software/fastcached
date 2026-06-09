// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/SocketAddress.hpp>
#include <FastCache/Platform/CpuAffinity.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Server/Connection.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#include <FastCache/Server/Server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/ResumeOn.hpp>
    #include <FastCache/Net/IocpSocket.hpp>
#elif defined(__linux__)
    #include <FastCache/Async/EpollReactor.hpp>
    #include <FastCache/Net/EpollSocket.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Async/KqueueReactor.hpp>
    #include <FastCache/Net/KqueueSocket.hpp>
#endif

#include <FastCache/Net/TlsWrap.hpp>

namespace FastCache
{

#if defined(_WIN32)
using PlatformReactor = IocpReactor;
using PlatformListener = IocpListener;
#elif defined(__linux__)
using PlatformReactor = EpollReactor;
using PlatformListener = EpollListener;
#elif defined(__APPLE__)
using PlatformReactor = KqueueReactor;
using PlatformListener = KqueueListener;
#else
    #error "No reactor implementation for this platform"
#endif

namespace
{

    /// Poll DaemonControls for a stop request and invoke `onStop` once seen.
    /// @param quit Set true by the caller to retire the watchdog.
    /// @param onStop Teardown callback (close listeners, stop reactors).
    template <typename StopFn>
    std::jthread MakeWatchdog(std::atomic<bool>& quit, StopFn onStop)
    {
        return std::jthread { [&quit, onStop = std::move(onStop)] {
            auto& controls = DaemonControls::Instance();
            while (!quit.load(std::memory_order_acquire))
            {
                if (controls.StopRequested())
                {
                    onStop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
            }
        } };
    }

    /// Single-reactor server: one event loop multiplexes every connection on
    /// this thread. No connection-concurrency ceiling and no cross-thread
    /// coroutine migration. This is the default (reactorCount == 1).
    /// Iterates `options.binds` so multiple endpoints (e.g. plaintext + TLS)
    /// share the same reactor when only one is requested.
    int RunSingleReactor(ReactorServerOptions const& options,
                         CacheEngine& engine,
                         ILogger& logger,
                         IAdmissionControl* admission,
                         IMetricsSink* metrics)
    {
        FC_THREAD_NAME("fc-reactor");
        SteadyClock clock;
        PlatformReactor reactor { clock };

        std::vector<std::unique_ptr<PlatformListener>> listeners;
        std::vector<std::unique_ptr<Server>> servers;
        listeners.reserve(options.binds.size());
        servers.reserve(options.binds.size());
        for (auto const& bind: options.binds)
        {
            auto listener = PlatformListener::Bind(reactor, bind.address, bind.port, options.listenBacklog);
            if (!listener || !listener->IsBound())
            {
                logger.Logf(LogLevel::Error,
                            "fastcached: cannot bind {}:{} : {}",
                            bind.address,
                            bind.port,
                            listener ? listener->BindError() : std::string_view { "null listener" });
                return EXIT_FAILURE;
            }
            listeners.push_back(std::move(listener));
            // Per-bind tls flag: a plaintext bind passes nullptr so accepted
            // sockets bypass the TLS wrapper; a TLS bind reuses the single
            // shared TlsContext (no per-bind cert/SNI in this iteration).
            auto* const perBindTls = bind.tls ? options.tlsContext : nullptr;
            auto session = options.session;
            session.reactor = &reactor;
            servers.push_back(std::make_unique<Server>(
                *listeners.back(), engine, logger, admission, metrics, session, perBindTls));
        }
        logger.Logf(LogLevel::Info, "ready, accepting connections ({} bind(s))", options.binds.size());

        auto runAccept = [](Server* s) -> DetachedTask {
            co_await s->Run();
            co_return;
        };
        for (auto& s: servers)
            runAccept(s.get());

        std::atomic<bool> watchdogQuit { false };
        auto watchdog = MakeWatchdog(watchdogQuit, [&] {
            for (auto& s: servers)
                s->Shutdown();
            reactor.Stop();
        });

        reactor.Run();
        watchdogQuit.store(true, std::memory_order_release);
        std::uint64_t total = 0;
        for (auto const& s: servers)
            total += s->AcceptedCount();
        logger.Logf(LogLevel::Info, "served {} connection(s)", total);
        return EXIT_SUCCESS;
    }

#if defined(_WIN32)

    /// Drive one accepted connection to completion on a specific reactor's
    /// thread. The connection is handed off from the acceptor thread: it first
    /// reschedules onto `reactor` (so the socket is created and all its I/O
    /// completions land on that one thread — no coroutine migration), then
    /// wraps the raw handle and runs the protocol session.
    DetachedTask RunHandedOffConnection(IocpReactor& reactor,
                                        Detail::NativeSocket raw,
                                        CacheEngine& engine,
                                        ILogger& logger,
                                        IAdmissionControl* admission,
                                        SessionContext session,
                                        [[maybe_unused]] TlsContext* tls)
    {
        co_await ResumeOn { reactor };
        // This connection now runs on `reactor`; pin pub/sub delivery to it so a
        // message published elsewhere wakes this subscriber via reactor.Submit.
        session.reactor = &reactor;
        // Firewall: this is a DetachedTask (unhandled_exception -> std::terminate),
        // so a handler exception must drop only this connection, not the daemon.
        try
        {
            auto socket = std::make_unique<IocpSocket>(reactor, static_cast<std::uintptr_t>(raw));
            if (!socket->IsAttached())
            {
                // CreateIoCompletionPort failed for this socket: no completion
                // will ever be dequeued, so awaiting would hang the connection
                // and leak its admission slot. Drop it now; the unique_ptr's
                // destructor closes the socket.
                logger.Logf(LogLevel::Error, "handed-off connection: IOCP association failed; dropping");
            }
            else
            {
                Connection connection { WrapTls(std::move(socket), tls), engine, logger, session };
                co_await connection.Run();
            }
        }
        catch (...)
        {
            LogConnectionFirewallException(logger);
        }
        if (admission)
            admission->OnConnectionEnded();
        co_return;
    }

    /// Windows multi-core: one blocking acceptor thread *per BindConfig*
    /// distributes raw sockets round-robin across N independent
    /// single-threaded IOCP reactors (Windows has no SO_REUSEPORT
    /// load-balancing). Each connection is pinned to the reactor it was
    /// handed to and wrapped with the bind's TLS flag.
    int RunMultiReactorWindows(ReactorServerOptions const& options,
                               CacheEngine& engine,
                               ILogger& logger,
                               IAdmissionControl* admission,
                               IMetricsSink* metrics,
                               unsigned reactorCount)
    {
        SteadyClock clock;
        std::vector<std::unique_ptr<IocpReactor>> reactors;
        reactors.reserve(reactorCount);
        for (auto i = 0U; i < reactorCount; ++i)
            reactors.push_back(std::make_unique<IocpReactor>(clock));

        // One listening socket per BindConfig; each acceptor thread owns one.
        std::vector<Detail::NativeSocket> listenSocks;
        std::vector<bool> bindTls; // parallel to listenSocks
        listenSocks.reserve(options.binds.size());
        bindTls.reserve(options.binds.size());
        for (auto const& bind: options.binds)
        {
            auto bound = Detail::BindAndListen(
                DefaultAddressResolver(), bind.address, bind.port, options.listenBacklog, /*extraTypeFlags*/ 0);
            if (!bound.has_value())
            {
                logger.Logf(LogLevel::Error, "fastcached: cannot bind {}:{} : {}", bind.address, bind.port, bound.error());
                for (auto const sock: listenSocks)
                    Detail::CloseNativeSocket(sock);
                return EXIT_FAILURE;
            }
            listenSocks.push_back(bound->socket);
            bindTls.push_back(bind.tls);
        }
        logger.Logf(LogLevel::Info,
                    "ready, accepting connections ({} bind(s) x {} reactors)",
                    options.binds.size(),
                    reactorCount);

        std::atomic<std::uint64_t> accepted { 0 };
        std::atomic<bool> stopping { false };

        std::vector<std::jthread> acceptors;
        acceptors.reserve(options.binds.size());
        for (std::size_t bindIdx = 0; bindIdx < options.binds.size(); ++bindIdx)
        {
            acceptors.emplace_back([&, bindIdx] {
                FC_THREAD_NAME(std::format("fc-acceptor-{}", bindIdx).c_str());
                std::size_t next = 0;
                auto const listenSock = listenSocks[bindIdx];
                auto* const perBindTls = bindTls[bindIdx] ? options.tlsContext : nullptr;
                while (!stopping.load(std::memory_order_acquire))
                {
                    auto raw = Detail::AcceptRaw(listenSock);
                    if (!raw.has_value())
                        break;
                    if (admission && !admission->AllowAccept())
                    {
                        if (metrics)
                            metrics->Increment(IMetricsSink::Counter::ConnectionsAdmissionRejected);
                        Detail::CloseNativeSocket(*raw);
                        continue;
                    }
                    if (admission)
                        admission->OnConnectionStarted();
                    accepted.fetch_add(1, std::memory_order_relaxed);
                    if (metrics)
                        metrics->Increment(IMetricsSink::Counter::ConnectionsTotal);
                    auto& reactor = *reactors[next % reactorCount];
                    ++next;
                    RunHandedOffConnection(reactor, *raw, engine, logger, admission, options.session, perBindTls);
                }
            });
        }

        std::atomic<bool> watchdogQuit { false };
        auto watchdog = MakeWatchdog(watchdogQuit, [&] {
            stopping.store(true, std::memory_order_release);
            for (auto const sock: listenSocks)
                Detail::CloseNativeSocket(sock);
            for (auto& reactor: reactors)
                reactor->Stop();
        });

        std::vector<std::jthread> threads;
        threads.reserve(reactorCount - 1);
        for (auto i = 1U; i < reactorCount; ++i)
            threads.emplace_back([&reactors, i] {
                FC_THREAD_NAME(std::format("fc-reactor-{}", i).c_str());
                reactors[i]->Run();
            });
        FC_THREAD_NAME("fc-reactor-0");
        reactors[0]->Run();
        threads.clear();

        watchdogQuit.store(true, std::memory_order_release);
        logger.Logf(LogLevel::Info, "served {} connection(s)", accepted.load(std::memory_order_relaxed));
        return EXIT_SUCCESS;
    }

#else

    /// POSIX multi-core: N independent single-threaded reactors, each with
    /// one listener per BindConfig (all bound with SO_REUSEPORT so the
    /// kernel load-balances connections across listeners on the same
    /// {address, port}). Every connection is accepted, owned, and served
    /// entirely by one reactor — no handoff.
    int RunMultiReactorPosix(ReactorServerOptions const& options,
                             CacheEngine& engine,
                             ILogger& logger,
                             IAdmissionControl* admission,
                             IMetricsSink* metrics,
                             unsigned reactorCount)
    {
        SteadyClock clock;
        std::vector<std::unique_ptr<PlatformReactor>> reactors;
        // Listeners and servers are laid out as `[reactor * binds + bind]`
        // — a flat array indexed by `reactor * binds.size() + bindIdx`. This
        // keeps the lifetime simple and lets the watchdog tear them down with
        // a single pass.
        std::vector<std::unique_ptr<PlatformListener>> listeners;
        std::vector<std::unique_ptr<Server>> servers;
        reactors.reserve(reactorCount);
        auto const bindCount = options.binds.size();
        listeners.reserve(reactorCount * bindCount);
        servers.reserve(reactorCount * bindCount);

        for (auto i = 0U; i < reactorCount; ++i)
        {
            reactors.push_back(std::make_unique<PlatformReactor>(clock));
            for (auto const& bind: options.binds)
            {
                auto listener = PlatformListener::Bind(
                    *reactors[i], bind.address, bind.port, options.listenBacklog, DefaultAddressResolver(), ReusePort::Yes);
                if (!listener || !listener->IsBound())
                {
                    logger.Logf(LogLevel::Error,
                                "fastcached: cannot bind {}:{} on reactor {}: {}",
                                bind.address,
                                bind.port,
                                i,
                                listener ? listener->BindError() : std::string_view { "null listener" });
                    return EXIT_FAILURE;
                }
                listeners.push_back(std::move(listener));
                auto* const perBindTls = bind.tls ? options.tlsContext : nullptr;
                auto session = options.session;
                session.reactor = reactors[i].get();
                servers.push_back(std::make_unique<Server>(
                    *listeners.back(), engine, logger, admission, metrics, session, perBindTls));
            }
        }
        logger.Logf(LogLevel::Info,
                    "ready, accepting connections ({} bind(s) x {} reactors)",
                    bindCount,
                    reactorCount);

        std::atomic<bool> watchdogQuit { false };
        auto watchdog = MakeWatchdog(watchdogQuit, [&] {
            for (auto& server: servers)
                server->Shutdown();
            for (auto& reactor: reactors)
                reactor->Stop();
        });

        auto const onlineCpus = OnlineCpuCount();
        auto runReactor = [&](unsigned index) {
            // Pin this reactor to its own core (best-effort) so its connections
            // and storage shards stay cache-resident on one core.
            if (options.pinReactorsToCpu && onlineCpus > 0)
            {
                if (!PinCallingThreadToCpu(index % onlineCpus))
                    logger.Logf(LogLevel::Warn, "reactor {}: CPU affinity not applied", index);
            }
            auto runAccept = [](Server* s) -> DetachedTask {
                co_await s->Run();
                co_return;
            };
            // Run one accept loop per bind on this reactor.
            for (std::size_t b = 0; b < bindCount; ++b)
                runAccept(servers[(index * bindCount) + b].get());
            reactors[index]->Run();
        };

        std::vector<std::jthread> threads;
        threads.reserve(reactorCount - 1);
        for (auto i = 1U; i < reactorCount; ++i)
            threads.emplace_back([&runReactor, i] {
                FC_THREAD_NAME(std::format("fc-reactor-{}", i).c_str());
                runReactor(i);
            });
        FC_THREAD_NAME("fc-reactor-0");
        runReactor(0);
        threads.clear();

        watchdogQuit.store(true, std::memory_order_release);
        std::uint64_t total = 0;
        for (auto& server: servers)
            total += server->AcceptedCount();
        logger.Logf(LogLevel::Info, "served {} connection(s)", total);
        return EXIT_SUCCESS;
    }

#endif

} // namespace

int RunReactorServer(ReactorServerOptions const& options,
                     CacheEngine& engine,
                     ILogger& logger,
                     IAdmissionControl* admission,
                     IMetricsSink* metrics)
{
    if (options.binds.empty())
    {
        logger.Log(LogLevel::Error, "fastcached: no listener configured (set --bind or --listen)");
        return EXIT_FAILURE;
    }
    auto const reactorCount = std::max(1U, options.reactorThreads);
    if (reactorCount == 1)
        return RunSingleReactor(options, engine, logger, admission, metrics);
#if defined(_WIN32)
    return RunMultiReactorWindows(options, engine, logger, admission, metrics, reactorCount);
#else
    return RunMultiReactorPosix(options, engine, logger, admission, metrics, reactorCount);
#endif
}

} // namespace FastCache
