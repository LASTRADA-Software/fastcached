// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

TEST_CASE("RunReactorServer rejects a TLS-flagged bind when no TLS context is configured",
          "[server][reactor-loop][tls-null-guard]")
{
    // Defensive contract: main.cpp validates that any TLS bind has a TLS
    // context BEFORE constructing ReactorServerOptions, but a future test
    // fixture or a refactor that builds options directly could deliver
    // `binds=[{tls=true}]` with `tlsContext=nullptr`. The unguarded code
    // would silently accept plaintext on the supposedly-TLS bind because
    // `perBindTls = bind.tls ? options.tlsContext : nullptr` collapses to
    // nullptr when the context is missing. Server then constructs
    // plaintext sockets on the TLS bind — a credential-leak hazard.
    //
    // The guard inside RunReactorServer (VerifyTlsContextForTlsBinds) is
    // the local enforcement. We verify it by constructing options with a
    // TLS-flagged bind and a null tlsContext, calling RunReactorServer
    // directly, and asserting EXIT_FAILURE without the listener ever
    // being bound (no port collision regardless of test order).
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;

    FastCache::ReactorServerOptions options;
    // Loopback + ephemeral port: the guard fires before the bind() call,
    // so we never actually open a listening socket.
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = true });
    options.tlsContext = nullptr;
    options.reactorThreads = 1;

    auto const exitCode = FastCache::RunReactorServer(options, engine, logger);
    REQUIRE(exitCode == EXIT_FAILURE);
}

TEST_CASE("Detail::VerifyTlsContextForTlsBinds accepts a plaintext bind without a TLS context",
          "[server][reactor-loop][tls-null-guard]")
{
    // Symmetric guard: the TLS check must NOT reject plaintext binds when
    // the context is null — that would break every non-TLS daemon. The
    // verifier is now exposed in the FastCache::Detail namespace so we
    // can drive it directly without spawning a real listener.
    //
    // Pre-fix this test was a SUCCEED-only stub that asserted nothing;
    // any regression tightening the guard to "context required for every
    // bind" would have passed CI while breaking every plaintext daemon.
    FastCache::CapturingLogger logger;
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = false });
    options.tlsContext = nullptr;

    auto const exitCode = FastCache::Detail::VerifyTlsContextForTlsBinds(options, logger);
    REQUIRE(exitCode == EXIT_SUCCESS);
    // No fatal diagnostic was emitted.
    REQUIRE(logger.Snapshot().empty());
}

TEST_CASE("Detail::VerifyTlsContextForTlsBinds rejects a TLS-flagged bind with no context",
          "[server][reactor-loop][tls-null-guard]")
{
    // Companion assertion to the integration test above: at the
    // primitive level, a TLS-flagged bind with a null context must
    // produce EXIT_FAILURE with a Fatal log record naming the bind.
    FastCache::CapturingLogger logger;
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 6379, .tls = true });
    options.tlsContext = nullptr;

    auto const exitCode = FastCache::Detail::VerifyTlsContextForTlsBinds(options, logger);
    REQUIRE(exitCode == EXIT_FAILURE);

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().level == FastCache::LogLevel::Fatal);
    REQUIRE(records.front().message.contains("TLS bind 127.0.0.1:6379"));
    REQUIRE(records.front().message.contains("no TLS context"));
}

