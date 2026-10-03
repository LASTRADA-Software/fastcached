// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/NetError.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/Unwrap.hpp>

namespace
{

/// Whether `RunReactorServer` can be called with @p Args after its options, engine and logger --
/// dependent on a template parameter, so an ill-formed call is `false` rather than a hard error.
template <typename... Args>
constexpr bool RunnableWith = requires(FastCache::ReactorServerOptions const& options,
                                       FastCache::CacheEngine& engine,
                                       FastCache::ILogger& logger,
                                       Args&... args) { FastCache::RunReactorServer(options, engine, logger, args...); };

// The `/healthz` registry is a REQUIRED argument. It used to be `ReactorServerOptions::acceptLoops`,
// null unless somebody set it, so a caller that forgot compiled and every accept loop's end reached
// no `/healthz`. A default for it brings that back, and fails here.
static_assert(!RunnableWith<>);
static_assert(RunnableWith<core::net::AcceptLoopHealth>);

} // namespace

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
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;

    FastCache::ReactorServerOptions options;
    // Loopback + ephemeral port: the guard fires before the bind() call,
    // so we never actually open a listening socket.
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = true });
    options.tlsContext = nullptr;
    options.reactorThreads = 1;

    core::net::AcceptLoopHealth acceptLoops;
    auto const exitCode = FastCache::RunReactorServer(options, engine, logger, acceptLoops);
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
    options.tlsContext = reinterpret_cast<core::net::ITlsContext*>(0x1);

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
    core::platform::ManualClock clock;
    FastCache::ReclaimLog log { &observer };
    core::net::testing::TestLoop reactor { clock };
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
    REQUIRE(chain.engine.Storage().Set("gone", FastCache::Testing::MakeBytes("v"), 0, chain.clock.now() + 1s).has_value());
    chain.observer.expired.clear();

    FastCache::ReactorServerOptions options;
    options.expiry = FastCache::ExpiryReaperOptions { .interval = 100ms };

    auto const cycle =
        FastCache::Detail::StartExpiryCycle(chain.reactor, chain.reactor, chain.engine, chain.logger, options, nullptr);
    REQUIRE(cycle != nullptr);
    chain.reactor.drain();
    CHECK(chain.engine.Storage().Snapshot().itemCount == 1U); // Nothing has lapsed yet.

    // Nothing touches the key. Before the cycle existed this is where it stayed
    // resident and unreported for the life of the process.
    chain.clock.advance(2s);
    chain.reactor.drain();

    CHECK(chain.engine.Storage().Snapshot().itemCount == 0U);
    CHECK(chain.observer.expired == std::vector<std::string> { "gone" });
}

TEST_CASE("Detail::StartExpiryCycle honours a zero interval by starting nothing", "[server][reactor-loop][expiry]")
{
    // `--expiry-interval=0s` is how an operator asks for the pre-#162 behaviour.
    // "Off" has to mean a coroutine that ended: one parked forever on a
    // deadline nothing will move is a frame the reactor has to outlive.
    using namespace std::chrono_literals;
    DaemonChain chain;
    REQUIRE(chain.engine.Storage().Set("gone", FastCache::Testing::MakeBytes("v"), 0, chain.clock.now() + 1s).has_value());

    FastCache::ReactorServerOptions options;
    options.expiry = FastCache::ExpiryReaperOptions { .interval = core::platform::SteadyDuration::zero() };

    auto const cycle =
        FastCache::Detail::StartExpiryCycle(chain.reactor, chain.reactor, chain.engine, chain.logger, options, nullptr);
    REQUIRE(cycle != nullptr);
    chain.reactor.drain();
    chain.clock.advance(1h);
    chain.reactor.drain();

    CHECK(cycle->Cycles() == 0U);
    CHECK(chain.reactor.pendingTimers() == 0);
    CHECK(chain.engine.Storage().Snapshot().itemCount == 1U); // Expiry stays access-driven.
}

