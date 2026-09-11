// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Net/SocketAddress.hpp>
#include <FastCache/Platform/CpuAffinity.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Server/Connection.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#include <FastCache/Server/Server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <FastCache/Async/ResumeOn.hpp>
    #include <FastCache/Net/IocpSocket.hpp>
#elif defined(__linux__)
    #include <FastCache/Net/EpollSocket.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Net/KqueueSocket.hpp>
#endif

#include <FastCache/Net/TlsWrap.hpp>

namespace FastCache
{

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
        if (int const r = Detail::VerifyTlsContextForTlsBinds(options, logger); r != EXIT_SUCCESS)
            return r;
        FC_THREAD_NAME("fc-reactor");
        SteadyClock ownClock;
        IClock& clock = options.clock != nullptr ? *options.clock : ownClock;

        // **Declared FIRST, so destroyed LAST, and since
        // [#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025) that
        // ordering carries a requirement it did not before.** It must outlive the
        // listeners and servers, which hold references to it -- but its destructor now
        // FREES the coroutine chains still parked on it, so their frames' destructors
        // run after `servers`, `expiryPool`, the reaper and the per-bind `TlsContext`
        // are already gone. Before that change those frames were never destroyed at
        // all, so nothing depended on what they touch.
        //
        // It holds as written, and the reasoning is the requirement: such a frame owns
        // its `Connection`, whose destruction reaches only the socket -- and
        // `~EpollSocket` / `~IocpSocket` call back into THIS reactor, which is alive
        // because it is the object being destroyed and a destructor body runs before
        // its members. Everything else `Connection` holds is a reference or a raw
        // pointer with a trivial destructor, and `IAdmissionControl::OnConnectionEnded`
        // sits on the `co_return` path, which a destroyed frame does not run.
        //
        // So: anything handed to a connection whose destructor does more than nothing
        // must outlive this reactor. **That sentence has a reader now, and it is not
        // this comment** ([#1051](https://github.com/LASTRADA-Software/fastcached/issues/1051)).
        // It is `ConnectionHoldings` in `Server/Connection.hpp`: everything a connection
        // is handed other than its socket goes through one type, and one
        // `static_assert` refuses that type the moment anything in it grows a
        // destructor. A comment stating an invariant nothing checks is a claim that
        // cannot fail, which is what this paragraph was until the guard existed.
        //
        // What the guard cannot see is the DECLARATION ORDER below, and that is
        // deliberate rather than a gap left open: it does not have to, because no local
        // declared here reaches a connection except by being handed to it. The guard
        // sits on the handing over, which is the narrow door, instead of on the wide
        // set of things that merely happen to be declared nearby.
        PlatformReactor reactor { clock };

