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
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// Test fixture: an InMemorySocketPair, a CacheEngine over an
/// InMemoryLruStorage, and a single MemcachedTextHandler driving the
/// server-side socket-> The client side is driven manually by tests.
struct TextFixture
{
    /// @param maxValueBytes Per-value storage cap (0 = unlimited, the default).
    explicit TextFixture(std::size_t maxValueBytes = 0):
        storage { 0, maxValueBytes }
    {
    }

    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::MemcachedTextHandler handler;
};

FastCache::Task<bool> WriteString(FastCache::ISocket* socket, std::string_view payload)
{
    auto const result = co_await socket->Write(FastCache::AsBytes(payload));
    co_return result.has_value();
}

FastCache::Task<std::string> ReadAvailable(FastCache::ISocket* socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(256);
        auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
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
    REQUIRE(FastCache::SyncRun(WriteString(fix.pair.client.get(), request)));
    // Half-close: server will see EOF after consuming `request`, but the
    // client's read side stays open so we can read the response back.
    fix.pair.client->ShutdownWrite();

    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primingBytes*/ {}));
    // The handler closed the server-side socket when its loop ended,
    // which closed the server→client pipe's write side. The client can now
    // read the buffered response and then observe EOF.
    return FastCache::SyncRun(ReadAvailable(fix.pair.client.get()));
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

TEST_CASE("memcached-text get round-trips a large (64 KiB) value via the gather path", "[protocol][text][large]")
{
    // Exercises the zero-copy scatter/gather GET response: the value segment
    // points directly at the cached payload. A 64 KiB value is the size where
    // the previous copy-into-one-buffer path lost to memcached, so this guards
    // both correctness and the no-copy reply assembly.
    TextFixture fix;
    constexpr std::size_t Size = 64U * 1024U;
    std::string value(Size, '\0');
    for (std::size_t i = 0; i < Size; ++i)
        value[i] = static_cast<char>('A' + (i % 26));

    auto const request = std::format("set big 0 0 {}\r\n{}\r\nget big\r\n", Size, value);
    auto const response = Exchange(fix, request);

    auto const expected = std::format("STORED\r\nVALUE big 0 {}\r\n{}\r\nEND\r\n", Size, value);
    REQUIRE(response == expected);
}

TEST_CASE("memcached-text multi-key get gathers every value block in order", "[protocol][text][large]")
{
    // The multi-key path emits per-hit [header][value][CRLF] segments; verify
    // ordering and framing hold when several keys (incl. a large value) are
    // requested in one command.
    TextFixture fix;
    std::string const big(4096, 'Z');
    auto const request =
        std::format("set a 0 0 1\r\nx\r\nset b 0 0 {}\r\n{}\r\nset c 0 0 1\r\ny\r\nget a b c\r\n", big.size(), big);
    auto const response = Exchange(fix, request);
    auto const expected =
        std::format("STORED\r\nSTORED\r\nSTORED\r\nVALUE a 0 1\r\nx\r\nVALUE b 0 {}\r\n{}\r\nVALUE c 0 1\r\ny\r\nEND\r\n",
                    big.size(),
                    big);
    REQUIRE(response == expected);
}

TEST_CASE("memcached-text set over the value cap yields SERVER_ERROR object too large", "[protocol][text][max-value]")
{
    TextFixture fix { /*maxValueBytes=*/8 };
    // One request, three commands: a value within the cap stores; one over it
    // is rejected with the stock memcached wording (framing stays aligned —
    // the oversized payload is still consumed); the prior value is intact.
    auto const response = Exchange(fix, "set k 0 0 8\r\nABCDEFGH\r\nset big 0 0 9\r\nABCDEFGHI\r\nget k\r\n");
    REQUIRE(response == "STORED\r\nSERVER_ERROR object too large for cache\r\nVALUE k 0 8\r\nABCDEFGH\r\nEND\r\n");
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

TEST_CASE("memcached-text incr/decr handle full-uint64 deltas without UB (regression)",
          "[protocol][text][arith][regression]")
{
    // End-to-end through CacheEngine: `decr` by 2^63 used to compute
    // `-static_cast<int64_t>(delta)` (negating INT64_MIN is UB), and `incr`
    // by 2^63 aliased to a decrement via the signed cast. The delta is now a
    // full uint64 with a direction flag.
    SECTION("incr by 2^63 adds rather than decrementing")
    {
        TextFixture fix;
        auto const r = Exchange(fix, "set c 0 0 1\r\n0\r\nincr c 9223372036854775808\r\n");
        REQUIRE(r == "STORED\r\n9223372036854775808\r\n");
    }
    SECTION("decr by 2^63 saturates to zero (no signed-overflow UB)")
    {
        TextFixture fix;
        auto const r = Exchange(fix, "set c 0 0 1\r\n5\r\ndecr c 9223372036854775808\r\n");
        REQUIRE(r == "STORED\r\n0\r\n");
    }
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

TEST_CASE("memcached-text touch refreshes expiry and reports TOUCHED / NOT_FOUND", "[protocol][text][touch]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 60 1\r\nx\r\ntouch k 120\r\ntouch nope 10\r\n");
    REQUIRE(response == "STORED\r\nTOUCHED\r\nNOT_FOUND\r\n");
}

TEST_CASE("memcached-text touch noreply suppresses the response", "[protocol][text][touch]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 60 1\r\nx\r\ntouch k 120 noreply\r\n");
    REQUIRE(response == "STORED\r\n");
}