TEST_CASE("ReactorServerOptions defaults the expiry cycle on", "[server][reactor-loop][expiry]")
{
    // The default has to be a cycle that runs: a `ReactorServerOptions` built
    // by a caller that does not mention expiry -- which is every test fixture
    // and every future embedder -- must not silently reproduce #162.
    FastCache::ReactorServerOptions const options;
    CHECK(options.expiry.interval > core::platform::SteadyDuration::zero());
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

/// The `(armed/expected)` progress an `acceptor armed` record carries.
struct ArmProgress
{
    std::size_t armed { 0 };    ///< How many had armed when this line was written.
    std::size_t expected { 0 }; ///< How many were registered to arm.
};

/// Read the trailing `(n/m)` off an `acceptor armed` record.
///
/// The point is to take the participant count FROM THE RUNNING PROCESS rather than
/// restate it here. How many participants a startup has is platform-specific -- on
/// POSIX the multi-reactor path arms one accept loop per (reactor, bind) pair, and
/// on Windows it arms one per acceptor THREAD plus one per IOCP reactor, because
/// there a handed-off connection is served by the reactor rather than by the
/// acceptor. A test that hard-codes either number is a second place the arithmetic
/// lives, and it was wrong: `armed.size() == 2` passed on Linux and failed on all
/// three Windows legs with `3 == 2`, which is exactly the defect the production
/// change had just been reworked to remove.
/// @param message An `acceptor armed: X (n/m)` log message.
/// @return The pair, or nullopt when the message does not carry one.
[[nodiscard]] std::optional<ArmProgress> ArmProgressOf(std::string_view message)
{
    auto const open = message.rfind('(');
    auto const slash = message.rfind('/');
    auto const close = message.rfind(')');
    if (open == std::string_view::npos || slash == std::string_view::npos || close == std::string_view::npos)
        return std::nullopt;
    if (!(open < slash && slash < close))
        return std::nullopt;

    ArmProgress progress;
    auto const armed = message.substr(open + 1, slash - open - 1);
    auto const expected = message.substr(slash + 1, close - slash - 1);
    if (std::from_chars(armed.data(), armed.data() + armed.size(), progress.armed).ec != std::errc {})
        return std::nullopt;
    if (std::from_chars(expected.data(), expected.data() + expected.size(), progress.expected).ec != std::errc {})
        return std::nullopt;
    return progress;
}

/// Everything one end-to-end run of `RunReactorServer` observed.
struct ServerRun
{
    std::vector<FastCache::CapturingLogger::Record> log; ///< Every record, in order.
    FastCache::Testing::WaitOutcome ready {};            ///< The readiness wait: whether the line arrived, and its cost.
    int exitCode { EXIT_FAILURE };                       ///< What the loop returned.
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

/// Assert that the daemon announced readiness once, and that every acceptor it
/// registered actually armed.
///
/// ## Why there is no assertion on the ORDER of these records
///
/// Two attempts at one lived here and both were wrong, in ways only a counted
/// stress run showed -- `ctest --repeat until-fail` reports the LAST iteration, so
/// it said nothing; a loop counting outcomes found 2 failures in 200.
///
///   - `armed.back() < ready.front()` assumed log position is arm order. Arming is
///     a `fetch_add` and a separate `Logf`, so a thread holding the second slot can
///     reach the logger first. Measured: `1 == 2`.
///   - Then "the record carrying `(m/m)` must precede the readiness line", on the
///     reasoning that the thread completing the count logs its own line and then
///     announces. Also wrong, and this is the interesting one: `MaybeAnnounce`
///     reads the CURRENT count rather than the value its own call produced, so the
///     announcer can be a thread that took an earlier slot -- it logs `(1/2)`, sees
///     that the total has since been reached, and announces while its sibling's
///     `(2/2)` line is still on its way to the logger. Measured: `3 < 1`.
///
/// **Readiness is not violated in either case**: the counter reached the total
/// before anything was announced, which is exactly what #646 asks for. What is not
/// witnessable from a log is the ORDER, because the arming and the reporting of it
/// are two steps and only the first one is what readiness depends on.
///
/// So the ordering property is asserted where it can be asserted deterministically
/// -- `ReadinessAnnouncer_test` drives it single-threaded and proves nothing is
/// announced until the last arm -- and this checks the WIRING: that the loop
/// registered every participant it created, that all of them armed, and that the
/// daemon said so once. The single-reactor case below adds the strict ordering,
/// which is sound there because that path arms on one thread.
/// @param log The run's records, in order.
void CheckReadinessComplete(std::vector<FastCache::CapturingLogger::Record> const& log)
{
    auto const ready = IndicesContaining(log, ReadyMarker);
    auto const armed = IndicesContaining(log, ArmedMarker);

    REQUIRE(ready.size() == 1);
    // Non-emptiness as well as the rest, because every check below holds vacuously
    // over an empty set of arms -- which is what this would degenerate into if the
    // arm records stopped being logged.
    REQUIRE_FALSE(armed.empty());

    // The total is the largest `expected` any arm reported. Not simply the first
    // record's: on the Windows path the acceptor threads are spawned before the set
    // is closed, so an early arm can legitimately report a total that is still
    // growing.
    std::size_t expected = 0;
    std::vector<std::size_t> positions;
    for (auto const index: armed)
    {
        auto const progress = ArmProgressOf(log[index].message);
        REQUIRE(progress.has_value());
        auto const& parsed = FastCache::Testing::Unwrap(progress);
        expected = std::max(expected, parsed.expected);
        positions.push_back(parsed.armed);
    }

    CHECK(expected != 0);
    CHECK(armed.size() == expected);

    // Every slot from 1 to the total, exactly once. Order-independent by
    // construction, which is the point: it catches a participant that never armed
    // and one that armed twice without caring which thread reached the logger first.
    std::ranges::sort(positions);
    std::vector<std::size_t> want(expected);
    std::ranges::copy(std::views::iota(std::size_t { 1 }, expected + 1), want.begin());
    CHECK(positions == want);
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

    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };

    core::net::AcceptLoopHealth acceptLoops;
    ServerRun run;
    std::thread server { [&] { run.exitCode = FastCache::RunReactorServer(options, engine, logger, acceptLoops); } };

    // Every 5 ms rather than the helper's default rest: each look copies the whole log.
    run.ready = FastCache::Testing::WaitUntilOutcome(
        "the readiness line",
        [&logger] { return !IndicesContaining(logger.Snapshot(), ReadyMarker).empty(); },
        [&logger] { return std::format("{} record(s) logged", logger.Snapshot().size()); },
        FastCache::Testing::WaitOptions { .step = {}, .context = {}, .bound = bound, .rest = 5ms });

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
    INFO("waited " << run.ready.elapsed.count() << "ms for the readiness line");
    REQUIRE(FastCache::Testing::Reached(run.ready));
    CHECK(run.exitCode == EXIT_SUCCESS);

    CheckReadinessComplete(run.log);

    auto const ready = IndicesContaining(run.log, ReadyMarker);
    auto const armed = IndicesContaining(run.log, ArmedMarker);
    // Strict ordering, and it is sound HERE and only here: the single-reactor path
    // arms every accept loop from one thread, so log position is arm order. This is
    // the closest thing in the suite to the original defect, which was a readiness
    // line sitting literally above the loop that starts the acceptors.
    REQUIRE_FALSE(armed.empty());
    CHECK(armed.back() < ready.front());
    CHECK(run.log[ready.front()].level == FastCache::LogLevel::Info);
    // The single-reactor path arms one accept loop per bind on every platform, so
    // this figure is the same everywhere and is the endpoint summary rather than a
    // participant count.
    CHECK(run.log[ready.front()].message.contains("2 bind(s)"));
    // Each loop armed under the name `RunReactorServer` gave its `Server` -- its bind's, which is
    // what its warnings and the `/healthz` registry say too. This is the wiring the `Server` and
    // `BindSurface` cases cannot see: a construction site passing any other name goes red here.
    CHECK(std::ranges::all_of(armed, [&run](std::size_t index) {
        return run.log[index].message.starts_with(std::format("{}: cache 127.0.0.1:0 (", ArmedMarker));
    }));
}

TEST_CASE("RunReactorServer waits for every reactor's accept loops before announcing readiness",
          "[server][reactor-loop][readiness]")
{
    // The multi-reactor path is where the pre-fix line was weakest: it was logged
    // before a single reactor thread had been CREATED, so the readiness line has to
    // wait for both of them.
    //
    // Port 0, so the OS picks and nothing in this suite can contend for it. This
    // case first asked for one shared port -- found by binding a probe and letting
    // it go -- on the reasoning that production's reactors share one endpoint under
    // `SO_REUSEPORT`. That reasoning was about production and not about the property
    // here, and it bought an intermittent failure: `bind(0)` hands back a port
    // INSIDE `ip_local_port_range` (32768+ on Linux), which any other test's
    // outbound connection can take between the probe closing and the server binding.
    // It did, once, in a `-j24` run -- the tell being a 0.13s failure, which is a
    // bind error rather than the 15s readiness timeout. `Config.hpp` already carries
    // that rule for the daemon's own default port.
    //
    // What this case asserts is that the readiness line waits for EVERY reactor's
    // accept loops, and two reactors on two ports exercise that identically: each
    // still arms its own loops on its own thread, and the last one still announces.
    // Sharing one port is production's arrangement, not this property, and no
    // assertion below reads the port at all.
    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "127.0.0.1", .port = 0, .tls = false });
    options.reactorThreads = 2;

    auto const run = RunUntilReady(options);
    INFO("waited " << run.ready.elapsed.count() << "ms for the readiness line");
    REQUIRE(FastCache::Testing::Reached(run.ready));
    CHECK(run.exitCode == EXIT_SUCCESS);

    CheckReadinessComplete(run.log);

    // The endpoint summary, which is what the operator reads. How many PARTICIPANTS
    // that took is platform-specific and is asserted against the announcer's own
    // figure inside the helper above, never against a literal here.
    auto const ready = IndicesContaining(run.log, ReadyMarker);
    CHECK(run.log[ready.front()].message.contains("1 bind(s) x 2 reactors"));

    // And the bind's loops armed under the names the multi-reactor path gave them -- the names their
    // warnings and the `/healthz` registry use. POSIX runs one `Server` per (reactor, bind), each
    // naming its reactor; Windows runs one acceptor thread per bind.
    auto const armedAs = [&run](std::string_view surface) {
        auto const prefix = std::format("{}: {} (", ArmedMarker, surface);
        return std::ranges::count_if(run.log, [&prefix](FastCache::CapturingLogger::Record const& record) {
            return record.message.starts_with(prefix);
        });
    };
