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
    int RunSingleReactor(ReactorServerOptions const& options,
                         CacheEngine& engine,
                         ILogger& logger,
                         IAdmissionControl* admission,
                         IMetricsSink* metrics)
    {
        FC_THREAD_NAME("fc-reactor");
        SteadyClock clock;
        PlatformReactor reactor { clock };

        auto listener = PlatformListener::Bind(reactor, options.bindAddress, options.port, options.listenBacklog);
        if (!listener || !listener->IsBound())
        {
            logger.Logf(LogLevel::Error,
                        "fastcached: cannot bind: {}",
                        listener ? listener->BindError() : std::string_view { "null listener" });
            return EXIT_FAILURE;
        }
        logger.Log(LogLevel::Info, "ready, accepting connections");

        Server server { *listener, engine, logger, admission, metrics, options.session, options.tlsContext };
        auto runAccept = [](Server* s) -> DetachedTask {
            co_await s->Run();
            co_return;
        };
        runAccept(&server);

        std::atomic<bool> watchdogQuit { false };
        auto watchdog = MakeWatchdog(watchdogQuit, [&] {
            server.Shutdown();
            reactor.Stop();
        });

        reactor.Run();
        watchdogQuit.store(true, std::memory_order_release);
        logger.Logf(LogLevel::Info, "served {} connection(s)", server.AcceptedCount());
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

    /// Windows multi-core: one blocking acceptor thread distributes raw sockets
    /// round-robin across N independent single-threaded IOCP reactors (Windows
    /// has no SO_REUSEPORT load-balancing). Each connection is pinned to the
    /// reactor it was handed to.
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

        // A plain (non-IOCP) listening socket; the acceptor blocks on it.
        auto bound = Detail::BindAndListen(
            DefaultAddressResolver(), options.bindAddress, options.port, options.listenBacklog, /*extraTypeFlags*/ 0);
        if (!bound.has_value())
        {
            logger.Logf(LogLevel::Error, "fastcached: cannot bind: {}", bound.error());
            return EXIT_FAILURE;
        }
        auto const listenSock = bound->socket;
        logger.Logf(LogLevel::Info, "ready, accepting connections ({} reactors)", reactorCount);

        std::atomic<std::uint64_t> accepted { 0 };
        std::atomic<bool> stopping { false };

        std::jthread acceptor { [&] {
            FC_THREAD_NAME("fc-acceptor");
            std::size_t next = 0;
            while (!stopping.load(std::memory_order_acquire))
            {
                auto raw = Detail::AcceptRaw(listenSock);
                if (!raw.has_value())
                    break; // listening socket closed (shutdown) or fatal accept error
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
                RunHandedOffConnection(reactor, *raw, engine, logger, admission, options.session, options.tlsContext);
            }
        } };

        std::atomic<bool> watchdogQuit { false };
        auto watchdog = MakeWatchdog(watchdogQuit, [&] {
            stopping.store(true, std::memory_order_release);
            Detail::CloseNativeSocket(listenSock); // unblock the acceptor's accept()
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

    /// POSIX multi-core: N independent single-threaded reactors, each with its
    /// own listener bound with SO_REUSEPORT on the same port. The kernel
    /// load-balances new connections across the listeners, so every connection
    /// is accepted, owned, and served entirely by one reactor — no handoff.
    int RunMultiReactorPosix(ReactorServerOptions const& options,
                             CacheEngine& engine,
                             ILogger& logger,
                             IAdmissionControl* admission,
                             IMetricsSink* metrics,
                             unsigned reactorCount)
    {
        SteadyClock clock;
        std::vector<std::unique_ptr<PlatformReactor>> reactors;
        std::vector<std::unique_ptr<PlatformListener>> listeners;
        std::vector<std::unique_ptr<Server>> servers;
        reactors.reserve(reactorCount);
        listeners.reserve(reactorCount);
        servers.reserve(reactorCount);

        for (auto i = 0U; i < reactorCount; ++i)
        {
            reactors.push_back(std::make_unique<PlatformReactor>(clock));
            auto listener = PlatformListener::Bind(*reactors[i],
                                                   options.bindAddress,
                                                   options.port,
                                                   options.listenBacklog,
                                                   DefaultAddressResolver(),
                                                   ReusePort::Yes);
            if (!listener || !listener->IsBound())
            {
                logger.Logf(LogLevel::Error,
                            "fastcached: cannot bind: {}",
                            listener ? listener->BindError() : std::string_view { "null listener" });
                return EXIT_FAILURE;
            }
            listeners.push_back(std::move(listener));
            servers.push_back(std::make_unique<Server>(
                *listeners[i], engine, logger, admission, metrics, options.session, options.tlsContext));
        }
        logger.Logf(LogLevel::Info, "ready, accepting connections ({} reactors)", reactorCount);

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
            runAccept(servers[index].get());
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
