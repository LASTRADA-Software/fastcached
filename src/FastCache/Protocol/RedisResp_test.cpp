// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/RedisResp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct RespFixture
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;
};

FastCache::Task<bool> WriteString(FastCache::ISocket* s, std::string_view payload)
{
    auto const r = co_await s->Write(FastCache::AsBytes(payload));
    co_return r.has_value();
}

FastCache::Task<std::string> DrainResponse(FastCache::ISocket* s)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const r = co_await s->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        for (std::size_t i = 0; i < *r; ++i)
            out.push_back(static_cast<char>(chunk[i]));
        if (*r < chunk.size())
            break;
    }
    co_return out;
}

std::string Exchange(RespFixture& fix, std::string_view request)
{
    REQUIRE(FastCache::SyncRun(WriteString(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primer*/ {}));
    return FastCache::SyncRun(DrainResponse(fix.pair.client.get()));
}

} // namespace

TEST_CASE("RESP: PING/PONG inline", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "PING\r\n") == "+PONG\r\n");
}

TEST_CASE("RESP: PING with payload echoes back as bulk", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n") == "$5\r\nhello\r\n");
}

TEST_CASE("RESP: SELECT accepts any database index with +OK", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n") == "+OK\r\n");
}

TEST_CASE("RESP: CLIENT SETNAME/SETINFO acknowledged, ID/GETNAME answered", "[protocol][resp]")
{
    {
        RespFixture fix;
        REQUIRE(Exchange(fix, "*3\r\n$6\r\nCLIENT\r\n$7\r\nSETNAME\r\n$2\r\nhi\r\n") == "+OK\r\n");
    }
    {
        RespFixture fix;
        REQUIRE(Exchange(fix, "*2\r\n$6\r\nCLIENT\r\n$2\r\nID\r\n") == ":1\r\n");
    }
    {
        RespFixture fix;
        REQUIRE(Exchange(fix, "*2\r\n$6\r\nCLIENT\r\n$7\r\nGETNAME\r\n") == "$0\r\n\r\n");
    }
}

TEST_CASE("RESP: CONFIG GET echoes requested params with a synthetic value", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*3\r\n$6\r\nCONFIG\r\n$3\r\nGET\r\n$9\r\nmaxmemory\r\n")
            == "*2\r\n$9\r\nmaxmemory\r\n$1\r\n0\r\n");
}

TEST_CASE("RESP: CONFIG SET is accepted as a no-op", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$9\r\nmaxmemory\r\n$1\r\n0\r\n") == "+OK\r\n");
}

TEST_CASE("RESP: SET then GET round-trip via the array form", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n$5\r\nhello\r\n");
}

TEST_CASE("RESP: GET miss yields nil", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*2\r\n$3\r\nGET\r\n$3\r\nmis\r\n") == "$-1\r\n");
}

TEST_CASE("RESP: SET with NX refuses overwrite", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nfst\r\n$2\r\nNX\r\n"
                              "*4\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nsnd\r\n$2\r\nNX\r\n");
    REQUIRE(out == "+OK\r\n$-1\r\n");
}

TEST_CASE("RESP: SETEX persists with TTL", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$2\r\n60\r\n$2\r\nhi\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n$2\r\nhi\r\n");
}

TEST_CASE("RESP: DEL returns the deletion count", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
                              "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n"
                              "*3\r\n$3\r\nDEL\r\n$1\r\na\r\n$1\r\nc\r\n");
    REQUIRE(out == "+OK\r\n+OK\r\n:1\r\n");
}

TEST_CASE("RESP: EXISTS counts present keys", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
                              "*3\r\n$6\r\nEXISTS\r\n$1\r\na\r\n$1\r\nb\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n");
}

TEST_CASE("RESP: HELLO 3 returns -NOPROTO", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n");
    REQUIRE(out.starts_with("-NOPROTO"));
}

TEST_CASE("RESP: QUIT closes the connection after +OK", "[protocol][resp]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*1\r\n$4\r\nQUIT\r\n") == "+OK\r\n");
}

TEST_CASE("RESP: unknown command yields -ERR", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*1\r\n$8\r\nBOGUSCMD\r\n");
    REQUIRE(out.starts_with("-ERR "));
}

TEST_CASE("RESP: FLUSHDB wipes all entries", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*1\r\n$7\r\nFLUSHDB\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n+OK\r\n$-1\r\n");
}