TEST_CASE("memcached-text gat returns the value and refreshes expiry", "[protocol][text][gat]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 7 60 5\r\nhello\r\ngat 120 k\r\n");
    REQUIRE(response == "STORED\r\nVALUE k 7 5\r\nhello\r\nEND\r\n");
}

TEST_CASE("memcached-text gats returns the CAS token along with the value", "[protocol][text][gat]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 60 1\r\nA\r\ngats 120 k\r\n");
    // After set CAS=1; touch bumps to CAS=2.
    REQUIRE(response == "STORED\r\nVALUE k 0 1 2\r\nA\r\nEND\r\n");
}

TEST_CASE("memcached-text gat with a missing key returns END only", "[protocol][text][gat]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "gat 120 nope\r\n");
    REQUIRE(response == "END\r\n");
}

TEST_CASE("memcached-text gat returns every key in a large multi-key request (regression)",
          "[protocol][text][gat][regression]")
{
    // The fixed 16-token Tokenize cap silently dropped keys past the 14th in
    // a multi-key gat/get; every key must now be returned.
    TextFixture fix;
    std::string req;
    for (int i = 0; i < 20; ++i)
        req += std::format("set k{} 0 60 1\r\nV\r\n", i);
    req += "gat 120";
    for (int i = 0; i < 20; ++i)
        req += std::format(" k{}", i);
    req += "\r\n";

    auto const response = Exchange(fix, req);
    for (int i = 0; i < 20; ++i)
        REQUIRE(response.contains(std::format("VALUE k{} 0 1", i)));
}

TEST_CASE("memcached-text cache_memlimit reconfigures the storage budget", "[protocol][text][admin]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "cache_memlimit 4\r\n");
    REQUIRE(response == "OK\r\n");
    REQUIRE(fix.storage.Snapshot().bytesLimit == 4U * 1024U * 1024U);
}

TEST_CASE("memcached-text cache_memlimit rejects an overflowing argument (regression)",
          "[protocol][text][admin][regression]")
{
    // megabytes * 1MiB must not wrap size_t to a tiny budget (mass eviction)
    // or zero (unlimited).
    TextFixture fix;
    auto const before = fix.storage.Snapshot().bytesLimit;
    auto const response = Exchange(fix, "cache_memlimit 18446744073709551615\r\n"); // 2^64-1 MB
    REQUIRE(response.contains("CLIENT_ERROR"));
    REQUIRE(fix.storage.Snapshot().bytesLimit == before); // budget left unchanged
}

TEST_CASE("memcached-text verbosity is accepted as a no-op", "[protocol][text][admin]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "verbosity 2\r\n");
    REQUIRE(response == "OK\r\n");
}

TEST_CASE("memcached-text stats settings returns STAT lines", "[protocol][text][stats]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "stats settings\r\n");
    REQUIRE(response.starts_with("STAT maxbytes "));
    REQUIRE(response.ends_with("END\r\n"));
    REQUIRE(response.contains("STAT cas_enabled yes"));
}

TEST_CASE("memcached-text stats items reports the synthetic LRU class", "[protocol][text][stats]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 0 1\r\nx\r\nstats items\r\n");
    REQUIRE(response.contains("STAT items:1:number 1"));
}

TEST_CASE("memcached-text stats slabs reports a single synthetic class", "[protocol][text][stats]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "stats slabs\r\n");
    REQUIRE(response.contains("STAT active_slabs 1"));
    REQUIRE(response.ends_with("END\r\n"));
}

TEST_CASE("memcached-text stats sizes emits a single approximate bucket", "[protocol][text][stats]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 0 4\r\nXXXX\r\nstats sizes\r\n");
    REQUIRE(response.contains("STAT 4 1\r\n"));
}

TEST_CASE("memcached-text stats reset is acknowledged", "[protocol][text][stats]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "stats reset\r\n");
    REQUIRE(response == "RESET\r\n");
}

TEST_CASE("memcached-text stats includes the new touch / cas / incr counters", "[protocol][text][stats]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "set k 0 0 1\r\nx\r\ntouch k 60\r\nstats\r\n");
    REQUIRE(response.contains("STAT touch_hits 1"));
    REQUIRE(response.contains("STAT cmd_touch 1"));
    REQUIRE(response.contains("STAT cas_hits "));
}

TEST_CASE("memcached-text slabs/lru/lru_crawler stubs reply OK", "[protocol][text][stubs]")
{
    TextFixture fix;
    auto const response = Exchange(fix, "slabs reassign 1 2\r\nlru tune\r\nlru_crawler enable\r\n");
    REQUIRE(response == "OK\r\nOK\r\nOK\r\n");
}