TEST_CASE("RunMultiReactorWindows-style stopAll is idempotent under double invocation",
          "[server][reactor-loop][shutdown-guard]")
{
    // Finding #12: RunMultiReactorWindows invokes stopAll via two paths
    // (the watchdog onStop on SIGINT, and an unconditional call at function
    // tail). Pre-fix, both invocations ran the listener-close loop —
    // closing every SOCKET handle twice. On Windows SOCKET handles are
    // recyclable; a stale second close could land on a freshly-accepted
    // unrelated socket that happened to receive the same numeric value.
    //
    // The fix wraps stopAll in `std::atomic_flag::test_and_set`. This
    // test exercises the structural guarantee: the wrapped lambda's
    // side effects run exactly once even under repeated calls (we only
    // test the structural pattern here because the production stopAll
    // is a function-local lambda; the platform-specific behaviour is
    // exercised by the end-to-end Server tests).
    std::atomic_flag stopRun = ATOMIC_FLAG_INIT;
    int closeCount = 0;
    int stopCount = 0;
    auto stopAll = [&] {
        if (stopRun.test_and_set(std::memory_order_acq_rel))
            return;
        // Stand-in for the production "close every listenSock" + "stop
        // every reactor" body. Production calls Detail::CloseNativeSocket
        // and reactor->Stop, both of which would fire side effects.
        ++closeCount;
        ++stopCount;
    };

    stopAll();
    stopAll();
    stopAll();

    REQUIRE(closeCount == 1);
    REQUIRE(stopCount == 1);
}

TEST_CASE("Detail::VerifyTlsContextForTlsBinds accepts mixed plaintext+TLS binds when a context is set",
          "[server][reactor-loop][tls-null-guard]")
{
    // The dual-listener (plaintext + TLS) scenario: a single shared
    // TlsContext is enough for both binds; the verifier should not
    // complain. We can't construct a real TlsContext without OpenSSL
    // initialisation, but the verifier only checks the pointer for
    // nullness, so a non-null dummy address is sufficient.
    FastCache::CapturingLogger logger;
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 6379, .tls = false });
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 6380, .tls = true });
    // Non-null sentinel; the verifier only checks the pointer for nullness.
    // reinterpret_cast is intentional — the verifier never dereferences.
    options.tlsContext = reinterpret_cast<FastCache::TlsContext*>(0x1);

    auto const exitCode = FastCache::Detail::VerifyTlsContextForTlsBinds(options, logger);
    REQUIRE(exitCode == EXIT_SUCCESS);
    REQUIRE(logger.Snapshot().empty());
}

namespace
{

/// Keeps the events a keyspace subscriber would have seen, so a case can assert
/// on the notification rather than only on the item count going down. Half of
/// what issue #162 cost was that nobody was told.
class ExpireRecorder final: public FastCache::IStorageMutationObserver
{
  public:
    void OnMutation(FastCache::MutationKind kind, std::string_view key) noexcept override
    {
        if (kind == FastCache::MutationKind::Expire)
            expired.emplace_back(key);
    }

    [[nodiscard]] bool HasObservers() const noexcept override
    {
        return true;
    }

    std::vector<std::string> expired;
};

/// The daemon's storage chain, as `main.cpp` builds it: the tiers record what
/// they reclaim into a log, and the notifying decorator on top drains that once
/// their call has returned. `StartExpiryCycle` is handed `engine.Storage()`, so
/// this is what decides whether a swept key is published or merely freed.
struct DaemonChain
{
    // `InMemoryLruStorage` first, for the reason `ExpiryReaper_test`'s fixture
    // spells out: it aligns its read counters to a cache line, and a 64-aligned
    // member anywhere but the front pads the whole struct past what
    // clang-tidy's padding budget allows. Construction order still holds.
    FastCache::InMemoryLruStorage lru;
    FastCache::NullLogger logger;
    ExpireRecorder observer;
    FastCache::NotifyingStorage storage { lru, &observer };
    FastCache::ManualClock clock;
    FastCache::ReclaimLog log { &observer };
    FastCache::TestReactor reactor { clock };
    FastCache::CacheEngine engine { storage, clock };

    DaemonChain()
    {
        storage.SetReclaimLog(&log);
    }
};

} // namespace