#if defined(_WIN32)
    CHECK(armedAs("cache 127.0.0.1:0") == 1);
#else
    CHECK(armedAs("cache 127.0.0.1:0 on reactor 0") == 1);
    CHECK(armedAs("cache 127.0.0.1:0 on reactor 1") == 1);
#endif
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
    INFO("waited " << run.ready.elapsed.count() << "ms for the readiness line");
    REQUIRE(FastCache::Testing::Reached(run.ready));

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
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger;

    FastCache::ReactorServerOptions options;
    options.binds.push_back(FastCache::BindConfig { .address = "192.0.2.1", .port = 9, .tls = false });
    options.reactorThreads = 1;

    core::net::AcceptLoopHealth acceptLoops;
    CHECK(FastCache::RunReactorServer(options, engine, logger, acceptLoops) == EXIT_FAILURE);

    auto const records = logger.Snapshot();
    auto const failures = IndicesContaining(records, "cannot bind");
    REQUIRE(failures.size() == 1);
    CHECK(records[failures.front()].level == FastCache::LogLevel::Error);
    // And nothing claimed readiness for a daemon that never served anything.
    CHECK(IndicesContaining(records, ReadyMarker).empty());
}

TEST_CASE("Detail::ArmAcceptLoops does not count an accept loop that never armed", "[server][reactor-loop][readiness]")
{
    // The guard inside the helper, shown refusing. This is the one branch of the
    // readiness path the end-to-end cases above cannot reach: they bind real
    // listeners, so every accept loop arms and the refusal never fires.
    //
    // A closed `core::net::testing::InMemoryListener` answers `Accept()` with a ready error instead of
    // parking, so `Server::Run()` returns before suspending and `IsAccepting()` is
    // false by the time the call returns. That is exactly "started but not armed",
    // and counting it would put an acceptor that does not exist into the readiness
    // line -- #646 one level deeper.
    //
    // The POSITIVE direction is asserted by the two end-to-end cases above, which
    // count one arm per bind against real listeners and pin the ordering. It is
    // deliberately not asserted here as well: a `Server` over a LIVE
    // `core::net::testing::InMemoryListener` ends parked on `Accept()`, and unparking it walks into a
    // dangling-handle defect in that fake which is unrelated to this change and
    // lives outside this lane -- see the note on the pull request. Saying which
    // direction is covered where is the point; a case that quietly covered one and
    // read as covering both would be the worse outcome.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };

    core::net::testing::InMemoryListener first;
    core::net::testing::InMemoryListener second;
    first.close();
    second.close();

    std::vector<std::unique_ptr<FastCache::Server>> servers;
    core::net::AcceptLoopHealth acceptLoops;
    servers.push_back(std::make_unique<FastCache::Server>(first, engine, logger, acceptLoops, "cache first"));
    servers.push_back(std::make_unique<FastCache::Server>(second, engine, logger, acceptLoops, "cache second"));

    FastCache::ReadinessAnnouncer announcer { logger, "2 bind(s)" };
    announcer.ExpectAcceptor();
    announcer.ExpectAcceptor();
    announcer.AcceptorsAllSpawned();
    FastCache::Detail::ArmAcceptLoops(servers, announcer, logger);

    CHECK_FALSE(servers[0]->IsAccepting());
    CHECK_FALSE(servers[1]->IsAccepting());

    // Nothing armed, so nothing is counted and -- the assertion that matters -- the
    // daemon does NOT claim to be ready.
    CHECK(announcer.ArmedCount() == 0);
    CHECK_FALSE(announcer.Announced());

    auto const records = logger.Snapshot();
    CHECK(IndicesContaining(records, ReadyMarker).empty());
    CHECK(IndicesContaining(records, ArmedMarker).empty());

    // And each loop that did not arm is REPORTED, at Error, naming the endpoint
    // that is not being served. Refusing to count without saying so would leave a
    // daemon that never announces readiness and never explains why.
    auto const refused = IndicesContaining(records, "did not arm");
    REQUIRE(refused.size() == 2);
    for (auto const index: refused)
    {
        CHECK(records[index].level == FastCache::LogLevel::Error);
        CHECK(records[index].message.contains("not being served"));
    }
    // Each under its OWN loop's name -- the one its warnings and `/healthz` use -- so the line says
    // which endpoint, rather than a position in a group.
    CHECK(records[refused[0]].message.starts_with("cache first: "));
    CHECK(records[refused[1]].message.starts_with("cache second: "));
}

