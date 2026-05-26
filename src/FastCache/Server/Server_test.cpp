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
#include <span>
#include <string>
#include <vector>

namespace
{

FastCache::Task<std::string> ReadResponse(FastCache::ISocket& socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(256);
        auto const result =
            co_await socket.Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value() || *result == 0)
            break;
        for (std::size_t i = 0; i < *result; ++i)
            out.push_back(static_cast<char>(chunk[i]));
        if (*result < chunk.size())
            break;
    }
    co_return out;
}

FastCache::Task<bool> Send(FastCache::ISocket& socket, std::string_view payload)
{
    auto const r = co_await socket.Write(FastCache::AsBytes(payload));
    co_return r.has_value();
}

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
    REQUIRE(FastCache::SyncRun(Send(*client, "set foo 0 0 5\r\nhello\r\nget foo\r\n")));
    client->ShutdownWrite();

    // Close the listener — pre-queued connections drain before Accept
    // observes the closed state, so the staged client still gets served.
    listener.Close();
    FastCache::SyncRun(server.Run());

    auto const response = FastCache::SyncRun(ReadResponse(*client));
    REQUIRE(response == "STORED\r\nVALUE foo 0 5\r\nhello\r\nEND\r\n");
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