TEST_CASE("Detail::StartExpiryCycle reclaims an untouched lapsed key through the engine's chain",
          "[server][reactor-loop][expiry]")
{
    // Issue #162 at the layer that had the bug. `ExpiryReaper` is tested on its
    // own; what is asserted here is the wiring -- that the loop starts a cycle
    // at all, and that it is pointed at `engine.Storage()` (the notifying
    // decorator) rather than at some tier below it. Sweeping the inner chain
    // would free the bytes and publish nothing, which is half the issue left in
    // place and invisible to every storage-level test.
    using namespace std::chrono_literals;
    DaemonChain chain;
    REQUIRE(chain.engine.Storage().Set("gone", FastCache::Testing::MakeBytes("v"), 0, chain.clock.Now() + 1s).has_value());
    chain.observer.expired.clear();

    FastCache::ReactorServerOptions options;
    options.expiry = FastCache::ExpiryReaperOptions { .interval = 100ms, .stopWakeBound = 25ms };

    auto const cycle = FastCache::Detail::StartExpiryCycle(chain.reactor, chain.engine, chain.logger, options, nullptr);
    REQUIRE(cycle != nullptr);
    chain.reactor.Drain();
    CHECK(chain.engine.Storage().Snapshot().itemCount == 1U); // Nothing has lapsed yet.

    // Nothing touches the key. Before the cycle existed this is where it stayed
    // resident and unreported for the life of the process.
    chain.clock.Advance(2s);
    chain.reactor.Drain();

    CHECK(chain.engine.Storage().Snapshot().itemCount == 0U);
    CHECK(chain.observer.expired == std::vector<std::string> { "gone" });
}

TEST_CASE("Detail::StartExpiryCycle honours a zero interval by starting nothing", "[server][reactor-loop][expiry]")
{
    // `--expiry-interval=0` is how an operator asks for the pre-#162 behaviour.
    // "Off" has to mean a coroutine that ended: one parked forever on a
    // deadline nothing will move is a frame the reactor has to outlive.
    using namespace std::chrono_literals;
    DaemonChain chain;
    REQUIRE(chain.engine.Storage().Set("gone", FastCache::Testing::MakeBytes("v"), 0, chain.clock.Now() + 1s).has_value());

    FastCache::ReactorServerOptions options;
    options.expiry = FastCache::ExpiryReaperOptions { .interval = FastCache::Duration::zero() };

    auto const cycle = FastCache::Detail::StartExpiryCycle(chain.reactor, chain.engine, chain.logger, options, nullptr);
    REQUIRE(cycle != nullptr);
    chain.reactor.Drain();
    chain.clock.Advance(1h);
    chain.reactor.Drain();

    CHECK(cycle->Cycles() == 0U);
    CHECK(chain.reactor.PendingTimers() == 0);
    CHECK(chain.engine.Storage().Snapshot().itemCount == 1U); // Expiry stays access-driven.
}

TEST_CASE("ReactorServerOptions defaults the expiry cycle on", "[server][reactor-loop][expiry]")
{
    // The default has to be a cycle that runs: a `ReactorServerOptions` built
    // by a caller that does not mention expiry -- which is every test fixture
    // and every future embedder -- must not silently reproduce #162.
    FastCache::ReactorServerOptions const options;
    CHECK(options.expiry.interval > FastCache::Duration::zero());
    CHECK(options.expiry.scanBudget != 0U);
    CHECK(options.expiry.purgeBudget != 0U);
}