        // Declared before the servers it counts and before anything that arms one,
        // so it outlives every caller and is destroyed last -- which is also what
        // makes its "readiness was never announced" line reachable on the bind-failure
        // path below.
        ReadinessAnnouncer announcer { logger, std::format("{} bind(s)", options.binds.size()) };

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
            // The endpoint's own policy travels with every connection it accepts.
            // Without this a handler cannot tell which listener a frame arrived on,
            // and "which surfaces are exposed where" stops being configurable.
            auto session = options.session;
            session.reactor = &reactor;
            // The same sink the connection counters already use, so a dispatch
            // outcome and the connection it arrived on land in one place. Null is
            // ordinary (no metrics endpoint configured) and every use is guarded.
            session.metrics = metrics;
            servers.push_back(std::make_unique<Server>(*listeners.back(),
                                                       engine,
                                                       logger,
                                                       admission,
                                                       metrics,
                                                       session,
                                                       perBindTls,
                                                       options.logSource ? LogSource::Yes : LogSource::No));
            // Registered where the participant is created, so the readiness count
            // and the thing being counted cannot disagree. See `ExpectAcceptor`.
            announcer.ExpectAcceptor();
        }
        announcer.AcceptorsAllSpawned();

        // Arm first, announce second. The readiness line used to be logged HERE,
        // above the loop that starts the acceptors (#646) -- a marker naming a fact
        // weaker than the bind a caller already had. `ReadinessAnnouncer` emits it,
        // and it emits it only once every accept loop is parked in `Accept()`.
        Detail::ArmAcceptLoops(servers, "reactor 0", announcer, logger);

        // **Declared BEFORE the reaper, and that ordering is the mechanism.** Locals
        // are destroyed in reverse, so `~ExpiryReaper` -- which cancels the cycle and
        // waits out any sweep still on this pool -- runs before `~ThreadPoolExecutor`
        // joins its thread. Reversed, the pool would be torn down under a sweep it is
        // still running. This is the shape `main.cpp` already relies on for the
        // compile pool, stated there as "~WorkerServer runs before
        // ~ThreadPoolExecutor because `server` is declared after the pool".
        //
        // One thread: the sweep is a single cycle, and a second would only let two
        // sweeps overlap on one store.
        ThreadPoolExecutor expiryPool { 1 };
        auto const expiry = Detail::StartExpiryCycle(reactor, expiryPool, engine, logger, options, metrics);

        std::atomic<bool> watchdogQuit { false };
        // **`Stop()` and NOTHING else, because this runs on the watchdog's own thread**
        // ([#1208](https://github.com/LASTRADA-Software/fastcached/issues/1208)).
        // `Server::Shutdown()` closes the listener, which reaches
        // `EpollReactor::Detach` and walks the batch `RunLoop` is dispatching -- so
        // shutting the servers down from here raced the loop by construction rather
        // than in a narrow window, since `Shutdown()` ran BEFORE `Stop()`.
        //
        // `Stop()` is the one call that is safe from another thread: it sets a flag and
        // writes the wake fd, touching nothing the loop walks.
        auto watchdog = MakeWatchdog(watchdogQuit, [&] { reactor.Stop(); });

        reactor.Run();
        watchdogQuit.store(true, std::memory_order_release);

        // **The other half, and REORDERING ALONE would not have been it.** Moving
        // `Stop()` above `Shutdown()` inside the watchdog leaves both on the watchdog's
        // thread, and `Stop()` only posts a wakeup and returns -- so the loop can still
        // be mid-batch when `Shutdown()` closes the listener. That is the gap
        // `~FrameEndpoint` fell into in #840. What makes this safe is the THREAD and the
        // moment: `Run()` has returned here, so `TeardownIsSerialisedWithDispatch()`
        // holds on its `!Running()` term, which is what `Detach` now asserts.
        //
        // It is also tidier than leaving the accept loops parked for the reactor's
        // destructor to abandon (#1025): closing the listener resumes each one inline,
        // on this thread, and it ends through its own `co_return`.
        for (auto& s: servers)
            s->Shutdown();

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
                                        [[maybe_unused]] TlsContext* tls,
                                        std::string peerAddress,
                                        LogSource logSource)
    {
        co_await ResumeOn { reactor };
        // This connection now runs on `reactor`; pin pub/sub delivery to it so a
        // message published elsewhere wakes this subscriber via reactor.Submit.
        session.reactor = &reactor;
        // Firewall: this is a DetachedTask (unhandled_exception -> std::terminate),
        // so a handler exception must drop only this connection, not the daemon.
        try
        {
            auto socket = std::make_unique<IocpSocket>(reactor, static_cast<std::uintptr_t>(raw), std::move(peerAddress));
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
                Connection connection {
                    WrapTls(std::move(socket), tls),
                    ConnectionHoldings { .engine = engine, .logger = logger, .session = session, .logSource = logSource }
                };
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
        if (int const r = Detail::VerifyTlsContextForTlsBinds(options, logger); r != EXIT_SUCCESS)
            return r;
        SteadyClock ownClock;
        IClock& clock = options.clock != nullptr ? *options.clock : ownClock;
        // Readiness on this platform takes TWO kinds of participant, and both are
        // load-bearing. An acceptor thread parked in `AcceptRaw` takes the socket; an
        // IOCP reactor's `Run()` is what ever serves it, because every accepted handle
        // is handed off. Counting only the acceptor threads would announce readiness
        // while every handed-off connection sat on a completion port nobody was
        // draining -- #646's own defect one layer in.
        //
        // Both are registered where they are CREATED rather than added up, because a
        // total stated as `binds + reactors` that came out too high would stop the
        // line ever being emitted, on the platform this cannot be executed on. See
        // `ExpectAcceptor`.
        ReadinessAnnouncer announcer { logger, std::format("{} bind(s) x {} reactors", options.binds.size(), reactorCount) };

        std::vector<std::unique_ptr<IocpReactor>> reactors;
        reactors.reserve(reactorCount);
        for (auto i = 0U; i < reactorCount; ++i)
        {
            reactors.push_back(std::make_unique<IocpReactor>(clock));
            announcer.ExpectAcceptor();
        }

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
        std::atomic<std::uint64_t> accepted { 0 };
        std::atomic<bool> stopping { false };
        // ONE round-robin counter shared across all acceptor threads. The
        // previous per-acceptor `std::size_t next = 0` per lambda meant
        // every bind's first connections landed on reactors[0], because
        // each acceptor's counter was independent — under steady low-rate
        // multi-bind load, reactors 0 and 1 silently received ~2x the
        // connections of reactors 2 and 3. A single atomic counter
        // restores the round-robin contract the leading comment promises.
        std::atomic<std::size_t> nextReactor { 0 };

        std::vector<std::jthread> acceptors;
        acceptors.reserve(options.binds.size());
        for (auto const bindIdx: std::views::iota(std::size_t { 0 }, options.binds.size()))
        {
            announcer.ExpectAcceptor();
            acceptors.emplace_back([&, bindIdx](std::stop_token stopToken) {
                // Hoist the formatted thread name into a stack local: Tracy's
                // SetThreadName stores the const char* and reads it on later
                // zone records, so a `std::format(...).c_str()` would dangle
                // immediately after the full-expression's semicolon.
                [[maybe_unused]] auto const threadName = std::format("fc-acceptor-{}", bindIdx);
                FC_THREAD_NAME(threadName.c_str());
                auto const listenSock = listenSocks[bindIdx];
                auto* const perBindTls = bindTls[bindIdx] ? options.tlsContext : nullptr;
                // Armed the moment this thread is about to block in `AcceptRaw`.
                announcer.AcceptorArmed(std::format("acceptor thread {}", bindIdx));
                while (!stopping.load(std::memory_order_acquire) && !stopToken.stop_requested())
                {
                    auto raw = Detail::AcceptRaw(listenSock);
                    if (!raw.has_value())
                        break;
                    if (admission && !admission->AllowAccept())
                    {
                        if (metrics)
                        {
                            metrics->Increment(IMetricsSink::Counter::ConnectionsAdmissionRejected);
                            if (perBindTls != nullptr)
                                metrics->Increment(IMetricsSink::Counter::ConnectionsAdmissionRejectedTls);
                        }
                        Detail::CloseNativeSocket(*raw);
                        continue;
                    }
                    if (admission)
                        admission->OnConnectionStarted();
                    accepted.fetch_add(1, std::memory_order_relaxed);
                    if (metrics)
                    {
                        metrics->Increment(IMetricsSink::Counter::ConnectionsTotal);
                        if (perBindTls != nullptr)
                            metrics->Increment(IMetricsSink::Counter::ConnectionsTotalTls);
                    }
                    auto const idx = nextReactor.fetch_add(1, std::memory_order_relaxed) % reactorCount;
                    auto& reactor = *reactors[idx];
                    // AcceptRaw hands off a connected handle without a captured
                    // peer; query it now (on the acceptor thread) so --log-source
                    // can prefix this connection's log lines with the client IP.
                    auto peer = options.logSource ? Detail::PeerAddressOf(*raw) : std::string {};
                    // Per-bind copy for the same reason the other two platform paths
                    // make one: this connection carries its endpoint's role mask, not
                    // the daemon's.
                    auto session = options.session;
                    RunHandedOffConnection(reactor,
                                           *raw,
                                           engine,
                                           logger,
                                           admission,
                                           session,
                                           perBindTls,
                                           std::move(peer),
                                           options.logSource ? LogSource::Yes : LogSource::No);
                }
            });
        }

        // Both spawn loops have run, so the set is complete. The reactor arms land
        // later, on their own threads, and the last of them announces.
        announcer.AcceptorsAllSpawned();

        std::atomic<bool> watchdogQuit { false };
        // Single-shot guard: stopAll is invoked both by the watchdog (on
        // SIGINT/SIGTERM) and unconditionally at the function tail. Without
        // a guard, every listenSock would be closed twice — and on Windows
        // SOCKET handles are recyclable, so a second closesocket on a stale
        // handle could tear down a freshly-accepted unrelated socket that
        // happened to receive the same numeric value. test_and_set is
        // atomic and idempotent — exactly one stopAll path actually runs
        // the teardown.
        //
        // **Closing these while an acceptor thread is parked in `AcceptRaw` is
        // deliberate, and it is the only thing that ends those threads.** The comment
        // above addresses double-close and says nothing about close-under-accept, which
        // reads like the second hazard went unexamined — it was
        // ([#1238](https://github.com/LASTRADA-Software/fastcached/issues/1238)), and
        // this paragraph is here because the next reader will arrive at the same
        // suspicion from the same comment.
        //
        // Measured on Windows, 3 runs: `closesocket` under a parked `::accept` returns
        // `WSAEINTR` → `NetErrorCode::Cancelled`, the acceptor's `!raw.has_value()`
        // breaks its loop, and `~jthread` joins. It is NOT the hazard #1207 fixed on
        // `BlockingListener`: that remedy is *stop, JOIN, then close*, and it rests on
        // POSIX NOT waking a parked `accept` (measured: still parked 12.5 s after the
        // close), so closing early buys nothing there. Here the close is the wakeup, so
        // joining first would hang forever. The POSIX sibling below can join-then-close
        // only because its acceptors are reactor-driven `PlatformListener`s. Pinned by
        // `ctest -R AcceptRaw`, which had no equivalent when #1238 was filed.
        //
        // What is NOT closed by this: an acceptor that has just passed its `stopping`
        // check can still call `AcceptRaw` on a handle `stopAll` closed a moment later.
        // No flag can remove that window — there is always a gap between the check and
        // the syscall — which is why the close, not the flag, is the stop mechanism;
        // the flag only saves a doomed syscall. The outcome there is `WSAENOTSOCK` →
        // `BadFileHandle`, so the loop breaks anyway, which is the intended end.
        std::atomic_flag stopRun = ATOMIC_FLAG_INIT;
        auto stopAll = [&] {
            if (stopRun.test_and_set(std::memory_order_acq_rel))
                return; // another caller already ran the teardown.
            stopping.store(true, std::memory_order_release);
            for (auto const sock: listenSocks)
                Detail::CloseNativeSocket(sock);
            for (auto& reactor: reactors)
                reactor->Stop();
        };
        auto watchdog = MakeWatchdog(watchdogQuit, stopAll);

        // **Declared BEFORE the reaper, and that ordering is the mechanism.** Locals
        // are destroyed in reverse, so `~ExpiryReaper` -- which cancels the cycle and
        // waits out any sweep still on this pool -- runs before `~ThreadPoolExecutor`
        // joins its thread. Reversed, the pool would be torn down under a sweep it is
        // still running. This is the shape `main.cpp` already relies on for the
        // compile pool, stated there as "~WorkerServer runs before
        // ~ThreadPoolExecutor because `server` is declared after the pool".
        //
        // One thread: the sweep is a single cycle, and a second would only let two
        // sweeps overlap on one store.
        ThreadPoolExecutor expiryPool { 1 };
        auto const expiry = Detail::StartExpiryCycle(*reactors[0], expiryPool, engine, logger, options, metrics);

        std::vector<std::jthread> threads;
        threads.reserve(reactorCount - 1);
        for (auto i = 1U; i < reactorCount; ++i)
            threads.emplace_back([&reactors, &announcer, i] {
                [[maybe_unused]] auto const threadName = std::format("fc-reactor-{}", i);
                FC_THREAD_NAME(threadName.c_str());
                announcer.AcceptorArmed(std::format("reactor {}", i));
                reactors[i]->Run();
            });
        FC_THREAD_NAME("fc-reactor-0");
        announcer.AcceptorArmed("reactor 0");
        reactors[0]->Run();
        threads.clear();

        // Unconditional cleanup: if reactors[0]->Run() returned through any
        // path other than the watchdog firing (test calls Stop() directly,
        // an unhandled exception unwinds, the IOCP completion port is
        // externally closed), the watchdog quits via watchdogQuit without
        // ever invoking onStop — leaving listenSocks open and the acceptor
        // jthreads blocked in AcceptRaw forever. Calling stopAll() here
        // closes the sockets so AcceptRaw returns WSAEINTR and the acceptor
        // lambdas exit; ~jthread joins cleanly. (This said `WSAEINTR/EBADF`;
        // the POSIX half describes neither this block, which is Windows-only,
        // nor a parked accept, which POSIX does not wake at all — #1238.)
        stopAll();
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
        if (int const r = Detail::VerifyTlsContextForTlsBinds(options, logger); r != EXIT_SUCCESS)
            return r;
        SteadyClock ownClock;
        IClock& clock = options.clock != nullptr ? *options.clock : ownClock;
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

        // Declared before the servers it counts and before any reactor thread, so it
        // outlives every caller of `AcceptorArmed`.
        ReadinessAnnouncer announcer { logger, std::format("{} bind(s) x {} reactors", bindCount, reactorCount) };

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
                // The endpoint's own policy travels with every connection it accepts.
                // Without this a handler cannot tell which listener a frame arrived on,
                // and "which surfaces are exposed where" stops being configurable.
                auto session = options.session;
                session.reactor = reactors[i].get();
                session.metrics = metrics;
                servers.push_back(std::make_unique<Server>(*listeners.back(),
                                                           engine,
                                                           logger,
                                                           admission,
                                                           metrics,
                                                           session,
                                                           perBindTls,
                                                           options.logSource ? LogSource::Yes : LogSource::No));
                // One participant per (reactor, bind) pair, registered where the
                // pair is created rather than multiplied out afterwards.
                announcer.ExpectAcceptor();
            }
        }
        // The set is complete before any reactor thread exists. The arms land later,
        // each on its own reactor's thread, and the last one announces -- which used
        // to be logged HERE, before a single reactor thread had been created (#646).
        announcer.AcceptorsAllSpawned();

        std::atomic<bool> watchdogQuit { false };
        // `Stop()` and nothing else -- the same rule as the single-reactor path, and
        // this is the second of the two sites #1208 reported. The servers are shut
        // down after every loop has returned, below.
        auto watchdog = MakeWatchdog(watchdogQuit, [&] {
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
            // Run one accept loop per bind on this reactor, and report them armed.
            // Arming happens on THIS thread and immediately before `Run()`, which is
            // why the readiness line cannot be emitted from the calling thread: it
            // would have to be emitted before the last reactor even started.
            auto const group = std::format("reactor {}", index);
            Detail::ArmAcceptLoops(
                std::span<std::unique_ptr<Server> const> { servers }.subspan(index * bindCount, bindCount),
                group,
                announcer,
                logger);
            reactors[index]->Run();
        };

        // **Declared BEFORE the reaper, and that ordering is the mechanism.** Locals
        // are destroyed in reverse, so `~ExpiryReaper` -- which cancels the cycle and
        // waits out any sweep still on this pool -- runs before `~ThreadPoolExecutor`
        // joins its thread. Reversed, the pool would be torn down under a sweep it is
        // still running. This is the shape `main.cpp` already relies on for the
        // compile pool, stated there as "~WorkerServer runs before
        // ~ThreadPoolExecutor because `server` is declared after the pool".
        //
        // One thread: the sweep is a single cycle, and a second would only let two
        // sweeps overlap on one store.
        ThreadPoolExecutor expiryPool { 1 };
        auto const expiry = Detail::StartExpiryCycle(*reactors[0], expiryPool, engine, logger, options, metrics);

        std::vector<std::jthread> threads;
        threads.reserve(reactorCount - 1);
        for (auto i = 1U; i < reactorCount; ++i)
            threads.emplace_back([&runReactor, i] {
                // Hoist into a stack local; Tracy's SetThreadName does not
                // copy and would dangle on a `std::format(...).c_str()`.
                [[maybe_unused]] auto const threadName = std::format("fc-reactor-{}", i);
                FC_THREAD_NAME(threadName.c_str());
                runReactor(i);
            });
        FC_THREAD_NAME("fc-reactor-0");
        runReactor(0);
        threads.clear();

        watchdogQuit.store(true, std::memory_order_release);

        // Every loop has returned -- `runReactor(0)` above and `threads.clear()`, which
        // joins -- so no reactor is `Running()` and closing a listener here cannot walk
        // a batch anybody is dispatching. See the single-reactor path for why the order
        // rather than the thread was never the fix (#1208).
        for (auto& server: servers)
            server->Shutdown();

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

namespace Detail
{

    std::unique_ptr<ExpiryReaper> StartExpiryCycle(IReactor& reactor,
                                                   IExecutor& sweepOn,
                                                   CacheEngine& engine,
                                                   ILogger& logger,
                                                   ReactorServerOptions const& options,
                                                   IMetricsSink* metrics)
    {
        auto reaper = std::make_unique<ExpiryReaper>(engine.Storage(), logger, options.expiry, metrics);
        reaper->Start(reactor, sweepOn);
        return reaper;
    }

    void ArmAcceptLoops(std::span<std::unique_ptr<Server> const> servers,
                        std::string_view group,
                        ReadinessAnnouncer& announcer,
                        ILogger& logger)
    {
        // A DetachedTask has a `suspend_never` initial suspend, so calling this runs
        // `Server::Run()` up to its first real suspension point -- which is the
        // listener's `Accept()`. That is what turns "started" into "armed", and it
        // is the whole reason the readiness line can be emitted below rather than
        // above.
        auto runAccept = [](Server* s) -> DetachedTask {
            co_await s->Run();
            co_return;
        };

        for (auto const index: std::views::iota(std::size_t { 0 }, servers.size()))
        {
            auto* const server = servers[index].get();
            runAccept(server);
            if (!server->IsAccepting())
            {
                // Not counted, deliberately. The loop ended before parking, so there
                // is no acceptor here -- and a readiness line that counted it would
                // be exactly the claim #646 is about, made one level deeper.
                logger.Logf(
                    LogLevel::Error, "{}: accept loop {} did not arm; this endpoint is not being served", group, index);
                continue;
            }
            announcer.AcceptorArmed(std::format("{} bind {}", group, index));
        }
    }

    int VerifyTlsContextForTlsBinds(ReactorServerOptions const& options, ILogger& logger)
    {
        for (auto const& bind: options.binds)
        {
            if (bind.tls && options.tlsContext == nullptr)
            {
                logger.Logf(LogLevel::Fatal,
                            "fastcached: TLS bind {}:{} requested but no TLS context configured",
                            bind.address,
                            bind.port);
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    }

} // namespace Detail

} // namespace FastCache
