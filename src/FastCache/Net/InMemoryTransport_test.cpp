// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

/// Gather-write three text segments and report whether all bytes were sent.
FastCache::Task<bool> WriteThreeSegments(FastCache::ISocket* socket,
                                         std::string_view a,
                                         std::string_view b,
                                         std::string_view c)
{
    std::array<std::span<std::byte const>, 3> const segments {
        FastCache::AsBytes(a),
        FastCache::AsBytes(b),
        FastCache::AsBytes(c),
    };
    auto const result = co_await socket->WriteVectored(segments);
    co_return result.has_value() && *result == a.size() + b.size() + c.size();
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

TEST_CASE("WriteVectored gathers segments into the same byte stream", "[net][inmemory]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    // [header][value][trailer] mirrors a GET reply: the peer must see the
    // exact concatenation a single contiguous write would have produced.
    REQUIRE(FastCache::SyncRun(WriteThreeSegments(pair.client.get(), "VALUE k 0 5\r\n", "hello", "\r\n")));

    auto const received = FastCache::SyncRun(ReadAllAvailable(pair.server.get(), 20));
    REQUIRE(received == "VALUE k 0 5\r\nhello\r\n");
}

TEST_CASE("WriteVectored skips empty segments and still reports the full count", "[net][inmemory]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    // An absent key segment (empty) must contribute nothing yet not corrupt
    // the stream — the binary GET path emits an empty key span on opaque gets.
    REQUIRE(FastCache::SyncRun(WriteThreeSegments(pair.client.get(), "head", "", "tail")));

    auto const received = FastCache::SyncRun(ReadAllAvailable(pair.server.get(), 8));
    REQUIRE(received == "headtail");
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

TEST_CASE("A Read with no buffered bytes parks and completes on a later peer Write", "[net][inmemory]")
{
    // Exercises the asynchronous park path: Read() finds the pipe empty,
    // suspends, and is later woken by the peer's Write via OnInboundProgress.
    // The awaitable that gets parked must be the one living in the awaiting
    // coroutine's frame (registered from await_suspend), not the local inside
    // Read() — otherwise _pendingRead dangles and the read never completes.
    auto pair = FastCache::InMemorySocketPair::Create();

    std::vector<std::byte> chunk(5);
    auto reader = [](FastCache::ISocket* socket, std::span<std::byte> buffer) -> FastCache::Task<std::size_t> {
        auto const r = co_await socket->Read(buffer);
        REQUIRE(r.has_value());
        co_return *r;
    }(pair.server.get(), std::span<std::byte> { chunk.data(), chunk.size() });

    // Drive the reader until it parks inside Read(). Nothing is buffered yet,
    // so it must suspend rather than complete.
    auto handle = reader.Native();
    handle.resume();
    REQUIRE_FALSE(reader.IsReady());

    // The peer writes: this pushes into the pipe and fires the inbound
    // progress callback, which must resume the parked reader to completion.
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), "hello")));

    REQUIRE(reader.IsReady());
    auto const got = std::get<1>(handle.promise().result);
    REQUIRE(got == 5);

    std::string out;
    for (auto const b: chunk)
        out.push_back(static_cast<char>(b));
    REQUIRE(out == "hello");
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