namespace
{

/// The literal `bench/runner.py` and the packaged-service CI step both grep for.
///
/// Neither can be recompiled from this repository's build, so #646 made the line
/// name a STRONGER fact without touching its wording. Pinned by its bytes here for
/// the reason a wire constant is: a symbol both ends spell can only test the name.
constexpr std::string_view ReadyMarker = "ready, accepting connections";

/// Text every per-acceptor Debug record carries.
constexpr std::string_view ArmedMarker = "acceptor armed";

/// A free loopback TCP port, found by binding one and letting it go.
///
/// Only the multi-reactor case needs it: every reactor there binds the SAME
/// `{address, port}` under `SO_REUSEPORT`, so port `0` would hand each of them a
/// different port and the case would stop being about one shared endpoint.
/// @return A port nothing held a moment ago.
[[nodiscard]] std::uint16_t FreeLoopbackPort()
{
    auto probe = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();
    return port;
}

/// Everything one end-to-end run of `RunReactorServer` observed.
struct ServerRun
{
    std::vector<FastCache::CapturingLogger::Record> log; ///< Every record, in order.
    std::chrono::milliseconds waited { 0 };              ///< Measured cost of the readiness wait.
    int exitCode { EXIT_FAILURE };                       ///< What the loop returned.
    bool sawReady { false };                             ///< Whether the readiness line ever arrived.
};

/// Positions of the records whose message contains `needle`.
/// @param records Records in the order they were logged.
/// @param needle Substring to look for.
/// @return Ascending indices.
[[nodiscard]] std::vector<std::size_t> IndicesContaining(std::vector<FastCache::CapturingLogger::Record> const& records,
                                                         std::string_view needle)
{
    std::vector<std::size_t> found;
    for (auto const index: std::views::iota(std::size_t { 0 }, records.size()))
        if (records[index].message.contains(needle))
            found.push_back(index);
    return found;
}

/// Run `RunReactorServer` on a background thread until it announces readiness,
/// then stop it and hand back everything it logged.
///
/// The wait is bounded and reports the elapsed it MEASURED rather than the one it
/// asked for, and the two failure shapes are kept apart: a run that never announced
/// comes back with `sawReady == false` and its whole log, so a case can say which of
/// "never got there" and "got there and said the wrong thing" happened.
/// @param options Server options; `binds` must be reachable on this host.
/// @param bound How long to wait for the readiness line.
/// @return The run's exit code, its records and what the wait cost.
[[nodiscard]] ServerRun RunUntilReady(FastCache::ReactorServerOptions const& options,
                                      std::chrono::milliseconds bound = std::chrono::milliseconds { 15000 })
{
    using namespace std::chrono_literals;

    // Process-wide, and reset rather than assumed clean: `catch_discover_tests`
    // gives every case its own process, but a case that ran a server before this
    // one would leave the flag set and this run would stop before arming.
    FastCache::DaemonControls::Instance().Reset();

    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };

    ServerRun run;
    std::thread server { [&] { run.exitCode = FastCache::RunReactorServer(options, engine, logger); } };

    auto const startedAt = std::chrono::steady_clock::now();
    auto elapsed = [startedAt] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
    };
    while (elapsed() < bound)
    {
        if (!IndicesContaining(logger.Snapshot(), ReadyMarker).empty())
        {
            run.sawReady = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    run.waited = elapsed();

    FastCache::DaemonControls::Instance().RequestStop();
    server.join();
    run.log = logger.Snapshot();
    FastCache::DaemonControls::Instance().Reset();
    return run;
}

} // namespace

TEST_CASE("RunReactorServer announces readiness only after every accept loop is armed", "[server][reactor-loop][readiness]")
{
    // #646, end to end and through the production entry point. The daemon used to
    // log this line above the loop that starts the acceptors, so an assertion placed
    // after it had been given something WEAKER than the bind -- and passed anyway,
    // because the listen backlog covers the window. That is why the property under
    // test is an ORDERING in the log rather than a successful connection: a
    // connection succeeds under the bug too.
    //
    // Two binds on one reactor, each at port 0 so the OS picks and nothing in this
    // suite can collide with them.
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = false });
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = false });
    options.reactorThreads = 1;

    auto const run = RunUntilReady(options);
    INFO("waited " << run.waited.count() << "ms for the readiness line");
    REQUIRE(run.sawReady);
    CHECK(run.exitCode == EXIT_SUCCESS);

    auto const ready = IndicesContaining(run.log, ReadyMarker);
    auto const armed = IndicesContaining(run.log, ArmedMarker);
    REQUIRE(ready.size() == 1);
    // One per bind. A count is asserted as well as the order, because an ordering
    // over an empty set of arms holds vacuously -- which is what this test would
    // have degenerated into had the arms stopped being logged.
    REQUIRE(armed.size() == 2);
    CHECK(armed.back() < ready.front());
    CHECK(run.log[ready.front()].level == FastCache::LogLevel::Info);
    CHECK(run.log[ready.front()].message.contains("2 bind(s)"));
}