TEST_CASE("The readiness ordering check holds for both platforms' participant shapes", "[server][reactor-loop][readiness]")
{
    // The two end-to-end cases above can only ever run this machine's shape, and the
    // shapes genuinely differ: POSIX arms one accept loop per (reactor, bind) pair,
    // Windows arms one per acceptor THREAD plus one per IOCP reactor. A count written
    // into the test therefore passes on the platform it was written on and fails on
    // the other -- which is not hypothetical, it is what happened: `armed.size() == 2`
    // was green on Linux and failed all three Windows legs with `3 == 2`.
    //
    // So the DECISION is exercised against a synthesised record set for each shape,
    // the way `node-scratch-isolation-e2e`'s verdict was split out from its
    // acquisition. Nothing here starts a server; what is under test is that the
    // ordering check is blind to how many participants a platform has.
    auto record = [](FastCache::LogLevel level, std::string message) {
        return FastCache::CapturingLogger::Record { .level = level, .message = std::move(message) };
    };

    SECTION("POSIX: one accept loop per (reactor, bind) pair")
    {
        // 2 reactors x 1 bind.
        CheckReadinessComplete({
            record(FastCache::LogLevel::Debug, "acceptor armed: cache 127.0.0.1:11211 on reactor 0 (1/2)"),
            record(FastCache::LogLevel::Debug, "acceptor armed: cache 127.0.0.1:11211 on reactor 1 (2/2)"),
            record(FastCache::LogLevel::Info, "ready, accepting connections (1 bind(s) x 2 reactors)"),
        });
    }

    SECTION("Windows: one per acceptor thread plus one per IOCP reactor")
    {
        // 1 acceptor thread + 2 reactors = 3, which is the count that broke the
        // literal. The reactors arm last here because their `Run()` is entered after
        // the acceptor threads are spawned.
        CheckReadinessComplete({
            record(FastCache::LogLevel::Debug, "acceptor armed: cache 127.0.0.1:11211 (1/3)"),
            record(FastCache::LogLevel::Debug, "acceptor armed: reactor 1 (2/3)"),
            record(FastCache::LogLevel::Debug, "acceptor armed: reactor 0 (3/3)"),
            record(FastCache::LogLevel::Info, "ready, accepting connections (1 bind(s) x 2 reactors)"),
        });
    }
}

