// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Logger.hpp>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/IAdmissionControl.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/HalfClose.hpp>
#if defined(FC_TLS_ENABLED)
    #include <core/net/Tls.hpp>
#endif
#include <FastCache/Server/Connection.hpp>
#include <FastCache/Server/Server.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace
{

core::async::Task<std::string> ReadResponse(core::net::ISocket* socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(256);
        auto const result = co_await socket->read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value() || *result == 0)
            break;
        for (auto const i: std::views::iota(std::size_t { 0 }, *result))
            out.push_back(static_cast<char>(chunk[i]));
        if (*result < chunk.size())
            break;
    }
    co_return out;
}

core::async::Task<bool> Send(core::net::ISocket* socket, std::string_view payload)
{
    auto const r = co_await socket->write(FastCache::AsBytes(payload));
    co_return r.has_value();
}

/// IStorage decorator whose Get throws. Used to prove the connection driver's
/// exception firewall drops a single connection rather than letting the throw
/// reach DetachedTask::unhandled_exception (std::terminate) and abort the whole
/// server. Every other method forwards to a real in-memory storage.
class ThrowOnGetStorage final: public FastCache::IStorage
{
  public:
    std::expected<FastCache::GetResult, FastCache::StorageError> Get(std::string_view /*key*/,
                                                                     core::platform::SteadyTimePoint /*now*/) override
    {
        throw std::bad_alloc {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Set(std::string_view key,
                                                                    std::vector<std::byte> value,
                                                                    std::uint32_t flags,
                                                                    core::platform::SteadyTimePoint expiry) override
    {
        return _inner.Set(key, std::move(value), flags, expiry);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Add(std::string_view key,
                                                                    std::vector<std::byte> value,
                                                                    std::uint32_t flags,
                                                                    core::platform::SteadyTimePoint expiry,
                                                                    core::platform::SteadyTimePoint now) override
    {
        return _inner.Add(key, std::move(value), flags, expiry, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Replace(std::string_view key,
                                                                        std::vector<std::byte> value,
                                                                        std::uint32_t flags,
                                                                        core::platform::SteadyTimePoint expiry,
                                                                        core::platform::SteadyTimePoint now) override
    {
        return _inner.Replace(key, std::move(value), flags, expiry, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Append(std::string_view key,
                                                                       std::span<std::byte const> suffix,
                                                                       FastCache::CasToken expected,
                                                                       core::platform::SteadyTimePoint now) override
    {
        return _inner.Append(key, suffix, expected, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Prepend(std::string_view key,
                                                                        std::span<std::byte const> prefix,
                                                                        FastCache::CasToken expected,
                                                                        core::platform::SteadyTimePoint now) override
    {
        return _inner.Prepend(key, prefix, expected, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> CompareAndSwap(std::string_view key,
                                                                               FastCache::CasToken expected,
                                                                               std::vector<std::byte> value,
                                                                               std::uint32_t flags,
                                                                               core::platform::SteadyTimePoint expiry,
                                                                               core::platform::SteadyTimePoint now) override
    {
        return _inner.CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
    }
    std::expected<IncrResult, FastCache::StorageError> IncrementOrInitialize(std::string_view key,
                                                                             std::uint64_t magnitude,
                                                                             bool decrement,
                                                                             core::platform::SteadyTimePoint now) override
    {
        return _inner.IncrementOrInitialize(key, magnitude, decrement, now);
    }
    std::expected<void, FastCache::StorageError> Delete(std::string_view key, core::platform::SteadyTimePoint now) override
    {
        return _inner.Delete(key, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Touch(std::string_view key,
                                                                      core::platform::SteadyTimePoint newExpiry,
                                                                      core::platform::SteadyTimePoint now) override
    {
        return _inner.Touch(key, newExpiry, now);
    }
    std::expected<FastCache::GetResult, FastCache::StorageError> Peek(std::string_view key,
                                                                      core::platform::SteadyTimePoint now) override
    {
        return _inner.Peek(key, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> MarkStale(
        std::string_view key,
        std::optional<core::platform::SteadyTimePoint> newExpiry,
        core::platform::SteadyTimePoint now) override
    {
        return _inner.MarkStale(key, newExpiry, now);
    }
    void FlushWithGeneration(core::platform::SteadyTimePoint effectiveAt) override
    {
        _inner.FlushWithGeneration(effectiveAt);
    }
    FastCache::PurgeOutcome PurgeExpired(core::platform::SteadyTimePoint now, FastCache::PurgeBudget budget) override
    {
        return _inner.PurgeExpired(now, budget);
    }
    void Resize(std::size_t newMaxBytes) override
    {
        _inner.Resize(newMaxBytes);
    }
    [[nodiscard]] FastCache::StorageStats Snapshot() const noexcept override
    {
        return _inner.Snapshot();
    }

  private:
    FastCache::InMemoryLruStorage _inner;
};

} // namespace

TEST_CASE("Server accepts and serves a memcached-text client end-to-end", "[server]")
{
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger };

    // Stage a client BEFORE running the server so Accept resolves
    // synchronously on the first iteration.
    auto client = listener.connectClient();
    REQUIRE(core::async::syncRun(Send(client.get(), "set foo 0 0 5\r\nhello\r\nget foo\r\n")));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());

    // Close the listener — pre-queued connections drain before Accept
    // observes the closed state, so the staged client still gets served.
    listener.close();
    core::async::syncRun(server.Run());

    auto const response = core::async::syncRun(ReadResponse(client.get()));
    REQUIRE(response == "STORED\r\nVALUE foo 0 5\r\nhello\r\nEND\r\n");
    REQUIRE(server.AcceptedCount() == 1);
}

TEST_CASE("Server with LogSource::Yes prefixes connection logs with the client IP", "[server][logsource]")
{
    // A connection that EOFs before any byte makes autodetect fail, which the
    // Connection logs at Debug. With --log-source on, that line carries the
    // accepted socket's peer address as a bracketed prefix.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger; // captures from Trace up
    core::net::testing::InMemoryListener listener;
    FastCache::Server server {
        listener, engine, logger, nullptr, nullptr, FastCache::SessionContext {}, nullptr, FastCache::LogSource::Yes
    };

    auto client = listener.connectClient(/*maxBytesInFlight*/ 0, /*peerAddress*/ "203.0.113.7");
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value()); // EOF with no bytes -> autodetect fails -> Debug log
    listener.close();
    core::async::syncRun(server.Run());

    auto const records = logger.Snapshot();
    REQUIRE_FALSE(records.empty());
    REQUIRE(std::ranges::any_of(
        records, [](auto const& r) { return r.message.starts_with("[203.0.113.7] Connection: autodetect failed"); }));
}

TEST_CASE("Server with LogSource::Yes prefixes the storage trace line with the client IP", "[server][logsource]")
{
    // The real use case: a well-behaved client. Each data operation produces
    // exactly ONE line — the TracingStorage `storage:` line — now carrying the
    // client IP. There is no separate connection-level command line to duplicate it.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage lru;
    FastCache::CapturingLogger logger; // captures from Trace up
    FastCache::TracingStorage storage { lru, logger, clock };
    FastCache::CacheEngine engine { storage, clock };
    core::net::testing::InMemoryListener listener;
    FastCache::Server server {
        listener, engine, logger, nullptr, nullptr, FastCache::SessionContext {}, nullptr, FastCache::LogSource::Yes
    };

    auto client = listener.connectClient(/*maxBytesInFlight*/ 0, /*peerAddress*/ "203.0.113.7");
    REQUIRE(core::async::syncRun(Send(client.get(), "set foo 0 0 5\r\nhello\r\nget foo\r\n")));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
    listener.close();
    core::async::syncRun(server.Run());

    auto const messages = logger.Snapshot();
    REQUIRE(std::ranges::any_of(
        messages, [](auto const& r) { return r.message.starts_with("[203.0.113.7] storage: SET key=foo result=STORED"); }));
    REQUIRE(std::ranges::any_of(
        messages, [](auto const& r) { return r.message.starts_with("[203.0.113.7] storage: GET key=foo result=HIT"); }));
    // No duplicate connection-level data-command line.
    REQUIRE(std::ranges::none_of(messages, [](auto const& r) { return r.message == "[203.0.113.7] set foo"; }));
    REQUIRE(std::ranges::none_of(messages, [](auto const& r) { return r.message == "[203.0.113.7] get foo"; }));
}

TEST_CASE("Server without --log-source leaves the storage trace line unprefixed", "[server][logsource]")
{
    // Storage trace logging is independent of --log-source; the flag only adds
    // the IP prefix. At Trace without the flag the line is the original format.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage lru;
    FastCache::CapturingLogger logger;
    FastCache::TracingStorage storage { lru, logger, clock };
    FastCache::CacheEngine engine { storage, clock };
    core::net::testing::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger }; // LogSource defaults to No

    auto client = listener.connectClient(/*maxBytesInFlight*/ 0, /*peerAddress*/ "203.0.113.7");
    REQUIRE(core::async::syncRun(Send(client.get(), "set foo 0 0 5\r\nhello\r\nget foo\r\n")));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
    listener.close();
    core::async::syncRun(server.Run());

    auto const messages = logger.Snapshot();
    REQUIRE(std::ranges::any_of(messages, [](auto const& r) { return r.message.starts_with("storage: SET key=foo"); }));
    REQUIRE(std::ranges::none_of(messages, [](auto const& r) { return r.message.contains("203.0.113.7"); }));
}

TEST_CASE("Server: non-data commands are logged only under --log-everything", "[server][logsource]")
{
    // "version" is a non-data command — it never reaches storage, so it has no
    // `storage:` line. By default it is invisible; under --log-everything it is
    // surfaced at the connection level (with the IP under --log-source). The
    // data "set" always appears on its storage line either way.
    auto run = [](bool logEverything) {
        core::platform::ManualClock clock;
        FastCache::InMemoryLruStorage lru;
        FastCache::CapturingLogger logger;
        FastCache::TracingStorage storage { lru, logger, clock };
        FastCache::CacheEngine engine { storage, clock };
        core::net::testing::InMemoryListener listener;
        FastCache::SessionContext session {};
        session.logEverything = logEverything;
        FastCache::Server server { listener, engine, logger, nullptr, nullptr, session, nullptr, FastCache::LogSource::Yes };

        auto client = listener.connectClient(/*maxBytesInFlight*/ 0, /*peerAddress*/ "203.0.113.7");
        REQUIRE(core::async::syncRun(Send(client.get(), "version\r\nset foo 0 0 5\r\nhello\r\n")));
        REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
        listener.close();
        core::async::syncRun(server.Run());
        return logger.Snapshot();
    };

    SECTION("default: storage line present, admin command suppressed")
    {
        auto const messages = run(/*logEverything*/ false);
        REQUIRE(std::ranges::any_of(
            messages, [](auto const& r) { return r.message.starts_with("[203.0.113.7] storage: SET key=foo"); }));
        REQUIRE(std::ranges::none_of(messages, [](auto const& r) { return r.message.contains("version"); }));
    }
    SECTION("--log-everything: admin command logged at the connection level")
    {
        auto const messages = run(/*logEverything*/ true);
        REQUIRE(std::ranges::any_of(
            messages, [](auto const& r) { return r.message.starts_with("[203.0.113.7] storage: SET key=foo"); }));
        REQUIRE(std::ranges::any_of(messages, [](auto const& r) { return r.message == "[203.0.113.7] version"; }));
    }
}

TEST_CASE("Server: command logging is silent at the default Info level", "[server][logsource]")
{
    // Trace-level access logging (including the storage line) must not appear
    // at the default level, so normal production output is unchanged whether or
    // not --log-source is set.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage lru;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Info }; // default threshold
    FastCache::TracingStorage storage { lru, logger, clock };
    FastCache::CacheEngine engine { storage, clock };
    core::net::testing::InMemoryListener listener;
    FastCache::Server server {
        listener, engine, logger, nullptr, nullptr, FastCache::SessionContext {}, nullptr, FastCache::LogSource::Yes
    };

    auto client = listener.connectClient(/*maxBytesInFlight*/ 0, /*peerAddress*/ "203.0.113.7");
    REQUIRE(core::async::syncRun(Send(client.get(), "set foo 0 0 5\r\nhello\r\nget foo\r\n")));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
    listener.close();
    core::async::syncRun(server.Run());

    auto const messages = logger.Snapshot();
    REQUIRE(std::ranges::none_of(messages, [](auto const& r) {
        return r.message.contains("connection accepted") || r.message.contains("storage:") || r.message.contains("foo");
    }));
}

TEST_CASE("Server with LogSource::No leaves connection logs unprefixed", "[server][logsource]")
{
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::CapturingLogger logger;
    core::net::testing::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger }; // LogSource defaults to No

    auto client = listener.connectClient(/*maxBytesInFlight*/ 0, /*peerAddress*/ "203.0.113.7");
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
    listener.close();
    core::async::syncRun(server.Run());

    auto const records = logger.Snapshot();
    REQUIRE_FALSE(records.empty());
    // The diagnostic is present, but with no source prefix.
    REQUIRE(
        std::ranges::any_of(records, [](auto const& r) { return r.message.starts_with("Connection: autodetect failed"); }));
    REQUIRE(std::ranges::none_of(records, [](auto const& r) { return r.message.contains("203.0.113.7"); }));
}

TEST_CASE("Server drops a connection whose handler throws instead of terminating", "[server][regression]")
{
    core::platform::ManualClock clock;
    ThrowOnGetStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger };

    auto client = listener.connectClient();
    REQUIRE(core::async::syncRun(Send(client.get(), "get foo\r\n")));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
    listener.close();

    // The Get throws std::bad_alloc. Without the firewall in RunConnectionDetached
    // the throw would reach DetachedTask::unhandled_exception -> std::terminate and
    // abort this test process. With it, the one connection is dropped and Run()
    // returns normally — the server survives a handler exception.
    core::async::syncRun(server.Run());
    REQUIRE(server.AcceptedCount() == 1);
}

TEST_CASE("Server: a refusal over a request the connection did not finish reading still reaches the client",
          "[server][linger]")
{
    // #1554. The RESP handler refuses a value past `maxPayloadBytes` and returns with the
    // rest of that value unread. A close then is a reset on the wire, and a reset destroys
    // the refusal before a Windows client reads it. The connection lingers instead: it
    // half-closes, listens until this client closes, then closes. Driven through
    // `Server::Run`, so what is asserted is production's close rather than a fixture's copy
    // of it.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    FastCache::SessionContext session {};
    session.maxPayloadBytes = 1024;
    FastCache::Server server { listener, engine, logger, nullptr, nullptr, session };

    auto client = listener.connectClient();
    std::string const value(4096, 'x');
    REQUIRE(core::async::syncRun(Send(client.get(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4096\r\n" + value + "\r\n")));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());
    listener.close();
    core::async::syncRun(server.Run());

    auto const response = core::async::syncRun(ReadResponse(client.get()));
    INFO("response was: " << response.substr(0, 64));
    REQUIRE(response.starts_with("-ERR Protocol error:"));
}

TEST_CASE("Server: a lingering connection holds its admission slot until it closes", "[server][linger][admission]")
{
    // #1554. A connection that refused and is listening to its client before closing is
    // still an open socket, so it counts against the admission cap -- or lingering would be a
    // way to hold sockets past it. The client here stays connected and silent after the
    // refusal, so only the linger's deadline ends the connection, and the slot is asserted
    // held on both sides of it.
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::TestLoop reactor { clock };
    core::net::testing::InMemoryListener listener;
    core::net::CountingAdmissionControl admission { /*maxConcurrent*/ 4 };
    FastCache::SessionContext session {};
    session.maxPayloadBytes = 1024;
    session.reactor = &reactor;
    FastCache::Server server { listener, engine, logger, &admission, nullptr, session };

    auto client = listener.connectClient();
    std::string const value(4096, 'x');
    REQUIRE(core::async::syncRun(Send(client.get(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4096\r\n" + value + "\r\n")));
    listener.close();
    core::async::syncRun(server.Run());

    // Refused -- the refusal and the half-close have reached the client -- and still open.
    CHECK(core::async::syncRun(ReadResponse(client.get())).starts_with("-ERR Protocol error:"));
    CHECK(admission.inFlight() == 1);

    clock.advance(FastCache::Connection::Linger.total - std::chrono::milliseconds { 1 });
    std::ignore = reactor.drain();
    CHECK(admission.inFlight() == 1);

    clock.advance(std::chrono::milliseconds { 1 });
    std::ignore = reactor.drain();
    CHECK(admission.inFlight() == 0);
}

TEST_CASE("Server::Shutdown closes the listener", "[server]")
{
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger };

    auto serverTask = server.Run();
    server.Shutdown();
    core::async::syncRun(std::move(serverTask));

    REQUIRE(server.AcceptedCount() == 0);
}

namespace
{

/// Admission policy that always refuses (used to test the rejection path
/// without needing to keep a long-running connection in flight).
class AlwaysDenyAdmission final: public core::net::IAdmissionControl
{
  public:
    [[nodiscard]] std::optional<core::net::AdmissionLease> tryAdmit() noexcept override
    {
        return std::nullopt;
    }

  private:
    void release() noexcept override {}
};

} // namespace

TEST_CASE("Server rejects connections when admission denies", "[server][admission]")
{
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    AlwaysDenyAdmission admission;
    FastCache::AtomicMetricsSink metrics;
    FastCache::Server server { listener, engine, logger, &admission, &metrics };

    auto c1 = listener.connectClient();
    auto c2 = listener.connectClient();
    REQUIRE(FastCache::Testing::ShutdownWrite(*c1).has_value());
    REQUIRE(FastCache::Testing::ShutdownWrite(*c2).has_value());

    listener.close();
    core::async::syncRun(server.Run());

    // Both incoming connections were refused; AcceptedCount only counts
    // admitted connections.
    REQUIRE(server.AcceptedCount() == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotal) == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejected) == 2);
}

TEST_CASE("Server admits + tracks ConnectionsTotal", "[server][admission]")
{
    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    core::net::CountingAdmissionControl admission { /*maxConcurrent*/ 4 };
    FastCache::AtomicMetricsSink metrics;
    FastCache::Server server { listener, engine, logger, &admission, &metrics };

    auto c1 = listener.connectClient();
    auto c2 = listener.connectClient();
    REQUIRE(FastCache::Testing::ShutdownWrite(*c1).has_value());
    REQUIRE(FastCache::Testing::ShutdownWrite(*c2).has_value());

    listener.close();
    core::async::syncRun(server.Run());

    REQUIRE(server.AcceptedCount() == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotal) == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejected) == 0);
    // No TLS context passed -> the TLS-bind counters stay at zero.
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotalTls) == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejectedTls) == 0);
}

#if defined(FC_TLS_ENABLED)
TEST_CASE("Server: ConnectionsTotalTls / ConnectionsAdmissionRejectedTls bumped on a TLS bind", "[server][admission][tls]")
{
    // Finding #14: per-bind metrics labels. The IMetricsSink interface
    // is counter-only (no labels); we instead expose paired counters so
    // operators can attribute traffic to plaintext vs TLS. A Server
    // constructed with a non-null TlsContext bumps both
    // `ConnectionsTotal` AND `ConnectionsTotalTls`, and similarly for
    // the admission-reject pair.
    //
    // Construct a real TlsContext from the testdata cert/key. The
    // accepted InMemorySocket will be wrapped in a TlsSocket which can
    // never complete a real handshake on top of an in-memory pipe, but
    // the metric is bumped BEFORE the wrap; the connection itself just
    // drops a few bytes into the void and exits when the client
    // ShutdownWrite-closes.
    auto const certPath = std::string { FASTCACHED_TESTDATA_DIR } + "/tls/server.crt";
    auto const keyPath = std::string { FASTCACHED_TESTDATA_DIR } + "/tls/server.key";
    auto tlsContextResult = core::net::makeTlsServerContextFromFiles(certPath, keyPath);
    REQUIRE(tlsContextResult.has_value());
    auto tlsContext = std::move(*tlsContextResult);

    core::platform::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    core::net::testing::InMemoryListener listener;
    core::net::CountingAdmissionControl admission { /*maxConcurrent*/ 4 };
    FastCache::AtomicMetricsSink metrics;
    FastCache::Server server { listener, engine, logger, &admission, &metrics, /*session*/ {}, tlsContext.get() };

    auto c1 = listener.connectClient();
    auto c2 = listener.connectClient();
    REQUIRE(FastCache::Testing::ShutdownWrite(*c1).has_value());
    REQUIRE(FastCache::Testing::ShutdownWrite(*c2).has_value());

    listener.close();
    core::async::syncRun(server.Run());

    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotal) == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotalTls) == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejected) == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejectedTls) == 0);
}
#endif
