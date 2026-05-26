// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// Test fixture: an InMemorySocketPair, a CacheEngine over an
/// InMemoryLruStorage, and a single MemcachedTextHandler driving the
/// server-side socket. The client side is driven manually by tests.
struct TextFixture
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::MemcachedTextHandler handler;
};

FastCache::Task<bool> WriteString(FastCache::ISocket& socket, std::string_view payload)
{
    auto const result = co_await socket.Write(FastCache::AsBytes(payload));
    co_return result.has_value();
}

FastCache::Task<std::string> ReadAvailable(FastCache::ISocket& socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(256);
        auto const result =
            co_await socket.Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value())
            break;
        if (*result == 0)
            break;
        for (std::size_t i = 0; i < *result; ++i)
            out.push_back(static_cast<char>(chunk[i]));
        if (*result < chunk.size())
            break;
    }
    co_return out;
}

/// Run a single client-side request → server-side response cycle. The
/// MemcachedTextHandler's Run loop processes one command per iteration,
/// then waits for the next line; we close the client socket after writing
/// our request so the handler observes EOF and the task completes.
std::string Exchange(TextFixture& fix, std::string_view request)
{
    REQUIRE(FastCache::SyncRun(WriteString(*fix.pair.client, request)));
    // Half-close: server will see EOF after consuming `request`, but the
    // client's read side stays open so we can read the response back.
    fix.pair.client->ShutdownWrite();

    FastCache::SyncRun(fix.handler.Run(*fix.pair.server, fix.engine, /*primingBytes*/ {}));
    // The handler closed the server-side socket when its loop ended,
    // which closed the server→client pipe's write side. The client can now
    // read the buffered response and then observe EOF.
    return FastCache::SyncRun(ReadAvailable(*fix.pair.client));
}

} // namespace

TEST_CASE("memcached-text version yields VERSION line", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "version\r\n");
    REQUIRE(response.starts_with("VERSION fastcached-"));
    REQUIRE(response.ends_with("\r\n"));
}

TEST_CASE("memcached-text set then get", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set foo 0 0 5\r\nhello\r\nget foo\r\n");
    REQUIRE(response == "STORED\r\nVALUE foo 0 5\r\nhello\r\nEND\r\n");
}

TEST_CASE("memcached-text get miss yields END only", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "get nothere\r\n");
    REQUIRE(response == "END\r\n");
}

TEST_CASE("memcached-text delete works on present and absent keys", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 0 1\r\nx\r\ndelete k\r\ndelete k\r\n");
    REQUIRE(response == "STORED\r\nDELETED\r\nNOT_FOUND\r\n");
}

TEST_CASE("memcached-text add then add yields NOT_STORED", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "add k 0 0 1\r\nx\r\nadd k 0 0 1\r\ny\r\n");
    REQUIRE(response == "STORED\r\nNOT_STORED\r\n");
}

TEST_CASE("memcached-text incr against a numeric value", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set c 0 0 2\r\n10\r\nincr c 5\r\ndecr c 100\r\n");
    REQUIRE(response == "STORED\r\n15\r\n0\r\n");
}

TEST_CASE("memcached-text cas mismatches and matches", "[protocol][text]")
{
    TextFixture fix;
    // After "set", the entry has CAS=1. We try CAS with 999 (mismatch), then with 1 (match).
    auto const response = Exchange(fix, "set k 0 0 1\r\nA\r\ncas k 0 0 1 999\r\nB\r\ncas k 0 0 1 1\r\nC\r\n");
    REQUIRE(response == "STORED\r\nEXISTS\r\nSTORED\r\n");
}

TEST_CASE("memcached-text quit closes the session", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "quit\r\n");
    REQUIRE(response.empty());
}

TEST_CASE("memcached-text flush_all wipes everything", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 0 1\r\nx\r\nflush_all\r\nget k\r\n");
    REQUIRE(response == "STORED\r\nOK\r\nEND\r\n");
}

TEST_CASE("memcached-text unknown command yields ERROR", "[protocol][text]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "bogus thing\r\n");
    REQUIRE(response == "ERROR\r\n");
}