TEST_CASE("ArmProgressOf reads the announcer's own figures and refuses what it cannot", "[server][reactor-loop][readiness]")
{
    // The helper the check above leans on. It must not answer a number for a line it
    // did not understand: a silent `0/0` would make the completeness assertion pass
    // vacuously, which is the failure mode of concluding from a parse nobody checked.
    auto const good = ArmProgressOf("acceptor armed: reactor 0 bind 1 (2/8)");
    REQUIRE(good.has_value());
    CHECK(FastCache::Testing::Unwrap(good).armed == 2);
    CHECK(FastCache::Testing::Unwrap(good).expected == 8);

    // A participant name containing parentheses must not shift the reading: the
    // progress is the LAST parenthesised group, and that is what `rfind` gives. A TLS
    // bind's name is one (`Detail::BindSurface`).
    auto const awkward = ArmProgressOf("acceptor armed: cache 0.0.0.0:6380 (TLS) (1/1)");
    REQUIRE(awkward.has_value());
    CHECK(FastCache::Testing::Unwrap(awkward).armed == 1);
    CHECK(FastCache::Testing::Unwrap(awkward).expected == 1);

    CHECK_FALSE(ArmProgressOf("acceptor armed: reactor 0 bind 0").has_value());
    CHECK_FALSE(ArmProgressOf("acceptor armed: reactor 0 bind 0 (2)").has_value());
    CHECK_FALSE(ArmProgressOf("acceptor armed: reactor 0 bind 0 (x/y)").has_value());
    CHECK_FALSE(ArmProgressOf("ready, accepting connections (2 bind(s))").has_value());
}

