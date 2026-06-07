// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

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

std::string Exchange(RespFixture& fix, std::string_view request, FastCache::SessionContext session = {})
{
    REQUIRE(FastCache::SyncRun(WriteString(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primer*/ {}, session));
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

TEST_CASE("RESP: HELLO 3 negotiates RESP3 and replies a map", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n");
    // RESP3 map header for the 6 server fields, with proto reported as 3.
    REQUIRE(out.starts_with("%6\r\n"));
    REQUIRE(out.contains("$5\r\nproto\r\n:3\r\n"));
    REQUIRE(out.contains("$6\r\nserver\r\n$10\r\nfastcached\r\n"));
}

TEST_CASE("RESP: HELLO 2 replies a flat array with proto 2", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n2\r\n");
    // RESP2 renders the same map as a flat array of 12 elements.
    REQUIRE(out.starts_with("*12\r\n"));
    REQUIRE(out.contains("$5\r\nproto\r\n:2\r\n"));
}

TEST_CASE("RESP: HELLO with an unsupported version replies -NOPROTO", "[protocol][resp]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n4\r\n");
    REQUIRE(out.starts_with("-NOPROTO"));
}

TEST_CASE("RESP: GET miss is _ under RESP3 but $-1 under RESP2", "[protocol][resp]")
{
    {
        RespFixture fix;
        // Negotiate RESP3, then a missing GET must use the null type.
        auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n*2\r\n$3\r\nGET\r\n$4\r\nnope\r\n");
        REQUIRE(out.ends_with("_\r\n"));
    }
    {
        RespFixture fix;
        auto const out = Exchange(fix, "*2\r\n$3\r\nGET\r\n$4\r\nnope\r\n");
        REQUIRE(out == "$-1\r\n");
    }
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

TEST_CASE("RESP: AUTH with no password set yields -ERR", "[protocol][resp][auth]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$4\r\nAUTH\r\n$1\r\nx\r\n");
    REQUIRE(out.starts_with("-ERR Client sent AUTH"));
}

TEST_CASE("RESP: data command before AUTH yields -NOAUTH when auth is required", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", session);
    REQUIRE(out.starts_with("-NOAUTH"));
}

TEST_CASE("RESP: PING before AUTH is also gated", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    REQUIRE(Exchange(fix, "PING\r\n", session).starts_with("-NOAUTH"));
}

TEST_CASE("RESP: AUTH with wrong password yields -WRONGPASS", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$4\r\nAUTH\r\n$5\r\nwrong\r\n", session);
    REQUIRE(out.starts_with("-WRONGPASS"));
}

TEST_CASE("RESP: correct AUTH unlocks subsequent commands", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$4\r\nAUTH\r\n$6\r\ns3cr3t\r\n"
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
                              session);
    REQUIRE(out == "+OK\r\n+OK\r\n$1\r\nv\r\n");
}

TEST_CASE("RESP: AUTH accepts the username/password form", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "alice", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$4\r\nAUTH\r\n$5\r\nalice\r\n$6\r\ns3cr3t\r\n"
                              "*2\r\n$3\r\nGET\r\n$3\r\nmis\r\n",
                              session);
    REQUIRE(out == "+OK\r\n$-1\r\n");
}

TEST_CASE("RESP: set commands round-trip (SADD/SCARD/SISMEMBER/SMEMBERS/SREM)", "[protocol][resp][set]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n" // -> :2
                              "*2\r\n$5\r\nSCARD\r\n$1\r\ns\r\n"                      // -> :2
                              "*3\r\n$9\r\nSISMEMBER\r\n$1\r\ns\r\n$1\r\na\r\n"       // -> :1
                              "*3\r\n$9\r\nSISMEMBER\r\n$1\r\ns\r\n$1\r\nz\r\n"       // -> :0
                              "*2\r\n$8\r\nSMEMBERS\r\n$1\r\ns\r\n"                   // -> *2 a b (sorted)
                              "*3\r\n$4\r\nSREM\r\n$1\r\ns\r\n$1\r\na\r\n");          // -> :1
    REQUIRE(out == ":2\r\n:2\r\n:1\r\n:0\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n:1\r\n");
}

