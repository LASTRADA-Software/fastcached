// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Server/PooledServerLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

FastCache::Task<std::string> ReadAll(FastCache::ISocket* socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> buf(256);
        auto const r = co_await socket->Read({ buf.data(), buf.size() });
        if (!r.has_value() || *r == 0)
            break;
        for (std::size_t i = 0; i < *r; ++i)
            out.push_back(static_cast<char>(buf[i]));
        if (*r < buf.size())
            break;
    }
    co_return out;
}

FastCache::Task<bool> Send(FastCache::ISocket* socket, std::string_view payload)
{
    auto const r = co_await socket->Write(FastCache::AsBytes(payload));
    co_return r.has_value();
}

} // namespace

TEST_CASE("PooledServerLoop serves N pre-staged connections to completion", "[pooled]")
{
    FastCache::ManualClock clock;
    // 4 shards so concurrent worker writes don't race on a single
    // unprotected InMemoryLruStorage instance.
    std::vector<std::unique_ptr<FastCache::IStorage>> shards;
    for (int i = 0; i < 4; ++i)
        shards.emplace_back(std::make_unique<FastCache::InMemoryLruStorage>());
    FastCache::ShardedStorage storage { std::move(shards) };
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;

    // Stage 10 clients before the server runs and pre-close the listener.
    // The accept loop drains every staged client and then exits cleanly.
    // (InMemoryListener has no cross-thread synchronisation, so the
    // server loop must run on the same thread as Close().)
    std::vector<std::unique_ptr<FastCache::ISocket>> clients;
    for (int i = 0; i < 10; ++i)
    {
        auto client = listener.ConnectClient();
        auto const payload = "set k" + std::to_string(i) + " 0 0 1\r\nv\r\nget k" + std::to_string(i) + "\r\n";
        REQUIRE(FastCache::SyncRun(Send(client.get(), payload)));
        client->ShutdownWrite();
        clients.push_back(std::move(client));
    }
    listener.Close();

    std::atomic<bool> stop { false };
    auto const accepted = FastCache::RunPooledServerLoop(listener, engine, logger, stop, /*poolSize=*/4);

    REQUIRE(accepted == 10u);
    for (int i = 0; i < 10; ++i)
    {
        auto const response = FastCache::SyncRun(ReadAll(clients[static_cast<std::size_t>(i)].get()));
        REQUIRE(response.find("STORED") != std::string::npos);
        REQUIRE(response.find("VALUE k" + std::to_string(i)) != std::string::npos);
    }
}

TEST_CASE("PooledServerLoop returns immediately on a pre-closed listener", "[pooled]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;
    listener.Close();

    std::atomic<bool> stop { false };
    auto const accepted = FastCache::RunPooledServerLoop(listener, engine, logger, stop, /*poolSize=*/2);
    REQUIRE(accepted == 0u);
    // Reaching here at all confirms the WorkerPool destructor drained
    // and joined the worker threads without hanging.
}

TEST_CASE("PooledServerLoop with poolSize=0 resolves to hardware_concurrency", "[pooled]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::NullLogger logger;
    FastCache::InMemoryListener listener;

    auto client = listener.ConnectClient();
    REQUIRE(FastCache::SyncRun(Send(client.get(), "set k 0 0 1\r\nv\r\nget k\r\n")));
    client->ShutdownWrite();
    listener.Close();

    std::atomic<bool> stop { false };
    auto const accepted = FastCache::RunPooledServerLoop(listener, engine, logger, stop, /*poolSize=*/0);
    REQUIRE(accepted == 1u);

    auto const response = FastCache::SyncRun(ReadAll(client.get()));
    REQUIRE(response.find("STORED") != std::string::npos);
}