namespace
{

/// The name the acceptor thread under test reports under, spelled the way `BindSurface` names a bind.
constexpr std::string_view AcceptorSurface = "cache 127.0.0.1:11211";

/// One accept's answer, as a scripted acceptor gives it.
using RawAcceptAnswer = std::expected<FastCache::AcceptedSocket, core::net::NetError>;

/// A failed accept answering @p code, the way `AcceptRaw` spells one.
RawAcceptAnswer FailedAccept(core::net::NetErrorCode code)
{
    return std::unexpected(core::net::NetError { .code = code, .systemCode = 0, .context = "accept" });
}

/// What an acceptor thread's accepts answer, and how many it made.
///
/// Past the script the acceptor sets the thread's stop flag and answers `Cancelled` -- which is what
/// the teardown's close does -- so a loop that wrongly outlives its script ends rather than spinning,
/// and the count says it did.
struct RawAcceptScript
{
    std::vector<RawAcceptAnswer> answers;
    std::atomic<bool>& stopping;
    std::size_t calls { 0 }; ///< Written on the acceptor thread, read after it is joined.
};

/// `Detail::IRawAcceptor` answering from a `RawAcceptScript` the case owns.
class ScriptedRawAcceptor final: public FastCache::Detail::IRawAcceptor
{
  public:
    explicit ScriptedRawAcceptor(RawAcceptScript& script) noexcept:
        _script { script }
    {
    }