TEST_CASE("RESP: SADD is idempotent (re-adding a member returns 0)", "[protocol][resp][set]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                              "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n");
    REQUIRE(out == ":1\r\n:0\r\n");
}

TEST_CASE("RESP: SISMEMBER is a boolean under RESP3", "[protocol][resp][set]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n"
                              "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                              "*3\r\n$9\r\nSISMEMBER\r\n$1\r\ns\r\n$1\r\na\r\n"
                              "*3\r\n$9\r\nSISMEMBER\r\n$1\r\ns\r\n$1\r\nz\r\n");
    REQUIRE(out.ends_with("#t\r\n#f\r\n"));
}

TEST_CASE("RESP: SMEMBERS is a set (~) under RESP3", "[protocol][resp][set]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n"
                              "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                              "*2\r\n$8\r\nSMEMBERS\r\n$1\r\ns\r\n");
    REQUIRE(out.ends_with("~1\r\n$1\r\na\r\n"));
}

TEST_CASE("RESP: a string command on a set key is WRONGTYPE", "[protocol][resp][set]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\ns\r\n");
    REQUIRE(out == ":1\r\n-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
}

TEST_CASE("RESP: DEBUG PROTOCOL exercises each RESP3 type", "[protocol][resp][debug]")
{
    auto debug = [](std::string_view type, std::string_view hello) {
        RespFixture fix;
        std::string req { hello };
        req += "*3\r\n$5\r\nDEBUG\r\n$8\r\nPROTOCOL\r\n$";
        req += std::to_string(type.size());
        req += "\r\n";
        req += type;
        req += "\r\n";
        return Exchange(fix, req);
    };
    constexpr std::string_view Hello3 = "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n";
    REQUIRE(debug("null", Hello3).ends_with("_\r\n"));
    REQUIRE(debug("true", Hello3).ends_with("#t\r\n"));
    REQUIRE(debug("false", Hello3).ends_with("#f\r\n"));
    REQUIRE(debug("double", Hello3).ends_with(",1.5\r\n"));
    REQUIRE(debug("bignum", Hello3).ends_with("(1234567999999999999999999999999999999\r\n"));
    REQUIRE(debug("verbatim", Hello3).contains("=29\r\ntxt:This is a verbatim\nstring\r\n"));
    REQUIRE(debug("attrib", Hello3).contains("|1\r\n"));
}

TEST_CASE("RESP: HELLO 3 AUTH authenticates inline as the first command", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    // The redis-py RESP3 handshake: HELLO 3 AUTH <user> <pass> as the very first
    // command. It must authenticate AND switch to RESP3, then serve commands.
    auto const out = Exchange(fix,
                              "*5\r\n$5\r\nHELLO\r\n$1\r\n3\r\n$4\r\nAUTH\r\n$7\r\ndefault\r\n$6\r\ns3cr3t\r\n"
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
                              session);
    REQUIRE(out.starts_with("%6\r\n"));             // HELLO map (authenticated)
    REQUIRE(out.contains("$5\r\nproto\r\n:3\r\n")); // RESP3 negotiated
    REQUIRE(out.ends_with("+OK\r\n$1\r\nv\r\n"));   // SET + GET served
}

TEST_CASE("RESP: HELLO without auth is -NOAUTH when a password is required", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n", session);
    REQUIRE(out.starts_with("-NOAUTH"));
}

TEST_CASE("RESP: HELLO AUTH with a wrong password is -WRONGPASS", "[protocol][resp][auth]")
{
    FastCache::AuthPolicy const policy { "default", "s3cr3t" };
    FastCache::SharedAuthSource authSource { std::make_shared<FastCache::AuthPolicy const>(policy) };
    FastCache::SessionContext const session { .authSource = &authSource };
    RespFixture fix;
    auto const out =
        Exchange(fix, "*5\r\n$5\r\nHELLO\r\n$1\r\n3\r\n$4\r\nAUTH\r\n$7\r\ndefault\r\n$5\r\nwrong\r\n", session);
    REQUIRE(out.starts_with("-WRONGPASS"));
}
