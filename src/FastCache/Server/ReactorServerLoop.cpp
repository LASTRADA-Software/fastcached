// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#include <FastCache/Server/Server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <ranges>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Net/IocpSocket.hpp>
#elif defined(__linux__)
    #include <FastCache/Async/EpollReactor.hpp>
    #include <FastCache/Net/EpollSocket.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Async/KqueueReactor.hpp>
    #include <FastCache/Net/KqueueSocket.hpp>
#endif

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

int RunReactorServer(ReactorServerOptions const& options,
                     CacheEngine& engine,
                     ILogger& logger,
                     IAdmissionControl* admission,
                     IMetricsSink* metrics)
{
    // The reactor runs on this thread (reactor.Run() below) plus, on a
    // multi-thread-capable reactor, a small pool of sibling threads — every
    // coroutine zone lands on whichever of those rows resumed it.
    FC_THREAD_NAME("fc-reactor");
    SteadyClock clock;

    // Only the Windows IOCP reactor is safe to drain from multiple threads
    // today; epoll/kqueue stay single-threaded until they grow per-fd
    // one-shot re-arming. Clamp the requested thread count accordingly.
#if defined(_WIN32)
    auto const reactorThreads = std::max(1U, options.reactorThreads);
    PlatformReactor reactor { clock, reactorThreads };
#else
    auto const reactorThreads = 1U;
    PlatformReactor reactor { clock };
#endif

    auto listener = PlatformListener::Bind(reactor, options.bindAddress, options.port, options.listenBacklog);
    if (!listener || !listener->IsBound())
    {
        logger.Logf(LogLevel::Error,
                    "fastcached: cannot bind: {}",
                    listener ? listener->BindError() : std::string_view { "null listener" });
        return EXIT_FAILURE;
    }
    logger.Log(LogLevel::Info, "ready, accepting connections");

    Server server { *listener, engine, logger, admission, metrics };

    // Fire the accept loop as a detached coroutine. DetachedTask's
    // initial_suspend=suspend_never starts the body inline; the first
    // co_await suspends on the listener and the reactor takes over.
    auto runAccept = [](Server* s) -> DetachedTask {
        co_await s->Run();
        co_return;
    };
    runAccept(&server);

    // Watchdog: poll DaemonControls for the stop request from a sidecar
    // thread. When set, shutdown the server (closes the listener) and
    // stop the reactor.
    std::atomic<bool> watchdogQuit { false };
    std::jthread watchdog { [&] {
        auto& controls = DaemonControls::Instance();
        while (!watchdogQuit.load(std::memory_order_acquire))
        {
            if (controls.StopRequested())
            {
                server.Shutdown();
                reactor.Stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
        }
    } };

    // Spawn the sibling worker threads (if any); this thread is one of the
    // reactorThreads drainers. Each thread independently pulls completions
    // from the shared port; the first Stop() chains a wake-up through all of
    // them. A blocking storage fsync on one thread leaves the rest free to
    // accept and serve other connections.
    std::vector<std::jthread> workers;
    workers.reserve(reactorThreads - 1);
    for (auto const index: std::views::iota(1U, reactorThreads))
        workers.emplace_back([&reactor, index] {
            // `index` only feeds the Tracy thread name; reference it
            // unconditionally so the capture isn't flagged unused when Tracy
            // is compiled out and FC_THREAD_NAME discards its argument.
            static_cast<void>(index);
            FC_THREAD_NAME(std::format("fc-reactor-{}", index).c_str());
            reactor.Run();
        });

    reactor.Run();
    workers.clear(); // join every sibling drainer before tearing down.

    watchdogQuit.store(true, std::memory_order_release);
    logger.Logf(LogLevel::Info, "served {} connection(s)", server.AcceptedCount());
    return EXIT_SUCCESS;
}

} // namespace FastCache
