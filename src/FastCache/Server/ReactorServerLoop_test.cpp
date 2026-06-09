// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <string>

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