    [[nodiscard]] RawAcceptAnswer Accept() override
    {
        auto const index = _script.calls++;
        if (index < _script.answers.size())
            return _script.answers[index];
        _script.stopping.store(true, std::memory_order_release);
        return FailedAccept(core::net::NetErrorCode::Cancelled);
    }

  private:
    RawAcceptScript& _script;
};

/// A drain wait that records every backoff it is asked for and blocks for none of them.
class RecordingDrainWait final: public FastCache::IDrainWait
{
  public:
    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
    {
        return FastCache::DefaultDrainWait().Now();
    }

    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        _sleeps.push_back(requested);
    }

    /// @return Every backoff asked for, in order. Read after the thread that asked is joined.
    [[nodiscard]] std::vector<std::chrono::milliseconds> const& Sleeps() const noexcept
    {
        return _sleeps;
    }

  private:
    std::vector<std::chrono::milliseconds> _sleeps;
};

/// Everything one acceptor thread under test is handed, and what it did.
struct AcceptorThreadFixture
{
    std::atomic<bool> stopping { false };
    RawAcceptScript script { .answers = {}, .stopping = stopping };
    FastCache::CapturingLogger logger;
    core::net::AcceptLoopHealth acceptLoops;
    RecordingDrainWait wait;
    FastCache::ReadinessAnnouncer announcer { logger, "1 bind(s) x 1 reactors" };
    std::vector<std::string> handedOff; ///< Peers the thread handed on, in order; read after the join.

    /// Run the thread production runs over the script, to its end.
    void RunToEnd()
    {
        announcer.ExpectAcceptor();
        announcer.AcceptorsAllSpawned();
        auto thread = FastCache::Detail::StartAcceptorThread(
            FastCache::Detail::AcceptorThreadOptions { .surface = std::string { AcceptorSurface },
                                                       .threadName = "fc-acceptor-0",
                                                       .acceptor = std::make_unique<ScriptedRawAcceptor>(script),
                                                       .logger = logger,
                                                       .acceptLoops = acceptLoops,
                                                       .wait = wait,
                                                       .stopping = stopping,
                                                       .announcer = announcer },
            [this](FastCache::AcceptedSocket raw) { handedOff.push_back(std::move(raw.peer)); });
        // Every script ends in a `Cancelled` the thread cannot outlive, so the join is bounded by the
        // script's length rather than by a clock.
        thread.join();
    }

    /// @return How many records were logged at @p level.
    [[nodiscard]] std::size_t CountAt(FastCache::LogLevel level) const
    {
        return static_cast<std::size_t>(std::ranges::count(
            logger.Snapshot(), level, [](FastCache::CapturingLogger::Record const& r) { return r.level; }));
    }
};

} // namespace

