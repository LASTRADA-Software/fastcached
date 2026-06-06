// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
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

/// IStorage stub whose Get throws. Used to prove the connection driver's
/// exception firewall drops a single connection rather than letting the throw
/// reach DetachedTask::unhandled_exception (std::terminate) and abort the whole
/// server. Only Get is exercised by the test; the rest return a benign miss.
class ThrowOnGetStorage final: public FastCache::IStorage
{
    static auto Miss()
    {
        return std::unexpected(FastCache::MakeStorageError(FastCache::StorageErrorCode::KeyNotFound));
    }

  public:
    std::expected<FastCache::GetResult, FastCache::StorageError> Get(std::string_view, FastCache::TimePoint) override
    {
        throw std::bad_alloc {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Set(std::string_view,
                                                                    std::vector<std::byte>,
                                                                    std::uint32_t,
                                                                    FastCache::TimePoint) override
    {
        return FastCache::CasToken { 1 };
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Add(
        std::string_view, std::vector<std::byte>, std::uint32_t, FastCache::TimePoint, FastCache::TimePoint) override
    {
        return Miss();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Replace(
        std::string_view, std::vector<std::byte>, std::uint32_t, FastCache::TimePoint, FastCache::TimePoint) override
    {
        return Miss();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Append(std::string_view,
                                                                       std::span<std::byte const>,
                                                                       FastCache::CasToken,
                                                                       FastCache::TimePoint) override
    {
        return Miss();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Prepend(std::string_view,
                                                                        std::span<std::byte const>,
                                                                        FastCache::CasToken,
                                                                        FastCache::TimePoint) override
    {
        return Miss();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> CompareAndSwap(std::string_view,
                                                                               FastCache::CasToken,
                                                                               std::vector<std::byte>,
                                                                               std::uint32_t,
                                                                               FastCache::TimePoint,
                                                                               FastCache::TimePoint) override
    {
        return Miss();
    }
    std::expected<IncrResult, FastCache::StorageError> IncrementOrInitialize(std::string_view,
                                                                             std::uint64_t,
                                                                             bool,
                                                                             FastCache::TimePoint) override
    {
        return std::unexpected(FastCache::MakeStorageError(FastCache::StorageErrorCode::KeyNotFound));
    }
    std::expected<void, FastCache::StorageError> Delete(std::string_view, FastCache::TimePoint) override
    {
        return std::unexpected(FastCache::MakeStorageError(FastCache::StorageErrorCode::KeyNotFound));
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Touch(std::string_view,
                                                                      FastCache::TimePoint,
                                                                      FastCache::TimePoint) override
    {
        return Miss();
    }
    std::expected<FastCache::GetResult, FastCache::StorageError> Peek(std::string_view, FastCache::TimePoint) override
    {
        return FastCache::GetResult {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> MarkStale(std::string_view,
                                                                          std::optional<FastCache::TimePoint>,
                                                                          FastCache::TimePoint) override
    {
        return Miss();
    }
    void FlushWithGeneration(FastCache::TimePoint) override {}
    std::size_t PurgeExpired(FastCache::TimePoint) override
    {
        return 0;
    }
    void Resize(std::size_t) override {}
    [[nodiscard]] FastCache::StorageStats Snapshot() const noexcept override
    {
        return {};
    }
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
}
