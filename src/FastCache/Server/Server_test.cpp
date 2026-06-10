// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsContext.hpp>
#endif
#include <FastCache/Server/Server.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{

FastCache::Task<std::string> ReadResponse(FastCache::ISocket* socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(256);
        auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value() || *result == 0)
            break;
        for (std::size_t i = 0; i < *result; ++i)
            out.push_back(static_cast<char>(chunk[i]));
        if (*result < chunk.size())
            break;
    }
    co_return out;
}

FastCache::Task<bool> Send(FastCache::ISocket* socket, std::string_view payload)
{
    auto const r = co_await socket->Write(FastCache::AsBytes(payload));
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
                                                                     FastCache::TimePoint /*now*/) override
    {
        throw std::bad_alloc {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Set(std::string_view key,
                                                                    std::vector<std::byte> value,
                                                                    std::uint32_t flags,
                                                                    FastCache::TimePoint expiry) override
    {
        return _inner.Set(key, std::move(value), flags, expiry);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Add(std::string_view key,
                                                                    std::vector<std::byte> value,
                                                                    std::uint32_t flags,
                                                                    FastCache::TimePoint expiry,
                                                                    FastCache::TimePoint now) override
    {
        return _inner.Add(key, std::move(value), flags, expiry, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Replace(std::string_view key,
                                                                        std::vector<std::byte> value,
                                                                        std::uint32_t flags,
                                                                        FastCache::TimePoint expiry,
                                                                        FastCache::TimePoint now) override
    {
        return _inner.Replace(key, std::move(value), flags, expiry, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Append(std::string_view key,
                                                                       std::span<std::byte const> suffix,
                                                                       FastCache::CasToken expected,
                                                                       FastCache::TimePoint now) override
    {
        return _inner.Append(key, suffix, expected, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Prepend(std::string_view key,
                                                                        std::span<std::byte const> prefix,
                                                                        FastCache::CasToken expected,
                                                                        FastCache::TimePoint now) override
    {
        return _inner.Prepend(key, prefix, expected, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> CompareAndSwap(std::string_view key,
                                                                               FastCache::CasToken expected,
                                                                               std::vector<std::byte> value,
                                                                               std::uint32_t flags,
                                                                               FastCache::TimePoint expiry,
                                                                               FastCache::TimePoint now) override
    {
        return _inner.CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
    }
    std::expected<IncrResult, FastCache::StorageError> IncrementOrInitialize(std::string_view key,
                                                                             std::uint64_t magnitude,
                                                                             bool decrement,
                                                                             FastCache::TimePoint now) override
    {
        return _inner.IncrementOrInitialize(key, magnitude, decrement, now);
    }
    std::expected<void, FastCache::StorageError> Delete(std::string_view key, FastCache::TimePoint now) override
    {
        return _inner.Delete(key, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Touch(std::string_view key,
                                                                      FastCache::TimePoint newExpiry,
                                                                      FastCache::TimePoint now) override
    {
        return _inner.Touch(key, newExpiry, now);
    }
    std::expected<FastCache::GetResult, FastCache::StorageError> Peek(std::string_view key,
                                                                      FastCache::TimePoint now) override
    {
        return _inner.Peek(key, now);
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> MarkStale(std::string_view key,
                                                                          std::optional<FastCache::TimePoint> newExpiry,
                                                                          FastCache::TimePoint now) override
    {
        return _inner.MarkStale(key, newExpiry, now);
    }
    void FlushWithGeneration(FastCache::TimePoint effectiveAt) override
    {
        _inner.FlushWithGeneration(effectiveAt);
    }
    std::size_t PurgeExpired(FastCache::TimePoint now) override
    {
        return _inner.PurgeExpired(now);
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
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger };

    // Stage a client BEFORE running the server so Accept resolves
    // synchronously on the first iteration.
    auto client = listener.ConnectClient();
    REQUIRE(FastCache::SyncRun(Send(client.get(), "set foo 0 0 5\r\nhello\r\nget foo\r\n")));
    client->ShutdownWrite();

    // Close the listener — pre-queued connections drain before Accept
    // observes the closed state, so the staged client still gets served.
    listener.Close();
    FastCache::SyncRun(server.Run());

    auto const response = FastCache::SyncRun(ReadResponse(client.get()));
    REQUIRE(response == "STORED\r\nVALUE foo 0 5\r\nhello\r\nEND\r\n");
    REQUIRE(server.AcceptedCount() == 1);
}

TEST_CASE("Server drops a connection whose handler throws instead of terminating", "[server][regression]")
{
    FastCache::ManualClock clock;
    ThrowOnGetStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger };

    auto client = listener.ConnectClient();
    REQUIRE(FastCache::SyncRun(Send(client.get(), "get foo\r\n")));
    client->ShutdownWrite();
    listener.Close();

    // The Get throws std::bad_alloc. Without the firewall in RunConnectionDetached
    // the throw would reach DetachedTask::unhandled_exception -> std::terminate and
    // abort this test process. With it, the one connection is dropped and Run()
    // returns normally — the server survives a handler exception.
    FastCache::SyncRun(server.Run());
    REQUIRE(server.AcceptedCount() == 1);
}

TEST_CASE("Server::Shutdown closes the listener", "[server]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    FastCache::Server server { listener, engine, logger };

    auto serverTask = server.Run();
    server.Shutdown();
    FastCache::SyncRun(std::move(serverTask));

    REQUIRE(server.AcceptedCount() == 0);
}

namespace
{

/// Admission policy that always refuses (used to test the rejection path
/// without needing to keep a long-running connection in flight).
class AlwaysDenyAdmission final: public FastCache::IAdmissionControl
{
  public:
    [[nodiscard]] bool AllowAccept() noexcept override
    {
        return false;
    }
    void OnConnectionStarted() noexcept override {}
    void OnConnectionEnded() noexcept override {}
};

} // namespace

TEST_CASE("Server rejects connections when admission denies", "[server][admission]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    AlwaysDenyAdmission admission;
    FastCache::AtomicMetricsSink metrics;
    FastCache::Server server { listener, engine, logger, &admission, &metrics };

    auto c1 = listener.ConnectClient();
    auto c2 = listener.ConnectClient();
    c1->ShutdownWrite();
    c2->ShutdownWrite();

    listener.Close();
    FastCache::SyncRun(server.Run());

    // Both incoming connections were refused; AcceptedCount only counts
    // admitted connections.
    REQUIRE(server.AcceptedCount() == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotal) == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejected) == 2);
}

TEST_CASE("Server admits + tracks ConnectionsTotal", "[server][admission]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    FastCache::CountingAdmissionControl admission { /*maxConcurrent*/ 4 };
    FastCache::AtomicMetricsSink metrics;
    FastCache::Server server { listener, engine, logger, &admission, &metrics };

    auto c1 = listener.ConnectClient();
    auto c2 = listener.ConnectClient();
    c1->ShutdownWrite();
    c2->ShutdownWrite();

    listener.Close();
    FastCache::SyncRun(server.Run());

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
    auto tlsContextResult = FastCache::TlsContext::Create(certPath, keyPath);
    REQUIRE(tlsContextResult.has_value());
    auto tlsContext = std::move(*tlsContextResult);

    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    FastCache::CountingAdmissionControl admission { /*maxConcurrent*/ 4 };
    FastCache::AtomicMetricsSink metrics;
    FastCache::Server server { listener, engine, logger, &admission, &metrics, /*session*/ {}, tlsContext.get() };

    auto c1 = listener.ConnectClient();
    auto c2 = listener.ConnectClient();
    c1->ShutdownWrite();
    c2->ShutdownWrite();

    listener.Close();
    FastCache::SyncRun(server.Run());

    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotal) == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsTotalTls) == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejected) == 0);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::ConnectionsAdmissionRejectedTls) == 0);
}
#endif