TEST_CASE("The Windows acceptor thread accepts past a reset and exhaustion and ends only on a closed listener",
          "[server][reactor-loop][accept-loop]")
{
    // The thread `RunMultiReactorWindows` spawns for every bind, driven through its accept seam. It
    // used to end on ANY failed accept, so one client resetting its queued connection -- a blocking
    // `accept()` answering WSAECONNRESET, which `SocketErrorCode` reads as `ConnReset` -- stopped
    // the bind while the port still listened, and every later connect was refused. Here the reset and the exhaustion must
    // each be accepted past, the connection after them served, and only the closed listener end the thread.
    AcceptorThreadFixture fixture;
    fixture.script.answers = {
        FailedAccept(core::net::NetErrorCode::ConnReset),
        FailedAccept(core::net::NetErrorCode::ResourceExhausted),
        RawAcceptAnswer {
            FastCache::AcceptedSocket { .handle = core::platform::InvalidHandle, .peer = "198.51.100.7:40000" } },
        FailedAccept(core::net::NetErrorCode::Cancelled),
    };
    fixture.RunToEnd();

    // Four accepts and no fifth: it kept accepting after the reset and the exhaustion, and the
    // Cancelled -- not the script running out -- ended it.
    CHECK(fixture.script.calls == 4);
    CHECK(fixture.handedOff == std::vector<std::string> { "198.51.100.7:40000" });
    // Only the exhaustion backed off: one failed connection is not a run worth yielding to.
    CHECK(fixture.wait.Sleeps() == std::vector { core::net::AcceptErrorPolicy::FirstBackoff });
    CHECK(fixture.announcer.ArmedCount() == 1);
    CHECK(fixture.CountAt(FastCache::LogLevel::Warn) >= 1);

    // Nobody was stopping it, so the end is news: one Error line, and the registry `/healthz`
    // answers from names the surface.
    auto const cancelled =
        core::net::NetError { .code = core::net::NetErrorCode::Cancelled, .systemCode = 0, .context = "accept" };
    auto const endedLine = core::net::describeAcceptLoopEnded(AcceptorSurface, cancelled);
    auto const records = fixture.logger.Snapshot();
    CHECK(std::ranges::count_if(records,
                                [&](FastCache::CapturingLogger::Record const& r) {
                                    return r.level == FastCache::LogLevel::Error && r.message == endedLine;
                                })
          == 1);
    auto const stopped = fixture.acceptLoops.snapshot();
    REQUIRE(stopped.size() == 1);
    // Under the thread's OWN name, which is its bind's: it used to be `cache` for every bind.
    CHECK(stopped.front().surface == AcceptorSurface);
    CHECK(stopped.front().reason == endedLine);
    CHECK(stopped.front().kind == core::net::AcceptLoopEventKind::GaveUp);
}

TEST_CASE("The Windows acceptor thread ended by the teardown's close reports nothing", "[server][reactor-loop][accept-loop]")
{
    // The sibling: the same Cancelled, arriving after the teardown set `stopping`, is the ordinary
    // end of a stopping daemon. It must neither log at Error nor mark the surface dead, or every
    // clean shutdown would turn `/healthz` red on its way out.
    AcceptorThreadFixture fixture;
    fixture.script.answers = { FailedAccept(core::net::NetErrorCode::ConnReset) };
    fixture.RunToEnd();

    CHECK(fixture.script.calls == 2);
    CHECK(fixture.handedOff.empty());
    CHECK(fixture.CountAt(FastCache::LogLevel::Error) == 0);
    CHECK(fixture.acceptLoops.snapshot().empty());
}

TEST_CASE("Each bind's accept loop reports under a name that says which bind it is", "[server][reactor-loop][accept-loop]")
{
    // Every loop reported as `cache`, so `/healthz` on a daemon with a plaintext and a TLS bind could
    // say only that SOMETHING stopped. The name is the bind, so two binds never share one.
    CHECK(FastCache::Detail::BindSurface(FastCache::BindConfig { .address = "0.0.0.0", .port = 11211, .tls = false })
          == "cache 0.0.0.0:11211");
    CHECK(FastCache::Detail::BindSurface(FastCache::BindConfig { .address = "0.0.0.0", .port = 6380, .tls = true })
          == "cache 0.0.0.0:6380 (TLS)");
    CHECK(FastCache::Detail::BindSurface(FastCache::BindConfig { .address = "::", .port = 11211, .tls = false })
          == "cache [::]:11211");
}
