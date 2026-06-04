// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#include <FastCache/Server/Server.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

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
    // The reactor runs to completion on this thread (reactor.Run() below), so
    // every coroutine zone in the single-threaded model lands on this row.
    FC_THREAD_NAME("fc-reactor");
    SteadyClock clock;
    PlatformReactor reactor { clock };

    auto listener = PlatformListener::Bind(reactor, options.bindAddress, options.port);
    if (!listener || !listener->IsBound())
    {
        logger.Logf(LogLevel::Error,
                    "fastcached: cannot bind: {}",
                    listener ? listener->BindError() : std::string_view { "null listener" });
        return EXIT_FAILURE;
    }

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

    reactor.Run();

    watchdogQuit.store(true, std::memory_order_release);
    logger.Logf(LogLevel::Info, "served {} connection(s)", server.AcceptedCount());
    return EXIT_SUCCESS;
}

} // namespace FastCache