TEST_CASE("RunReactorServer waits for every reactor's accept loops before announcing readiness",
          "[server][reactor-loop][readiness]")
{
    // The multi-reactor path is where the pre-fix line was weakest: it was logged
    // before a single reactor thread had been CREATED. Two reactors sharing one
    // endpoint under SO_REUSEPORT, so the readiness line has to wait for both.
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = FreeLoopbackPort(), .tls = false });
    options.reactorThreads = 2;

    auto const run = RunUntilReady(options);
    INFO("waited " << run.waited.count() << "ms for the readiness line");
    REQUIRE(run.sawReady);
    CHECK(run.exitCode == EXIT_SUCCESS);

    auto const ready = IndicesContaining(run.log, ReadyMarker);
    auto const armed = IndicesContaining(run.log, ArmedMarker);
    REQUIRE(ready.size() == 1);
    REQUIRE(armed.size() == 2);
    CHECK(armed.back() < ready.front());
    CHECK(run.log[ready.front()].message.contains("1 bind(s) x 2 reactors"));
}

TEST_CASE("The daemon's readiness marker keeps the exact bytes its out-of-tree waiters grep for",
          "[server][reactor-loop][readiness]")
{
    // `bench/runner.py`'s READY_MARKER and `.github/workflows/build.yml`'s packaged-
    // service step both match this substring and neither is rebuilt from this tree,
    // so a reword here breaks two waiters silently -- one of them a CI step that
    // would then report a missing startup banner for a daemon that started fine.
    //
    // #646 deliberately changed what the line MEANS while leaving what it SAYS
    // alone, which is only safe as long as somebody is holding the bytes.
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = false });
    options.reactorThreads = 1;

    auto const run = RunUntilReady(options);
    INFO("waited " << run.waited.count() << "ms for the readiness line");
    REQUIRE(run.sawReady);

    auto const ready = IndicesContaining(run.log, ReadyMarker);
    REQUIRE(ready.size() == 1);
    CHECK(run.log[ready.front()].message.starts_with("ready, accepting connections ("));
}

TEST_CASE("RunReactorServer reports a bind it cannot make at Error and refuses to start",
          "[server][reactor-loop][bind-verdict]")
{
    // The FATAL half of #603's distinction, asserted so that "a tolerated bind
    // failure is a Warn" cannot be applied to the sites where it would be wrong.
    // These listener paths do NOT continue -- they return EXIT_FAILURE -- so Error
    // is the level that matches the verdict, and the daemon's admin endpoint (which
    // does continue) is the one that must not share it. See
    // `DescribeToleratedAdminBindFailure`.
    //
    // Provoked with RFC 5737 `192.0.2.1`, an address no host holds: it is refused on
    // every platform and needs no second listener kept alive, which is the technique
    // `AdminHttpServer_test` already uses for the same reason.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger;

    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "192.0.2.1", .port = 9, .tls = false });
    options.reactorThreads = 1;

    CHECK(FastCache::RunReactorServer(options, engine, logger) == EXIT_FAILURE);

    auto const records = logger.Snapshot();
    auto const failures = IndicesContaining(records, "cannot bind");
    REQUIRE(failures.size() == 1);
    CHECK(records[failures.front()].level == FastCache::LogLevel::Error);
    // And nothing claimed readiness for a daemon that never served anything.
    CHECK(IndicesContaining(records, ReadyMarker).empty());
}
