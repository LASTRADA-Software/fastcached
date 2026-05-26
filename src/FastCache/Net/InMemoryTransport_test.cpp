// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

FastCache::Task<std::string> ReadAllAvailable(FastCache::ISocket* socket, std::size_t expected)
{
    std::string out;
    while (out.size() < expected)
    {
        std::vector<std::byte> chunk(expected - out.size());
        auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        REQUIRE(result.has_value());
        if (*result == 0)
            break;
        chunk.resize(*result);
        for (auto const b: chunk)
            out.push_back(static_cast<char>(b));
    }
    co_return out;
}

FastCache::Task<bool> WriteString(FastCache::ISocket* socket, std::string_view payload)
{
    auto const bytes = FastCache::AsBytes(payload);
    auto const result = co_await socket->Write(bytes);
    co_return result.has_value() && *result == payload.size();
}

} // namespace

TEST_CASE("InMemorySocketPair shuttles bytes both directions", "[net][inmemory]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), "hello")));

    auto received = FastCache::SyncRun(ReadAllAvailable(pair.server.get(), 5));
    REQUIRE(received == "hello");

    REQUIRE(FastCache::SyncRun(WriteString(pair.server.get(), "world!")));
    received = FastCache::SyncRun(ReadAllAvailable(pair.client.get(), 6));
    REQUIRE(received == "world!");
}

TEST_CASE("InMemorySocketPair surfaces EOF when one side closes", "[net][inmemory]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), "ab")));
    pair.client->Close();

    // Drain the two bytes, then expect EOF on the next read.
    auto received = FastCache::SyncRun(ReadAllAvailable(pair.server.get(), 4));
    REQUIRE(received == "ab");

    std::byte tail[1] {};
    auto const result =
        FastCache::SyncRun([](FastCache::ISocket* socket, std::span<std::byte> buffer) -> FastCache::Task<bool> {
            auto const r = co_await socket->Read(buffer);
            REQUIRE(r.has_value());
            co_return *r == 0;
        }(pair.server.get(), std::span<std::byte> { tail, 1 }));
    REQUIRE(result);
}

TEST_CASE("InMemoryPipe respects the backpressure cap", "[net][inmemory]")
{
    auto pair = FastCache::InMemorySocketPair::Create(4);
    bool clientWroteOk = FastCache::SyncRun(WriteString(pair.client.get(), "1234"));
    REQUIRE(clientWroteOk);

    // Pipe is now full (4 buffered, cap=4). Next write should partial-fail.
    auto const second = FastCache::SyncRun([](FastCache::ISocket* socket) -> FastCache::Task<bool> {
        std::string_view const five = "EXTRA";
        auto const result = co_await socket->Write(FastCache::AsBytes(five));
        co_return !result.has_value();
    }(pair.client.get()));
    REQUIRE(second);
}
