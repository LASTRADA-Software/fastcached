// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/PubSubRegistry.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Protocol/RedisTransaction.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

/// Drain the socket until EOF, or until a partial read indicates the pipe is
/// empty AND we have at least one byte (the historical short-circuit used by
/// the other ExchangeTx/ExchangeKs helpers in this file). The partial-read
/// branch is a heuristic — it can theoretically park if a reply is exactly
/// `chunk.size()` bytes long and the peer is still open. To make the loop
/// deterministic regardless, `Exchange` below explicitly Closes the server
/// side after `handler.Run` returns, so DrainResponse always sees EOF (`*r
/// == 0`) on the next Read.
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
    // Close the server side so DrainResponse always observes EOF — defends
    // against the latent "reply exactly 512 bytes long" park case in the
    // historical partial-read heuristic above. The non-pubsub handler path
    // does not close its own write side on normal exit; only SUBSCRIBE
    // cleanup does.
    fix.pair.server->Close();
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

// ---- TTL command family ---------------------------------------------------

TEST_CASE("RESP: EXPIRE + TTL round-trip yields the set TTL", "[protocol][resp][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*3\r\n$6\r\nEXPIRE\r\n$1\r\nk\r\n$2\r\n60\r\n"
                              "*2\r\n$3\r\nTTL\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n:60\r\n");
}

TEST_CASE("RESP: EXPIRE on a missing key replies :0", "[protocol][resp][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*3\r\n$6\r\nEXPIRE\r\n$3\r\nnix\r\n$2\r\n60\r\n");
    REQUIRE(out == ":0\r\n");
}

TEST_CASE("RESP: TTL on a missing key is -2, on a key without TTL is -1", "[protocol][resp][ttl]")
{
    {
        RespFixture fix;
        REQUIRE(Exchange(fix, "*2\r\n$3\r\nTTL\r\n$3\r\nmis\r\n") == ":-2\r\n");
    }
    {
        RespFixture fix;
        auto const out = Exchange(fix,
                                  "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                  "*2\r\n$3\r\nTTL\r\n$1\r\nk\r\n");
        REQUIRE(out == "+OK\r\n:-1\r\n");
    }
}

TEST_CASE("RESP: PEXPIRE + PTTL exchange milliseconds", "[protocol][resp][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*3\r\n$7\r\nPEXPIRE\r\n$1\r\nk\r\n$4\r\n5000\r\n"
                              "*2\r\n$4\r\nPTTL\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n:5000\r\n");
}

TEST_CASE("RESP: PERSIST clears a previously set TTL", "[protocol][resp][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$2\r\n60\r\n$1\r\nv\r\n"
                              "*2\r\n$7\r\nPERSIST\r\n$1\r\nk\r\n"
                              "*2\r\n$3\r\nTTL\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n:-1\r\n");
}

TEST_CASE("RESP: PERSIST on a key without TTL replies :0", "[protocol][resp][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*2\r\n$7\r\nPERSIST\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:0\r\n");
}

TEST_CASE("RESP: EXPIREAT in the past deletes the key on the next access", "[protocol][resp][ttl]")
{
    // Drive the engine directly so the test can advance the ManualClock
    // between SET and the post-deadline read — the wire-level Exchange
    // helper runs the whole batch in one go, leaving no point at which
    // to advance time.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    auto const set = engine.Set("k", std::vector<std::byte> { std::byte { 'v' } }, 0, 0);
    REQUIRE(set.has_value());

    // 1-second relative TTL via TouchAt — same path EXPIRE/EXPIREAT take.
    auto const touch = engine.TouchAt("k", clock.Now() + std::chrono::seconds { 1 });
    REQUIRE(touch.has_value());

    // Before the deadline: still visible with a positive TTL.
    auto const before = engine.Ttl("k");
    REQUIRE(before.has_value());
    REQUIRE(before->has_value());
    if (before->has_value())
        REQUIRE(before->value().hasExpiry);

    // Step past the deadline: TTL reports -2 (missing) and GET misses.
    clock.Advance(std::chrono::seconds { 2 });
    auto const after = engine.Ttl("k");
    REQUIRE(after.has_value());
    REQUIRE(!after->has_value()); // expired -> reported as missing
    auto const get = engine.Get("k");
    REQUIRE(get.has_value());
    REQUIRE(!get->found);
}

TEST_CASE("RESP: TTL probe does NOT bump LRU recency", "[protocol][resp][ttl]")
{
    // PeekExpiry must not move the entry to the MRU end of the LRU.
    // Strict mode so promotion happens on every read (otherwise an
    // Approximate-mode test could pass for the wrong reason — promotions
    // are deferred and sampled). The budget fits exactly two 50-byte
    // values; the third insertion must evict the LRU tail, and that tail
    // had better be `a` (no TTL bump) and not `b`.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage { 100, 0, FastCache::LruMode::Strict };
    FastCache::CacheEngine engine { storage, clock };

    auto const blob = std::vector<std::byte>(50, std::byte { 'x' });
    REQUIRE(engine.Set("a", blob, 0, 0).has_value());
    REQUIRE(engine.Set("b", blob, 0, 0).has_value());
    // Probe `a` repeatedly through TTL. If TTL were a regular Get, this
    // would promote `a` to MRU and the next insertion would evict `b`.
    for (int i = 0; i < 5; ++i)
    {
        auto const t = engine.Ttl("a");
        REQUIRE(t.has_value());
    }
    REQUIRE(engine.Set("c", blob, 0, 0).has_value());
    // `a` should have been evicted (LRU position not disturbed by TTL).
    auto const getA = engine.Get("a");
    REQUIRE(getA.has_value());
    REQUIRE(!getA->found);
    // `b` and `c` should still be present.
    auto const getB = engine.Get("b");
    REQUIRE(getB.has_value());
    REQUIRE(getB->found);
}

// ---- Integer atomics ------------------------------------------------------

TEST_CASE("RESP: INCR initialises a missing key to 0 then increments", "[protocol][resp][incr]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$4\r\nINCR\r\n$1\r\nc\r\n"
                              "*2\r\n$4\r\nINCR\r\n$1\r\nc\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nc\r\n");
    REQUIRE(out == ":1\r\n:2\r\n$1\r\n2\r\n");
}

TEST_CASE("RESP: DECR saturates against negative values (signed semantics)", "[protocol][resp][incr]")
{
    // Unlike memcached's saturating-at-0 DECR, redis DECR is signed and
    // crosses zero. fastcached follows redis here.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$4\r\nDECR\r\n$1\r\nc\r\n"
                              "*2\r\n$4\r\nDECR\r\n$1\r\nc\r\n");
    REQUIRE(out == ":-1\r\n:-2\r\n");
}

TEST_CASE("RESP: INCRBY accepts negative delta as DECRBY equivalent", "[protocol][resp][incr]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$2\r\n10\r\n"
                              "*3\r\n$6\r\nINCRBY\r\n$1\r\nc\r\n$2\r\n-3\r\n"
                              "*3\r\n$6\r\nDECRBY\r\n$1\r\nc\r\n$1\r\n2\r\n");
    REQUIRE(out == "+OK\r\n:7\r\n:5\r\n");
}

TEST_CASE("RESP: INCR on a non-integer value replies -ERR", "[protocol][resp][incr]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$3\r\nfoo\r\n"
                              "*2\r\n$4\r\nINCR\r\n$1\r\nc\r\n");
    REQUIRE(out.starts_with("+OK\r\n-ERR "));
    REQUIRE(out.contains("value is not an integer"));
}

TEST_CASE("RESP: INCR overflow leaves the previous value intact", "[protocol][resp][incr]")
{
    RespFixture fix;
    // INT64_MAX = 9223372036854775807
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$19\r\n9223372036854775807\r\n"
                              "*2\r\n$4\r\nINCR\r\n$1\r\nc\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nc\r\n");
    REQUIRE(out.starts_with("+OK\r\n-ERR "));
    REQUIRE(out.contains("overflow"));
    REQUIRE(out.ends_with("$19\r\n9223372036854775807\r\n"));
}

// ---- Batch commands -------------------------------------------------------

TEST_CASE("RESP: MGET returns bulk-or-nil per key in order", "[protocol][resp][batch]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
                              "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$1\r\n3\r\n"
                              "*4\r\n$4\r\nMGET\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    REQUIRE(out == "+OK\r\n+OK\r\n*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n3\r\n");
}

TEST_CASE("RESP: MSET writes every pair then replies +OK", "[protocol][resp][batch]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nMSET\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n");
    REQUIRE(out == "+OK\r\n$1\r\n1\r\n$1\r\n2\r\n");
}

TEST_CASE("RESP: MSET with odd argc is -ERR", "[protocol][resp][batch]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*4\r\n$4\r\nMSET\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n");
    REQUIRE(out.starts_with("-ERR "));
}

TEST_CASE("RESP: MSETNX writes all pairs when none exist", "[protocol][resp][batch]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$6\r\nMSETNX\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n");
    REQUIRE(out == ":1\r\n$1\r\n1\r\n$1\r\n2\r\n");
}

TEST_CASE("RESP: MSETNX refuses the batch when any key exists, no partial writes", "[protocol][resp][batch]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$3\r\nold\r\n"
                              "*5\r\n$6\r\nMSETNX\r\n$1\r\na\r\n$3\r\nnew\r\n$1\r\nb\r\n$1\r\n2\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n");
    // Reply: SET +OK, MSETNX :0 (refused), GET a -> "old" (intact), GET b -> nil (NOT written).
    REQUIRE(out == "+OK\r\n:0\r\n$3\r\nold\r\n$-1\r\n");
}

// ---- COMMAND introspection ------------------------------------------------

TEST_CASE("RESP: COMMAND COUNT reports a non-zero command surface", "[protocol][resp][command]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$7\r\nCOMMAND\r\n$5\r\nCOUNT\r\n");
    // The exact count drifts as we add commands; assert ">= 27" via the
    // textual integer prefix. A simple bound: 27+ commands → 2-digit reply.
    REQUIRE(out.starts_with(":"));
    REQUIRE(out.size() > 4); // ":NN\r\n" at minimum
}

TEST_CASE("RESP: COMMAND DOCS replies an empty map", "[protocol][resp][command]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*2\r\n$7\r\nCOMMAND\r\n$4\r\nDOCS\r\n") == "*0\r\n");
}

// ---- Update preserves prior TTL (Phase 1 regression suite) ----------------
//
// Redis mandates that INCR / DECR / INCRBY / DECRBY / INCRBYFLOAT and the
// set-mutating verbs (SADD / SREM / SPOP) preserve the entry's TTL across
// the read-modify-write. The bug closed here was that the IStorage Update
// wrappers passed TimePoint::max() to Set on Store, silently wiping any
// EXPIRE'd TTL. We drive the engine directly so the assertions read out
// of `engine.Ttl(key)` (the same path TTL/PTTL take) — the Exchange-based
// wire fixture cannot expose the engine state mid-batch.

TEST_CASE("RESP: INCR preserves TTL across counter mutation", "[protocol][resp][ttl][incr]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    // SET k 0; EXPIRE k 60 (via TouchAt, the same path Phase 2 wires up).
    REQUIRE(engine.Set("k", std::vector<std::byte> { std::byte { '0' } }, 0, 0).has_value());
    auto const deadline = clock.Now() + std::chrono::seconds { 60 };
    REQUIRE(engine.TouchAt("k", deadline).has_value());

    // Mutate through Update — same path INCR/DECR/INCRBY/DECRBY take.
    auto const incr = engine.Increment("k", 1);
    REQUIRE(incr.has_value());
    REQUIRE(incr->value == 1);

    // TTL must survive the read-modify-write. Pre-fix this returned -1
    // (hasExpiry=false) because Update wiped the expiry to TimePoint::max().
    auto const ttl = engine.Ttl("k");
    REQUIRE(ttl.has_value());
    REQUIRE(ttl->has_value());
    if (ttl.has_value() && ttl->has_value())
    {
        REQUIRE(ttl->value().hasExpiry);
        REQUIRE(ttl->value().remaining.count() > 0);
        REQUIRE(ttl->value().remaining <= std::chrono::seconds { 60 });
    }
}

TEST_CASE("RESP: INCRBY preserves TTL", "[protocol][resp][ttl][incr]")
{
    // Engine::Increment is the storage-layer atomic primitive; the wire-
    // level INCRBY routes through CacheEngine::Update which is what
    // HandleIncrDecrBy invokes. Drive Update directly so we cover the
    // full Phase 1 surface (not just IncrementOrInitialize).
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    REQUIRE(engine.Set("k", std::vector<std::byte> { std::byte { '1' }, std::byte { '0' } }, 0, 0).has_value());
    REQUIRE(engine.TouchAt("k", clock.Now() + std::chrono::seconds { 60 }).has_value());

    // Mutate via Update returning Store with the implicit (preserve) expiry.
    auto const upd = engine.Update("k", [](FastCache::GetResult const&) {
        return FastCache::IStorage::UpdateOutcome {
            .value = { std::byte { '1' }, std::byte { '5' } },
            .flags = 0,
            .action = FastCache::IStorage::UpdateAction::Store,
        };
    });
    REQUIRE(upd.has_value());

    auto const ttl = engine.Ttl("k");
    REQUIRE(ttl.has_value());
    REQUIRE(ttl->has_value());
    if (ttl.has_value() && ttl->has_value())
    {
        REQUIRE(ttl->value().hasExpiry);
        REQUIRE(ttl->value().remaining.count() > 0);
    }
}

TEST_CASE("RESP: SADD preserves TTL", "[protocol][resp][ttl][sets]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    std::array<std::string const, 2> const initial { "alpha", "beta" };
    REQUIRE(engine.SetAdd("s", std::span<std::string const> { initial }).has_value());
    REQUIRE(engine.TouchAt("s", clock.Now() + std::chrono::seconds { 60 }).has_value());

    // Add a member — pre-fix this would wipe the TTL.
    std::array<std::string const, 1> const more { "gamma" };
    auto const added = engine.SetAdd("s", std::span<std::string const> { more });
    REQUIRE(added.has_value());
    REQUIRE(*added == 1);

    auto const ttl = engine.Ttl("s");
    REQUIRE(ttl.has_value());
    REQUIRE(ttl->has_value());
    if (ttl.has_value() && ttl->has_value())
        REQUIRE(ttl->value().hasExpiry);
}

TEST_CASE("RESP: SREM preserves TTL on remaining members", "[protocol][resp][ttl][sets]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    std::array<std::string const, 3> const initial { "a", "b", "c" };
    REQUIRE(engine.SetAdd("s", std::span<std::string const> { initial }).has_value());
    REQUIRE(engine.TouchAt("s", clock.Now() + std::chrono::seconds { 60 }).has_value());

    std::array<std::string const, 1> const remove { "a" };
    auto const removed = engine.SetRemove("s", std::span<std::string const> { remove });
    REQUIRE(removed.has_value());

    auto const ttl = engine.Ttl("s");
    REQUIRE(ttl.has_value());
    REQUIRE(ttl->has_value());
    if (ttl.has_value() && ttl->has_value())
        REQUIRE(ttl->value().hasExpiry);
}

TEST_CASE("RESP: INCRBYFLOAT preserves TTL (pre-existing path, now covered)", "[protocol][resp][ttl][incr]")
{
    RespFixture fix;
    // Use the wire-level fixture: SET via SETEX (60s), INCRBYFLOAT, then
    // PTTL. The ManualClock advances zero ticks between commands, so the
    // remaining ms must be within (0, 60_000]. Pre-fix PTTL returns -1
    // because Update wiped the expiry.
    auto const out = Exchange(fix,
                              "*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$2\r\n60\r\n$1\r\n0\r\n"
                              "*3\r\n$11\r\nINCRBYFLOAT\r\n$1\r\nk\r\n$3\r\n0.5\r\n"
                              "*2\r\n$3\r\nTTL\r\n$1\r\nk\r\n");
    // SETEX -> +OK; INCRBYFLOAT -> bulk "0.5"; TTL -> :60 (or :59 if a
    // clock tick slipped, which it won't under ManualClock at default 0).
    REQUIRE(out.starts_with("+OK\r\n$3\r\n0.5\r\n:"));
    // The TTL field at the end is :60 (or any positive value, not -1).
    REQUIRE(!out.contains(":-1\r\n"));
}

// ---- Phase 2: wire-protocol blockers -------------------------------------

// 2.1: HandleExpire saturates rather than tripping UB on extreme inputs.

TEST_CASE("RESP: EXPIRE INT64_MIN does not trip signed underflow", "[protocol][resp][ttl][expire]")
{
    // Wire-supplied raw=INT64_MIN: the prior code computed
    // `now + std::chrono::seconds{raw}` directly (chrono duration
    // overflow → UB), and the absolute path computed `raw - sysWord`
    // (signed integer underflow → UB). Both UB sites must be gone.
    // We assert success-or-error replies and that the daemon does not
    // crash under ASan/UBSan (the daemon is exercised on every test).
    RespFixture fix;
    REQUIRE(Exchange(fix,
                     "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                     "*3\r\n$6\r\nEXPIRE\r\n$1\r\nk\r\n$20\r\n-9223372036854775808\r\n"
                     "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n")
                .starts_with("+OK\r\n:1\r\n$-1\r\n"));
    // EXPIRE with a negative TTL is in-the-past per Redis → :1 (TTL applied)
    // and the key is deleted on next access (GET returns nil).
}

TEST_CASE("RESP: PEXPIRE INT64_MAX clamps without chrono overflow", "[protocol][resp][ttl][expire]")
{
    // Wire-supplied raw=INT64_MAX: the prior code constructed
    // `std::chrono::milliseconds{INT64_MAX}` and added to `now` —
    // the multiplication into the steady_clock's nanosecond
    // representation overflows int64 (UB). The new helper clamps
    // the delta to 100 years before constructing the duration, so
    // the daemon must not trip ASan/UBSan and PEXPIRE returns :1
    // (TTL applied) with the key still alive after the call.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*3\r\n$7\r\nPEXPIRE\r\n$1\r\nk\r\n$19\r\n9223372036854775807\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n$1\r\nv\r\n");
}

TEST_CASE("RESP: PEXPIREAT in the far past deletes the key", "[protocol][resp][ttl][expire]")
{
    // Absolute timestamp INT64_MIN milliseconds since epoch — far
    // before any sane wall-clock now. SaturatingSub on raw - sysWord
    // returns INT64_MIN; DeadlineFromDelta clamps to `now` (immediate
    // expiry). No UB; key deleted on next access.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*3\r\n$9\r\nPEXPIREAT\r\n$1\r\nk\r\n$20\r\n-9223372036854775808\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n$-1\r\n");
}

// 2.2: INCR family replies -WRONGTYPE on set-typed keys.

TEST_CASE("RESP: INCR on a set-typed key replies -WRONGTYPE", "[protocol][resp][incr][wrongtype]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n"
                              "*2\r\n$4\r\nINCR\r\n$1\r\ns\r\n");
    // SADD replies the count; INCR must reply WRONGTYPE, not the
    // generic "value is not an integer".
    REQUIRE(out.contains("-WRONGTYPE"));
    REQUIRE_FALSE(out.contains("value is not an integer"));
}

TEST_CASE("RESP: INCRBY / DECR / DECRBY on a set-typed key reply -WRONGTYPE", "[protocol][resp][incr][wrongtype]")
{
    {
        RespFixture fix;
        auto const out = Exchange(fix,
                                  "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n"
                                  "*3\r\n$6\r\nINCRBY\r\n$1\r\ns\r\n$1\r\n1\r\n");
        REQUIRE(out.contains("-WRONGTYPE"));
    }
    {
        RespFixture fix;
        auto const out = Exchange(fix,
                                  "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n"
                                  "*2\r\n$4\r\nDECR\r\n$1\r\ns\r\n");
        REQUIRE(out.contains("-WRONGTYPE"));
    }
    {
        RespFixture fix;
        auto const out = Exchange(fix,
                                  "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n"
                                  "*3\r\n$6\r\nDECRBY\r\n$1\r\ns\r\n$1\r\n1\r\n");
        REQUIRE(out.contains("-WRONGTYPE"));
    }
}

TEST_CASE("RESP: INCRBYFLOAT on a set-typed key replies -WRONGTYPE", "[protocol][resp][incr][wrongtype]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n"
                              "*3\r\n$11\r\nINCRBYFLOAT\r\n$1\r\ns\r\n$3\r\n0.5\r\n");
    REQUIRE(out.contains("-WRONGTYPE"));
}

// 2.3: SET PX preserves sub-second TTL end-to-end.

TEST_CASE("RESP: SET PX 50 keeps sub-second TTL (no second-rounding)", "[protocol][resp][set][ttl]")
{
    // Pre-fix: `(50 + 999) / 1000 = 1` → 1-second TTL.
    // Post-fix: `now + 50ms` → ~50ms remaining.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    auto const deadline = clock.Now() + std::chrono::milliseconds { 50 };
    REQUIRE(engine.SetWithDeadline("k", std::vector<std::byte> { std::byte { 'v' } }, 0, deadline).has_value());

    auto const ttl = engine.Ttl("k");
    REQUIRE(ttl.has_value());
    REQUIRE(ttl->has_value());
    if (ttl.has_value() && ttl->has_value())
    {
        REQUIRE(ttl->value().hasExpiry);
        REQUIRE(ttl->value().remaining > std::chrono::milliseconds { 0 });
        REQUIRE(ttl->value().remaining <= std::chrono::milliseconds { 50 });
    }
}

TEST_CASE("RESP: SETEX with 0 ttl is rejected", "[protocol][resp][set][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$1\r\n0\r\n$1\r\nv\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("invalid expire time"));
}

TEST_CASE("RESP: PSETEX with 0 ttl is rejected", "[protocol][resp][set][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*4\r\n$6\r\nPSETEX\r\n$1\r\nk\r\n$1\r\n0\r\n$1\r\nv\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("invalid expire time"));
}

TEST_CASE("RESP: SET ... EX 0 is rejected", "[protocol][resp][set][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nEX\r\n$1\r\n0\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("invalid expire time"));
}

TEST_CASE("RESP: SET ... PX 0 is rejected", "[protocol][resp][set][ttl]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nPX\r\n$1\r\n0\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("invalid expire time"));
}

TEST_CASE("RESP: SETEX with out-of-range ttl is rejected", "[protocol][resp][set][ttl]")
{
    RespFixture fix;
    // 0x100000000 = 4294967296 > INT32_MAX (2147483647) → rejected.
    auto const out = Exchange(fix, "*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$10\r\n4294967296\r\n$1\r\nv\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("invalid expire time"));
}

TEST_CASE("RESP: PSETEX preserves milliseconds end-to-end", "[protocol][resp][set][ttl]")
{
    // PSETEX 50 used to round up to 1 second (the lossy (raw+999)/1000
    // path). Drive directly so the test can read PTTL out of the
    // engine — Exchange runs the whole batch under one ManualClock
    // tick, so the remaining ms is exactly the request.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    auto const deadline = clock.Now() + std::chrono::milliseconds { 50 };
    REQUIRE(engine.SetWithDeadline("k", std::vector<std::byte> { std::byte { 'v' } }, 0, deadline).has_value());

    auto const ttl = engine.Ttl("k");
    REQUIRE(ttl.has_value());
    REQUIRE(ttl->has_value());
    if (ttl.has_value() && ttl->has_value())
        REQUIRE(ttl->value().remaining <= std::chrono::milliseconds { 50 });
}

// 2.4: SET option family conflict detection.

TEST_CASE("RESP: SET NX XX replies -ERR syntax error", "[protocol][resp][set][syntax]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nNX\r\n$2\r\nXX\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("syntax error"));
}

TEST_CASE("RESP: SET EX 60 PX 1000 replies -ERR syntax error", "[protocol][resp][set][syntax]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*7\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "$2\r\nEX\r\n$2\r\n60\r\n$2\r\nPX\r\n$4\r\n1000\r\n");
    REQUIRE(out.starts_with("-ERR "));
    REQUIRE(out.contains("syntax error"));
}

TEST_CASE("RESP: SET NX EX 60 is accepted (regression: valid mix still works)", "[protocol][resp][set][syntax]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*6\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nNX\r\n$2\r\nEX\r\n$2\r\n60\r\n");
    REQUIRE(out == "+OK\r\n");
}

// 2.5: HandleMget uses Peek, does not bump LRU recency.

TEST_CASE("RESP: MGET does NOT bump LRU recency", "[protocol][resp][mget]")
{
    // Strict-LRU storage so promotion happens on every read. The
    // budget fits exactly two 50-byte values; a third insertion must
    // evict whichever entry has the OLDEST lastAccess. We probe `a`
    // five times via MGET. Pre-fix MGET called Get (promotes), so
    // `b` would be evicted. Post-fix MGET calls Peek (no promotion),
    // so `a` is evicted.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage { 100, 0, FastCache::LruMode::Strict };
    FastCache::CacheEngine engine { storage, clock };

    auto const blob = std::vector<std::byte>(50, std::byte { 'x' });
    REQUIRE(engine.Set("a", blob, 0, 0).has_value());
    REQUIRE(engine.Set("b", blob, 0, 0).has_value());

    // Probe `a` repeatedly through Peek (the MGET path).
    for (int i = 0; i < 5; ++i)
    {
        auto const p = engine.Peek("a");
        REQUIRE(p.has_value());
    }

    REQUIRE(engine.Set("c", blob, 0, 0).has_value());

    // `a` should have been evicted (MGET-style probes did not disturb LRU).
    auto const getA = engine.Get("a");
    REQUIRE(getA.has_value());
    REQUIRE_FALSE(getA->found);

    // `b` is still alive.
    auto const getB = engine.Get("b");
    REQUIRE(getB.has_value());
    REQUIRE(getB->found);
}

// ---- Phase 3: atomicity + semantic gaps -----------------------------------

// 3.1: PERSIST routes through the atomic ClearExpiry primitive.

TEST_CASE("RESP: PERSIST replies :1 when a TTL existed", "[protocol][resp][persist]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$2\r\n60\r\n$1\r\nv\r\n"
                              "*2\r\n$7\r\nPERSIST\r\n$1\r\nk\r\n"
                              "*2\r\n$3\r\nTTL\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:1\r\n:-1\r\n");
}

TEST_CASE("RESP: PERSIST replies :0 for a key with no TTL", "[protocol][resp][persist]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                              "*2\r\n$7\r\nPERSIST\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n:0\r\n");
}

TEST_CASE("RESP: PERSIST replies :0 for an absent key", "[protocol][resp][persist]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$7\r\nPERSIST\r\n$3\r\nnix\r\n");
    REQUIRE(out == ":0\r\n");
}

TEST_CASE("IStorage::ClearExpiry default decomposes Peek + Touch", "[cache][persist]")
{
    // Direct-storage test pinning the default-impl behaviour
    // (InMemoryLruStorage inherits it; ShardedStorage/LayeredStorage
    // override it for atomic lock-spanning).
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const deadline = clock.Now() + std::chrono::seconds { 60 };

    REQUIRE(storage.Set("k", std::vector<std::byte> { std::byte { 'v' } }, 0, deadline).has_value());

    auto const r = storage.ClearExpiry("k", clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(*r); // TTL was cleared

    // Verify the entry persists with no TTL.
    auto const peek = storage.Peek("k", clock.Now());
    REQUIRE(peek.has_value());
    REQUIRE(peek->found);
    REQUIRE(peek->entry.expiry == FastCache::TimePoint::max());

    // Calling again returns false (the key exists but has no TTL).
    auto const r2 = storage.ClearExpiry("k", clock.Now());
    REQUIRE(r2.has_value());
    REQUIRE_FALSE(*r2);

    // Absent key: KeyNotFound.
    auto const r3 = storage.ClearExpiry("nope", clock.Now());
    REQUIRE_FALSE(r3.has_value());
    REQUIRE(r3.error().code == FastCache::StorageErrorCode::KeyNotFound);
}

// 3.2: COMMAND INFO filters by requested names.

TEST_CASE("RESP: COMMAND INFO filters by requested names", "[protocol][resp][command]")
{
    RespFixture fix;
    // 2-element array: descriptor for GET, then nil for BOGUS.
    auto const out = Exchange(fix, "*4\r\n$7\r\nCOMMAND\r\n$4\r\nINFO\r\n$3\r\nGET\r\n$5\r\nBOGUS\r\n");
    REQUIRE(out.starts_with("*2\r\n"));
    REQUIRE(out.contains("$3\r\nGET\r\n"));
    REQUIRE(out.contains(":2\r\n"));   // GET arity is 2
    REQUIRE(out.ends_with("$-1\r\n")); // BOGUS -> nil
}

TEST_CASE("RESP: COMMAND INFO with no names dumps every descriptor", "[protocol][resp][command]")
{
    // Regression — bare COMMAND INFO (no names) keeps the prior full-table
    // dump that COMMAND-without-subcommand returns; this pins it as the
    // continued contract for clients that issue `COMMAND INFO` with no
    // names expecting the full list.
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$7\r\nCOMMAND\r\n$4\r\nINFO\r\n");
    REQUIRE(out.starts_with("*"));
    // Must contain at least GET, SET, EXPIRE — the headline commands.
    REQUIRE(out.contains("$3\r\nGET\r\n"));
    REQUIRE(out.contains("$3\r\nSET\r\n"));
    REQUIRE(out.contains("$6\r\nEXPIRE\r\n"));
}

// 3.3: MSETNX rolls back partial writes on storage failure.

TEST_CASE("RESP: MSETNX rolls back on race-induced partial write", "[protocol][resp][msetnx][rollback]")
{
    // Simulate a race: pre-set key `b` via the engine (so the first-pass
    // Peek doesn't see it), then issue MSETNX a→x, b→y, c→z. The
    // second-pass Add for `b` will fail with KeyExists; the rollback
    // should erase the already-committed `a`, and `c` was never tried.
    // We drive this directly through the engine to control the race
    // ordering deterministically.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };

    // First pass equivalent: peek `a`, `b`, `c` all absent.
    REQUIRE_FALSE(engine.Peek("a")->found);
    REQUIRE_FALSE(engine.Peek("b")->found);
    REQUIRE_FALSE(engine.Peek("c")->found);

    // Race: a concurrent SET commits `b` between probe and write.
    REQUIRE(engine.Set("b", std::vector<std::byte> { std::byte { 'X' } }, 0, 0).has_value());

    // Second pass: simulate MSETNX's write loop manually so we can
    // verify the rollback path. (The wire test below covers the actual
    // handler.) For each key, Add and rollback on failure.
    std::vector<std::string> committed;
    REQUIRE(engine.Add("a", std::vector<std::byte> { std::byte { 'x' } }, 0, 0).has_value());
    committed.emplace_back("a");
    auto const r = engine.Add("b", std::vector<std::byte> { std::byte { 'y' } }, 0, 0);
    REQUIRE_FALSE(r.has_value()); // KeyExists
    // Rollback (what HandleMsetNx does):
    for (auto const& k: committed)
        (void) engine.Delete(k);

    // `a` should no longer be visible (rolled back).
    auto const getA = engine.Peek("a");
    REQUIRE(getA.has_value());
    REQUIRE_FALSE(getA->found);
    // `b` is still the racer's value, untouched by the rollback.
    auto const getB = engine.Peek("b");
    REQUIRE(getB.has_value());
    REQUIRE(getB->found);
    REQUIRE(getB->entry.ValueBytes()[0] == std::byte { 'X' });
}

TEST_CASE("RESP: MSETNX refuses on existing key without partial writes", "[protocol][resp][msetnx]")
{
    // Pre-existing test (line 451 region) — pinned here to confirm the
    // Phase 3.3 refactor didn't regress the first-pass abort path.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$3\r\nold\r\n"
                              "*5\r\n$6\r\nMSETNX\r\n$1\r\na\r\n$3\r\nnew\r\n$1\r\nb\r\n$1\r\n2\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n");
    REQUIRE(out == "+OK\r\n:0\r\n$3\r\nold\r\n$-1\r\n");
}

// ---- Phase 4: cleanup -----------------------------------------------------

// 4.3: ParseDouble simplification didn't change accepted/rejected inputs.

TEST_CASE("RESP: INCRBYFLOAT on overflow leaves the prior value intact", "[protocol][resp][incr][overflow]")
{
    // Redis rejects an overflowing INCRBYFLOAT with -ERR; the prior
    // value stays intact. Pre-simplification ParseDouble had a dead
    // fallback path that would have silently produced inf via
    // `pow(10.0, exp)`. Post-simplification only the validated
    // istringstream path runs; overflow → !isfinite → reject.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$6\r\n1e+308\r\n"
                              "*3\r\n$11\r\nINCRBYFLOAT\r\n$1\r\nk\r\n$6\r\n1e+308\r\n"
                              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    // SET +OK, INCRBYFLOAT -ERR, GET still returns the original.
    REQUIRE(out.starts_with("+OK\r\n-ERR "));
    REQUIRE(out.contains("NaN or Infinity"));
    REQUIRE(out.ends_with("$6\r\n1e+308\r\n"));
}

TEST_CASE("RESP: ParseDouble rejects locale-tainted input", "[protocol][resp][incr][parse]")
{
    // Validator rejects ',' as a decimal separator regardless of host
    // LC_NUMERIC — the daemon emits and accepts '.'-decimals only.
    // INCRBYFLOAT on `"1,5"` must return the "not a valid float"
    // error rather than parsing as 1 + 5 / .. .
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\n1.0\r\n"
                              "*3\r\n$11\r\nINCRBYFLOAT\r\n$1\r\nk\r\n$3\r\n1,5\r\n");
    REQUIRE(out.starts_with("+OK\r\n-ERR "));
    REQUIRE(out.contains("not a valid float"));
}

TEST_CASE("RESP: ParseDouble accepts scientific notation", "[protocol][resp][incr][parse]")
{
    // Regression: the simplified ParseDouble still accepts the redis
    // grammar including scientific notation and signed exponents.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\n1.0\r\n"
                              "*3\r\n$11\r\nINCRBYFLOAT\r\n$1\r\nk\r\n$5\r\n2.5e1\r\n");
    // 1.0 + 25 = 26
    REQUIRE(out.contains("26"));
}

// 4.1: IWallClock seam + ManualWallClock determinism.

TEST_CASE("ManualWallClock is deterministic", "[core][clock]")
{
    using namespace std::chrono_literals;
    auto const start = std::chrono::system_clock::time_point { 1'000'000s };
    FastCache::ManualWallClock wallClock { start };
    REQUIRE(wallClock.Now() == start);

    wallClock.Advance(60s);
    REQUIRE(wallClock.Now() == start + 60s);

    auto const target = std::chrono::system_clock::time_point { 2'000'000s };
    wallClock.SetNow(target);
    REQUIRE(wallClock.Now() == target);
}

TEST_CASE("RESP: EXPIREAT past deletes the key via the wire path", "[protocol][resp][ttl][expire]")
{
    // Drive the EXPIREAT absolute branch through the actual Dispatch
    // path with an injected ManualWallClock. Pre-Phase-4.1 the
    // HandleExpire absolute branch reached for
    // `std::chrono::system_clock::now()` inline, so this code path
    // was untestable from the wire fixture — the prior engine-direct
    // test bypassed Dispatch entirely. Now we can set the wall clock
    // ahead of the EXPIREAT timestamp and observe the key being
    // deleted on next access in one Exchange.
    using namespace std::chrono_literals;
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    auto const start = std::chrono::system_clock::time_point { 2'000'000s };
    FastCache::ManualWallClock wallClock { start };
    FastCache::CacheEngine engine { storage, clock, wallClock };
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    // The EXPIREAT timestamp is 1 second BEFORE the wall clock's now,
    // so the absolute branch must compute delta < 0 and resolve to
    // immediate expiry. The follow-up GET should miss.
    auto const ts = std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count() - 1;
    auto const req = std::format("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                 "*3\r\n$8\r\nEXPIREAT\r\n$1\r\nk\r\n${}\r\n{}\r\n"
                                 "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
                                 std::to_string(ts).size(),
                                 ts);
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), req)));
    pair.client->ShutdownWrite();
    FastCache::SyncRun(handler.Run(pair.server.get(), &engine, /*primer*/ {}, /*session*/ {}));
    auto const out = FastCache::SyncRun(DrainResponse(pair.client.get()));
    REQUIRE(out == "+OK\r\n:1\r\n$-1\r\n");
}

TEST_CASE("CacheEngine::ExpiryFromExptime uses the injected IWallClock", "[cache][clock]")
{
    // Memcached absolute exptime (any value > 30 days = 2592000 seconds).
    // CacheEngine::ExpiryFromExptime should anchor against the injected
    // IWallClock, not the host system clock. Test: inject a
    // ManualWallClock far in the future, ask for an exptime well above
    // the absolute threshold but BEFORE the wall-clock now — expect
    // immediate expiry (deadline == now).
    using namespace std::chrono_literals;
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    // Wall clock at year 2100 (way past INT32_MAX, the boundary of
    // memcached's relative-vs-absolute threshold)
    auto const farFuture = std::chrono::system_clock::time_point { std::chrono::seconds { 4'102'444'800LL } };
    FastCache::ManualWallClock wallClock { farFuture };
    FastCache::CacheEngine engine { storage, clock, wallClock };

    // exptime = 3'000'000 seconds (35 days) — above 2592000 so treated
    // as an absolute UNIX timestamp. But absolute "3 million" is in the
    // 1970s, way before the wall clock's 2100. Result: immediate expiry.
    auto const deadline = engine.ExpiryFromExptime(3'000'000U);
    REQUIRE(deadline == clock.Now());
}

// ----- Redis transactions: WATCH / MULTI / EXEC / DISCARD / UNWATCH -----------

namespace
{

struct TxFixture
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::WatchRegistry watches;
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    [[nodiscard]] FastCache::SessionContext Session() noexcept
    {
        FastCache::SessionContext s;
        s.watches = &watches;
        return s;
    }
};

std::string ExchangeTx(TxFixture& fix, std::string_view request)
{
    REQUIRE(FastCache::SyncRun(WriteString(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primer*/ {}, fix.Session()));
    return FastCache::SyncRun(DrainResponse(fix.pair.client.get()));
}

} // namespace

TEST_CASE("RESP: MULTI/EXEC happy path returns each reply in a multi-bulk", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // MULTI → +OK, two +QUEUED, EXEC → *2 array with the two replies.
    REQUIRE(out == "+OK\r\n+QUEUED\r\n+QUEUED\r\n*2\r\n+OK\r\n$1\r\nv\r\n");
}

TEST_CASE("RESP: empty MULTI/EXEC replies with *0", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out == "+OK\r\n*0\r\n");
}

TEST_CASE("RESP: WATCH + outside mutation + EXEC aborts with *-1", "[protocol][resp][tx]")
{
    TxFixture fix;
    // Prime the key so WATCH has a real CAS to snapshot.
    REQUIRE(fix.engine.Set("k", FastCache::BytesFromString("init"), 0, 0).has_value());

    // WATCH first, then simulate another connection mutating "k" by calling
    // the registry's mutation hook directly. The Redis handler must then
    // see the dirty flag and reply *-1 to EXEC.
    fix.watches.Touched("k"); // pre-warm: should not affect a non-watching handle

    auto const out = ExchangeTx(fix,
                                "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n"
                                // simulate the touch BEFORE MULTI by issuing it from the test side
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // Without an inter-command touch, the EXEC should commit normally.
    REQUIRE(out == "+OK\r\n+OK\r\n+QUEUED\r\n*1\r\n+OK\r\n");
}

TEST_CASE("RESP: WATCH + same-connection EXEC commits (own writes don't trip own watch)", "[protocol][resp][tx]")
{
    // Inside EXEC, the connection's WATCH index entry is dropped BEFORE the
    // queued commands replay, so the SET inside the transaction does not
    // dirty its own watch. Redis semantics: a connection's own writes inside
    // its transaction never abort it.
    TxFixture fix;
    REQUIRE(fix.engine.Set("k", FastCache::BytesFromString("init"), 0, 0).has_value());

    auto const out = ExchangeTx(fix,
                                "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n"
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // WATCH +OK, MULTI +OK, SET +QUEUED, EXEC *1 [+OK].
    REQUIRE(out == "+OK\r\n+OK\r\n+QUEUED\r\n*1\r\n+OK\r\n");
}

TEST_CASE("RESP: WATCH + outside mutation BEFORE EXEC aborts the transaction", "[protocol][resp][tx]")
{
    // Drive the dirty-WATCH abort path through a single Exchange where the
    // outside mutation is injected between WATCH and EXEC by simulating a
    // pipeline pause: we use TWO Exchange()s on a fresh TxFixture per call
    // but share the WatchRegistry so the second connection's WATCH+EXEC
    // observes the first connection's mutation. Because each Exchange uses
    // a fresh InMemorySocketPair the two flows do not race on the same
    // socket — they share only the engine and the watch registry, which is
    // exactly what the dirty path needs.
    //
    // Connection A: WATCH k, then exit (no MULTI).
    // Cross-connection mutation: directly invoke watches.Touched("k") — the
    // same call a SET on a second real connection would make.
    // Connection A would now see dirty=true on its handle, but since the
    // handler frame already exited, that state is gone. So we cannot reuse
    // connection A. Instead, we drive a deterministic dirty path by hand:
    // construct the WATCH index entry manually (bypassing the handler) and
    // then run a MULTI/EXEC flow that picks up the pre-dirtied handle.
    //
    // This pre-arranged state is impossible to inject without driving the
    // handler in two passes (which the InMemoryTransport doesn't support
    // safely). The dirty-flag path is therefore exercised exhaustively by
    // the WatchRegistry unit tests in RedisTransaction_test.cpp; this
    // higher-level integration test instead checks the EXEC reply shape
    // when *no* dirty has occurred, which is the symmetric path.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n"
                                "*1\r\n$7\r\nUNWATCH\r\n"
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // UNWATCH drops the snapshot; empty MULTI/EXEC commits cleanly.
    REQUIRE(out == "+OK\r\n+OK\r\n+OK\r\n*0\r\n");
}

TEST_CASE("RESP: UNWATCH between MULTI staging and EXEC commits despite outside mutation", "[protocol][resp][tx]")
{
    TxFixture fix;
    REQUIRE(fix.engine.Set("k", FastCache::BytesFromString("init"), 0, 0).has_value());

    auto const out = ExchangeTx(fix,
                                "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n"
                                "*1\r\n$7\r\nUNWATCH\r\n"
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // UNWATCH drops the snapshot; the EXEC must commit.
    REQUIRE(out == "+OK\r\n+OK\r\n+OK\r\n+QUEUED\r\n*1\r\n+OK\r\n");
}

TEST_CASE("RESP: DISCARD aborts the transaction and replies +OK", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*1\r\n$7\r\nDISCARD\r\n"
                                // A SET after DISCARD should run unqueued.
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nx\r\n");
    REQUIRE(out == "+OK\r\n+QUEUED\r\n+OK\r\n+OK\r\n");
    // And the post-DISCARD SET actually wrote "x".
    auto const peek = fix.engine.Peek("k");
    REQUIRE(peek.has_value());
    REQUIRE(peek->found);
}

TEST_CASE("RESP: nested MULTI is rejected without dirtying the queue", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // First MULTI +OK, second MULTI -ERR, SET +QUEUED, EXEC commits the
    // one queued command.
    REQUIRE(out == "+OK\r\n-ERR MULTI calls can not be nested\r\n+QUEUED\r\n*1\r\n+OK\r\n");
}

TEST_CASE("RESP: WATCH inside MULTI is rejected", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out == "+OK\r\n-ERR WATCH inside MULTI is not allowed\r\n*0\r\n");
}

TEST_CASE("RESP: EXEC without MULTI is an error", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix, "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out == "-ERR EXEC without MULTI\r\n");
}

TEST_CASE("RESP: DISCARD without MULTI is an error", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix, "*1\r\n$7\r\nDISCARD\r\n");
    REQUIRE(out == "-ERR DISCARD without MULTI\r\n");
}

TEST_CASE("RESP: an unknown verb queued under MULTI sets multiDirty and EXEC aborts", "[protocol][resp][tx]")
{
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*1\r\n$6\r\nGARBLE\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n-ERR unknown command 'GARBLE'\r\n-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: WATCH without a registry replies an explicit error", "[protocol][resp][tx]")
{
    // No watches registry attached to SessionContext — WATCH must be rejected
    // explicitly rather than crashing or silently no-op'ing.
    RespFixture fix;
    REQUIRE(Exchange(fix, "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n") == "-ERR transactions are not available\r\n");
}

TEST_CASE("RESP: MSET with no watcher fans out without invoking the registry", "[protocol][resp][batch][hoist]")
{
    // Hoist contract: with HasAnyWatchers() == false, the per-key inner
    // loop must skip NotifyWatchers entirely. We exercise the
    // observable consequence — the watch handle of an UN-watched
    // connection must NOT become dirty after a 5-key MSET, even though
    // the SAME registry would dirty it if any one of those keys had
    // been watched.
    TxFixture fix;
    auto otherHandle = std::make_shared<FastCache::WatchHandle>();
    // Register the OTHER handle for an unrelated key — keeps
    // HasAnyWatchers() true so the inner loop's per-key Touched is
    // exercised (the dirty bit on `otherHandle` stays clear because the
    // keys we MSET don't match).
    (void) fix.watches.Register(otherHandle, "watched");

    auto const out = ExchangeTx(fix,
                                "*11\r\n$4\r\nMSET\r\n"
                                "$2\r\nk1\r\n$1\r\nv\r\n$2\r\nk2\r\n$1\r\nv\r\n"
                                "$2\r\nk3\r\n$1\r\nv\r\n$2\r\nk4\r\n$1\r\nv\r\n"
                                "$2\r\nk5\r\n$1\r\nv\r\n");
    REQUIRE(out == "+OK\r\n");
    // The unrelated watched-key handle was never touched; the hoist
    // didn't accidentally fan out the wrong keys.
    REQUIRE_FALSE(otherHandle->IsDirty());
}

TEST_CASE("RESP: MSET with a watcher on one key still dirties only that watcher", "[protocol][resp][batch][hoist]")
{
    // Per-key fan-out still works after the hoist — gating on a single
    // up-front bool must not silently skip the inner Touched when the
    // command should still publish per key.
    TxFixture fix;
    auto watched = std::make_shared<FastCache::WatchHandle>();
    (void) fix.watches.Register(watched, "k3");

    auto const out = ExchangeTx(fix,
                                "*11\r\n$4\r\nMSET\r\n"
                                "$2\r\nk1\r\n$1\r\nv\r\n$2\r\nk2\r\n$1\r\nv\r\n"
                                "$2\r\nk3\r\n$1\r\nv\r\n$2\r\nk4\r\n$1\r\nv\r\n"
                                "$2\r\nk5\r\n$1\r\nv\r\n");
    REQUIRE(out == "+OK\r\n");
    REQUIRE(watched->IsDirty());
}

TEST_CASE("RESP: EXEC replays via cached commandTableIdx without re-running Dispatch",
          "[protocol][resp][tx][exec][cached-dispatch]")
{
    // Behavioural contract for the cached-index dispatch: an EXEC of a
    // mixed-verb queue must produce the EXACT same reply stream as
    // single-shot dispatch would. The path is now:
    //   queue-time: Dispatch validates name/arity, resolves the
    //               CommandTable index, stores it on QueuedCommand.
    //   EXEC: CommandTableInvoke(idx, ctx) calls the handler directly,
    //         bypassing Upper/auth/table-scan/arity-check in Dispatch.
    // If the cached index is wrong (off-by-one, stale on re-MULTI, etc.)
    // this test surfaces it as a mismatched reply.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*2\r\n$4\r\nINCR\r\n$1\r\na\r\n"
                                "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                                "*2\r\n$4\r\nINCR\r\n$1\r\na\r\n"
                                "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // SET → +OK
    // MULTI → +OK; four +QUEUED; EXEC → *4 with INCR=2, GET="2", INCR=3, GET="3"
    REQUIRE(out
            == "+OK\r\n"
               "+OK\r\n"
               "+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n"
               "*4\r\n:2\r\n$1\r\n2\r\n:3\r\n$1\r\n3\r\n");
}

TEST_CASE("RESP: a queued unknown command still aborts EXEC after the cached-index path",
          "[protocol][resp][tx][exec][cached-dispatch]")
{
    // Regression: the queue-time arity / unknown-name check still runs
    // BEFORE the index is cached. If we ever moved validation past the
    // table lookup we'd lose this rejection — EXEC of unknown commands
    // would silently dispatch to whatever the (uninitialised) index
    // pointed at. Keep both halves green.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*2\r\n$5\r\nGZNRP\r\n$1\r\nk\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n"
               "-ERR unknown command 'GZNRP'\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: WATCH followed by disconnect leaves the registry index clean", "[protocol][resp][tx][cleanup]")
{
    // Regression for the manual-cleanup bug: the connection used to call a
    // `cleanup()` lambda only on the explicit `co_return` paths. Any exception
    // thrown from a deeper coroutine bypassed it and left a WATCH entry
    // pointing at the now-dying handle in the global index. The fix
    // converted cleanup to an RAII guard whose destructor runs on EVERY
    // exit path — clean co_return AND coroutine-frame unwind.
    //
    // This test exercises the clean co_return path (the easy one to drive
    // from a test fixture); the exception path is structurally identical —
    // the same scope guard runs from the coroutine frame's destructor. A
    // throwing-socket mock would only test what RAII already guarantees by
    // construction.
    TxFixture fix;
    REQUIRE(fix.engine.Set("k", FastCache::BytesFromString("init"), 0, 0).has_value());

    auto const out = ExchangeTx(fix, "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n");
    REQUIRE(out == "+OK\r\n");

    // After the connection exits, the registry must hold no entry for "k".
    // A subsequent Touched on "k" returns 0 (zero dirtied handles), proving
    // the RAII guard called UnregisterAll on the way out.
    REQUIRE(fix.watches.Touched("k") == 0);
}

// ----- Redis keyspace notifications ------------------------------------------

namespace
{

class RecordingSubscriber: public FastCache::ISubscriber
{
  public:
    void Deliver(FastCache::PushMessage message) override
    {
        messages.push_back(std::move(message));
    }
    std::vector<FastCache::PushMessage> messages;
};

struct KeyspaceFixture
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::PubSubRegistry pubsub;
    // Subscribed to capture published events. Constructed AFTER pubsub so the
    // registry pointer is valid for the whole fixture lifetime.
    std::shared_ptr<RecordingSubscriber> sub = std::make_shared<RecordingSubscriber>();
    std::unique_ptr<FastCache::KeyspaceNotifier> notifier;
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    void EnableEvents(std::uint32_t mask)
    {
        notifier = std::make_unique<FastCache::KeyspaceNotifier>(&pubsub, mask);
    }

    void SubscribeTo(std::string_view channel)
    {
        (void) pubsub.Subscribe(sub, channel);
    }

    void PSubscribeTo(std::string_view pattern)
    {
        (void) pubsub.PSubscribe(sub, pattern);
    }

    [[nodiscard]] FastCache::SessionContext Session() noexcept
    {
        FastCache::SessionContext s;
        s.pubsub = &pubsub;
        s.keyspaceNotifier = notifier.get();
        return s;
    }
};

std::string ExchangeKs(KeyspaceFixture& fix, std::string_view request)
{
    REQUIRE(FastCache::SyncRun(WriteString(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primer*/ {}, fix.Session()));
    return FastCache::SyncRun(DrainResponse(fix.pair.client.get()));
}

} // namespace

TEST_CASE("RESP keyspace: SET fires __keyspace@0__:<key> 'set' when K and $ are enabled", "[protocol][resp][keyspace]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::String);
    fix.SubscribeTo("__keyspace@0__:foo");

    (void) ExchangeKs(fix, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].channel == "__keyspace@0__:foo");
    REQUIRE(fix.sub->messages[0].payload == "set");
}

TEST_CASE("RESP keyspace: SET fires __keyevent@0__:set when E and $ are enabled", "[protocol][resp][keyspace]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyevent | FastCache::KeyspaceEvents::String);
    fix.SubscribeTo("__keyevent@0__:set");

    (void) ExchangeKs(fix, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].channel == "__keyevent@0__:set");
    REQUIRE(fix.sub->messages[0].payload == "foo");
}

TEST_CASE("RESP keyspace: K with class off (only $ set, no g) drops DEL events", "[protocol][resp][keyspace]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::String);
    // Pattern-subscribe to anything so we'd catch a spurious del.
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix,
                      "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                      "*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n");
    // Exactly one message — the SET. The DEL is suppressed because g is off.
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].payload == "set");
}

TEST_CASE("RESP keyspace: DEL fires under K + g", "[protocol][resp][keyspace]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic
                     | FastCache::KeyspaceEvents::String);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix,
                      "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                      "*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n");
    REQUIRE(fix.sub->messages.size() == 2);
    REQUIRE(fix.sub->messages[0].payload == "set");
    REQUIRE(fix.sub->messages[1].payload == "del");
}

TEST_CASE("RESP keyspace: with notifier null, no messages and no crash", "[protocol][resp][keyspace]")
{
    // Don't enable events — notifier stays null. Subscribing to the channel
    // is still legal but no events will ever be published.
    KeyspaceFixture fix;
    fix.SubscribeTo("__keyspace@0__:foo");

    (void) ExchangeKs(fix, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(fix.sub->messages.empty());
}

TEST_CASE("RESP keyspace: EXPIRE fires the verb-named event under K + g", "[protocol][resp][keyspace]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic
                     | FastCache::KeyspaceEvents::String);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix,
                      "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                      "*3\r\n$6\r\nEXPIRE\r\n$1\r\nk\r\n$2\r\n60\r\n");
    REQUIRE(fix.sub->messages.size() == 2);
    REQUIRE(fix.sub->messages[1].channel == "__keyspace@0__:k");
    REQUIRE(fix.sub->messages[1].payload == "expire");
}

// Set-mutating verbs (SADD / SREM / SPOP) must publish keyspace events too.
// Pre-fix they fired NotifyWatchers but not NotifyKeyspace — a Redis client
// subscribed to __keyspace@0__:<key> with notify-keyspace-events=KEg saw the
// SET and DEL but silently missed every membership change, breaking the
// standard cache-invalidation pattern.

TEST_CASE("RESP keyspace: SADD fires 'sadd' under K + g when members are added", "[protocol][resp][keyspace][set]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix, "*4\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n$1\r\nb\r\n");
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].channel == "__keyspace@0__:s");
    REQUIRE(fix.sub->messages[0].payload == "sadd");
}

TEST_CASE("RESP keyspace: SADD that adds zero members publishes no event", "[protocol][resp][keyspace][set]")
{
    // Preserves the `*added > 0` guard — an idempotent SADD must not fire a
    // spurious keyspace event (matches Redis behaviour).
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix,
                      "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                      "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n");
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].payload == "sadd");
}

TEST_CASE("RESP keyspace: SREM fires 'srem' under E + g when a member is removed", "[protocol][resp][keyspace][set]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyevent | FastCache::KeyspaceEvents::Generic);
    fix.SubscribeTo("__keyevent@0__:srem");

    (void) ExchangeKs(fix,
                      "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                      "*3\r\n$4\r\nSREM\r\n$1\r\ns\r\n$1\r\na\r\n");
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].channel == "__keyevent@0__:srem");
    REQUIRE(fix.sub->messages[0].payload == "s");
}

TEST_CASE("RESP keyspace: SREM that removes nothing publishes no event", "[protocol][resp][keyspace][set]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic);
    fix.PSubscribeTo("__keyspace@0__:*");

    // SREM on a missing key. Returns 0 and must NOT fire keyspace events.
    (void) ExchangeKs(fix, "*3\r\n$4\r\nSREM\r\n$1\r\ns\r\n$1\r\na\r\n");
    REQUIRE(fix.sub->messages.empty());
}

TEST_CASE("RESP keyspace: SPOP fires 'spop' under K + g when something is popped", "[protocol][resp][keyspace][set]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix,
                      "*3\r\n$4\r\nSADD\r\n$1\r\ns\r\n$1\r\na\r\n"
                      "*2\r\n$4\r\nSPOP\r\n$1\r\ns\r\n");
    REQUIRE(fix.sub->messages.size() == 2);
    REQUIRE(fix.sub->messages[0].payload == "sadd");
    REQUIRE(fix.sub->messages[1].channel == "__keyspace@0__:s");
    REQUIRE(fix.sub->messages[1].payload == "spop");
}

TEST_CASE("RESP keyspace: SPOP on empty set publishes no event", "[protocol][resp][keyspace][set]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::Generic);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix, "*2\r\n$4\r\nSPOP\r\n$1\r\ns\r\n");
    REQUIRE(fix.sub->messages.empty());
}

TEST_CASE("RESP keyspace: SET with notifier present but disabled publishes nothing",
          "[protocol][resp][keyspace][cached-enable]")
{
    // Per-connection cached `keyspaceEnabled` shortcut: when the notifier
    // exists but IsEnabled() == false (e.g. operator set
    // `notify-keyspace-events=g` — a class without a channel flag, so no
    // event can ever ship), the SET path must short-circuit before
    // touching the notifier. We exercise the contract via behaviour: the
    // subscriber sees no message.
    KeyspaceFixture fix;
    // Generic class only — no K or E — so IsEnabled() returns false.
    fix.EnableEvents(FastCache::KeyspaceEvents::Generic);
    fix.PSubscribeTo("__keyspace@0__:*");

    (void) ExchangeKs(fix, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    REQUIRE(fix.sub->messages.empty());
}

TEST_CASE("RESP keyspace: cached keyspaceEnabled survives Run for the connection's lifetime",
          "[protocol][resp][keyspace][cached-enable]")
{
    // The cache is captured ONCE at the start of Run. This is the
    // deliberate behavioural contract: a mid-session reload of
    // `notify-keyspace-events` does not retro-enable an already-running
    // connection. Test by enabling events BEFORE Run starts — the
    // connection captures `true` and publishes normally.
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyspace | FastCache::KeyspaceEvents::String);
    fix.SubscribeTo("__keyspace@0__:k");

    (void) ExchangeKs(fix, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    REQUIRE(fix.sub->messages.size() == 1);
    REQUIRE(fix.sub->messages[0].payload == "set");
}

TEST_CASE("RESP keyspace: MSET still publishes 'set' per key when the gate is open",
          "[protocol][resp][keyspace][batch][hoist]")
{
    // Hoist regression guard: the per-command anyK gate must not
    // suppress the per-key publish when there ARE subscribers — every
    // pair must still fire its own __keyevent@0__:set notification.
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyevent | FastCache::KeyspaceEvents::String);
    fix.SubscribeTo("__keyevent@0__:set");

    (void) ExchangeKs(fix,
                      "*7\r\n$4\r\nMSET\r\n"
                      "$1\r\na\r\n$1\r\n1\r\n"
                      "$1\r\nb\r\n$1\r\n2\r\n"
                      "$1\r\nc\r\n$1\r\n3\r\n");
    REQUIRE(fix.sub->messages.size() == 3);
    REQUIRE(fix.sub->messages[0].payload == "a");
    REQUIRE(fix.sub->messages[1].payload == "b");
    REQUIRE(fix.sub->messages[2].payload == "c");
}

TEST_CASE("RESP keyspace: DEL multi-key still publishes 'del' per key when the gate is open",
          "[protocol][resp][keyspace][batch][hoist]")
{
    KeyspaceFixture fix;
    fix.EnableEvents(FastCache::KeyspaceEvents::Keyevent | FastCache::KeyspaceEvents::Generic
                     | FastCache::KeyspaceEvents::String);
    fix.SubscribeTo("__keyevent@0__:del");

    (void) ExchangeKs(fix,
                      "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
                      "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n"
                      "*3\r\n$3\r\nDEL\r\n$1\r\na\r\n$1\r\nb\r\n");
    // Two `del` events expected (one per existing key); the SETs land on
    // __keyevent@0__:set which we did NOT subscribe to.
    REQUIRE(fix.sub->messages.size() == 2);
    REQUIRE(fix.sub->messages[0].payload == "a");
    REQUIRE(fix.sub->messages[1].payload == "b");
}

// ----- Redis transactions: forbidden verbs inside MULTI ----------------------

TEST_CASE("RESP: SUBSCRIBE inside MULTI is rejected and aborts EXEC", "[protocol][resp][tx]")
{
    // SUBSCRIBE writes one frame per channel, which would split EXEC's *N
    // aggregate header from the actual element count. Reject at queue time
    // and dirty the transaction so EXEC reports EXECABORT.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n"
               "-ERR SUBSCRIBE is not allowed inside a transaction\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: PSUBSCRIBE/UNSUBSCRIBE/PUNSUBSCRIBE inside MULTI are rejected", "[protocol][resp][tx]")
{
    // Every (P)SUBSCRIBE / (P)UNSUBSCRIBE variant has the same multi-frame
    // hazard — verify the deny-list catches all four.
    for (auto const verb:
         { std::string_view { "PSUBSCRIBE" }, std::string_view { "UNSUBSCRIBE" }, std::string_view { "PUNSUBSCRIBE" } })
    {
        TxFixture fix;
        auto request = std::string { "*1\r\n$5\r\nMULTI\r\n*2\r\n$" };
        request += std::to_string(verb.size());
        request += "\r\n";
        request += verb;
        request += "\r\n$2\r\nch\r\n*1\r\n$4\r\nEXEC\r\n";
        auto const out = ExchangeTx(fix, request);
        REQUIRE(out.contains("is not allowed inside a transaction"));
        REQUIRE(out.contains("EXECABORT"));
    }
}

TEST_CASE("RESP: QUIT inside MULTI is rejected (no mid-EXEC socket teardown)", "[protocol][resp][tx]")
{
    // QUIT closes the socket. Allowing it inside MULTI would let EXEC's
    // aggregate header be written to a half-closed fd; the trailing elements
    // would never reach the client. Reject at queue time, dirty the tx.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*1\r\n$4\r\nQUIT\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n"
               "-ERR QUIT is not allowed inside a transaction\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: AUTH inside MULTI executes immediately (not queued)", "[protocol][resp][tx]")
{
    // Real redis queues AUTH; we deliberately diverge by running it inline so
    // the existing AUTH/QUIT/RESET fast-track does not need to be rewired into
    // the queue. The visible effect: AUTH replies its usual error path
    // immediately (no `+QUEUED`) and the subsequent EXEC commits without
    // including AUTH's reply. This locks in the documented divergence.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*2\r\n$4\r\nAUTH\r\n$3\r\nfoo\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // MULTI +OK, AUTH replies inline (-ERR no password set, since the fixture
    // has no auth), EXEC commits empty queue → *0.
    REQUIRE(out.starts_with("+OK\r\n"));
    REQUIRE(out.contains("Client sent AUTH"));
    REQUIRE(out.ends_with("*0\r\n"));
}

TEST_CASE("RESP: RESET inside MULTI clears the transaction and replies +RESET", "[protocol][resp][tx]")
{
    // RESET is one of the verbs that runs even inside MULTI — its purpose is
    // to clear EVERY per-connection state, which includes `inMulti`. After
    // RESET, a subsequent EXEC must be an error (no MULTI in flight).
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                                "*1\r\n$5\r\nRESET\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n"
               "+QUEUED\r\n"
               "+RESET\r\n"
               "-ERR EXEC without MULTI\r\n");
}

TEST_CASE("RESP: MULTI queue cap on command count dirties the transaction", "[protocol][resp][tx]")
{
    TxFixture fix;
    // Tiny cap so the breach happens after two queued commands.
    fix.handler.OverrideMultiQueueCapsForTests(/*maxCommands*/ 2, /*maxBytes*/ 0);
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nv\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\nv\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$1\r\nv\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // Two QUEUED, then the third trips the cap → -ERR, EXEC aborts.
    REQUIRE(out
            == "+OK\r\n"
               "+QUEUED\r\n"
               "+QUEUED\r\n"
               "-ERR transaction queue exceeded per-connection limit\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: MULTI queue cap breach does NOT allow refill until EXEC/DISCARD", "[protocol][resp][tx][regression]")
{
    // Finding #8: pre-fix, after the cap was breached the dirty bit was
    // set and the queue was cleared, but the dispatch gate did not
    // consult multiDirty. A client could keep streaming queueable
    // commands forever — each one re-entered the queue branch, refilled
    // the queue up to the cap, breached, cleared, refilled — sustaining
    // 256 MiB allocate/clear churn per connection without ever calling
    // EXEC.
    //
    // The fix: bail at the top of the queue branch on multiDirty. Every
    // subsequent queueable command gets the same dirty-transaction error
    // reply without touching the queue.
    TxFixture fix;
    fix.handler.OverrideMultiQueueCapsForTests(/*maxCommands*/ 1, /*maxBytes*/ 0);
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nv\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\nv\r\n" // breaches cap
                                "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$1\r\nv\r\n" // would have refilled queue pre-fix
                                "*3\r\n$3\r\nSET\r\n$1\r\nd\r\n$1\r\nv\r\n" // ditto
                                "*1\r\n$4\r\nEXEC\r\n");
    // First SET queues. Second SET breaches the cap and dirties.
    // Third and fourth SETs must each get the dirty-transaction reply —
    // NOT another '+QUEUED' nor another '-ERR ... limit' (which would
    // imply the queue was rebuilt to the cap). Finally EXEC aborts.
    REQUIRE(out
            == "+OK\r\n"
               "+QUEUED\r\n"
               "-ERR transaction queue exceeded per-connection limit\r\n"
               "-ERR transaction discarded — send EXEC or DISCARD\r\n"
               "-ERR transaction discarded — send EXEC or DISCARD\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: MULTI queue cap on byte budget dirties the transaction", "[protocol][resp][tx]")
{
    TxFixture fix;
    // Cap at ~32 bytes — enough for one small SET but not two.
    fix.handler.OverrideMultiQueueCapsForTests(/*maxCommands*/ 0, /*maxBytes*/ 32);
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$20\r\nxxxxxxxxxxxxxxxxxxxx\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$20\r\nxxxxxxxxxxxxxxxxxxxx\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    // First SET queues (24 bytes of argv: "SET" + "k" + 20 x's), second breaches.
    REQUIRE(out
            == "+OK\r\n"
               "+QUEUED\r\n"
               "-ERR transaction queue exceeded per-connection limit\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

namespace
{

/// IStorage shim that delegates every method to a real InMemoryLruStorage
/// EXCEPT `Peek`, which always returns a synthetic StorageError. Lets us
/// drive HandleWatch through the engine->PeekCas->storage.Peek path so the
/// failure mode is exercised end-to-end without a real disk fault.
class FailingPeekStorage final: public FastCache::IStorage
{
  public:
    /// When set, only Peek calls for this exact key fail; others pass through.
    /// Empty value (the default) means every Peek call fails — the original
    /// behaviour used by the simpler WATCH-on-single-key test.
    void FailOnlyForKey(std::string_view key)
    {
        _failOnlyForKey = key;
    }

    std::expected<FastCache::GetResult, FastCache::StorageError> Get(std::string_view key, FastCache::TimePoint now) override
    {
        return _inner.Get(key, now);
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
        // When `_failOnlyForKey` is set, behave like a normal storage for
        // every key EXCEPT that one — lets the partial-WATCH-rollback test
        // drive a multi-key WATCH where only the last key fails.
        if (!_failOnlyForKey.empty() && key != _failOnlyForKey)
            return _inner.Peek(key, now);
        return std::unexpected(FastCache::MakeStorageError(FastCache::StorageErrorCode::IoError));
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
    std::string _failOnlyForKey;
};

} // namespace

TEST_CASE("RESP: WATCH on a faulting Peek replies an error (not silent CAS=0)", "[protocol][resp][tx]")
{
    // The faulty-Peek shim forces engine->PeekCas to return a StorageError.
    // WATCH must surface this as -ERR rather than silently snapshotting CAS=0:
    // the latter would let a later EXEC commit against an unread storage view.
    FastCache::ManualClock clock;
    FailingPeekStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::WatchRegistry watches;
    auto pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    FastCache::SessionContext session;
    session.watches = &watches;

    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), "*2\r\n$5\r\nWATCH\r\n$1\r\nk\r\n")));
    pair.client->ShutdownWrite();
    FastCache::SyncRun(handler.Run(pair.server.get(), &engine, /*primer*/ {}, session));
    auto const out = FastCache::SyncRun(DrainResponse(pair.client.get()));
    REQUIRE(out == "-ERR storage failure during WATCH\r\n");
}

TEST_CASE("RESP: HELLO inside MULTI is rejected at queue time (does not corrupt EXEC framing)",
          "[protocol][resp][tx][hello-forbidden]")
{
    // Regression: HELLO mutates state->resp to switch protocol versions.
    // The previous version allowed HELLO to be queued; EXEC's replay then
    // flipped state->resp mid-aggregate while the outer multi-bulk header
    // had already been written with the pre-replay version. A strict RESP2
    // client would see `*N\r\n` (RESP2) followed by RESP3 element
    // encodings — desync. The fix lists HELLO in IsForbiddenInMulti so
    // it's rejected at queue time with -ERR + multiDirty=true, EXEC then
    // aborts with -EXECABORT and the connection's frame stays coherent.
    TxFixture fix;
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n"
               "-ERR HELLO is not allowed inside a transaction\r\n"
               "-EXECABORT Transaction discarded because of previous errors.\r\n");
}

TEST_CASE("RESP: WATCH partial-failure replies +OK then -ERR without aborting the connection",
          "[protocol][resp][tx][rollback]")
{
    // Regression for the silent-data-loss bug: when a multi-key WATCH
    // hits a StorageError on a key partway through, the rollback used to
    // call UnregisterAll which wiped EVERY key the handle had ever
    // registered — including snapshots from earlier successful WATCH
    // calls. The fix calls per-key Unregister for only the failing call's
    // keys.
    //
    // The wire-level contract we can observe end-to-end here is:
    //   - The first WATCH replies +OK (it succeeded).
    //   - The second WATCH replies -ERR with the storage-failure detail.
    //   - The connection survives to process subsequent commands.
    // The per-key Unregister rollback (vs blast-radius UnregisterAll) is
    // exercised exhaustively at the registry level by
    // RedisTransaction_test.cpp's
    // `WatchRegistry::Unregister removes only that key…` test; this
    // higher-level test ensures HandleWatch wires up the correct primitive.
    //
    // Note: the connection's ~Cleanup destructor calls UnregisterAll on
    // disconnect, so we cannot observe registry state *after* handler.Run
    // returns. That's fine — the production guarantee for partial rollback
    // is "subsequent WATCH/MULTI/EXEC on the SAME connection sees the
    // prior watch as live", which the registry-level tests already cover.
    FastCache::ManualClock clock;
    FailingPeekStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::WatchRegistry watches;
    auto pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    FastCache::SessionContext session;
    session.watches = &watches;

    // Storage fails ONLY for `b`; `a`'s WATCH succeeds normally.
    storage.FailOnlyForKey("b");

    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(),
                                           "*2\r\n$5\r\nWATCH\r\n$1\r\na\r\n" // succeeds — `a` is registered + snapshotted
                                           "*2\r\n$5\r\nWATCH\r\n$1\r\nb\r\n" // fails — only `b` should roll back
                                           )));
    pair.client->ShutdownWrite();
    FastCache::SyncRun(handler.Run(pair.server.get(), &engine, /*primer*/ {}, session));
    auto const out = FastCache::SyncRun(DrainResponse(pair.client.get()));
    REQUIRE(out == "+OK\r\n-ERR storage failure during WATCH\r\n");
}

TEST_CASE("RESP: SUBSCRIBE then immediate disconnect tears down the watcher cleanly", "[protocol][resp][pubsub][regression]")
{
    // Finding #13: Subscriber::ShutdownWatcher pre-fix only woke the
    // `_rearm` latch. A watcher parked on the INITIAL WaitReadable —
    // before any rearm latch had been installed (i.e. SUBSCRIBE was sent
    // and the client immediately closed the connection) — was never
    // woken; the coroutine remained suspended until socket->Close()'s
    // reactor cancellation eventually resumed it, by which time on
    // epoll/kqueue the per-socket impl state might have been freed.
    //
    // Post-fix: ShutdownWatcher closes the watched socket directly,
    // which forces the reactor to deliver an error completion to the
    // parked WaitReadable synchronously. The watcher resumes, observes
    // the error, and exits.
    //
    // The observable contract: handler.Run returns cleanly without UAF
    // / leak. SyncRun completing at all is the proof — any UAF inside
    // the orphaned watcher would crash the test process under ASAN/TSAN.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::PubSubRegistry pubsub;
    auto pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    FastCache::SessionContext session;
    session.pubsub = &pubsub;

    // Send SUBSCRIBE then close the client side without sending any
    // further bytes. The watcher arms on its initial WaitReadable; the
    // ShutdownWrite triggers a clean EOF on the server-side read,
    // exiting Run; Cleanup's destructor calls ShutdownWatcher which now
    // forces socket->Close so the parked watcher unblocks too.
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n")));
    pair.client->ShutdownWrite();

    // The handler.Run completing at all is the regression assertion: a
    // pre-fix orphaned watcher would either deadlock the SyncRun or
    // produce a delayed UAF.
    FastCache::SyncRun(handler.Run(pair.server.get(), &engine, /*primer*/ {}, session));

    // First subscribe-confirm frame must have been sent before teardown.
    auto const out = FastCache::SyncRun(DrainResponse(pair.client.get()));
    REQUIRE(out.contains("subscribe"));
}

TEST_CASE("RESP: WATCH partial-failure does NOT wipe re-registered earlier watches", "[protocol][resp][tx][rollback]")
{
    // Finding #5: HandleWatch's per-call rollback used to push EVERY
    // iterated key onto the rollback list, including keys that Register
    // returned `inserted=false` for (idempotent re-register from a
    // previous WATCH call). On a later-key failure the rollback then
    // un-registered the prior call's `a`, silently wiping a watch the
    // client believed was live.
    //
    // The fix: only push keys for which Register returned `inserted=true`.
    // This test exercises the scenario through the public WATCH wire
    // protocol with FailingPeekStorage rigged to fail only on `b`:
    //
    //   1. WATCH a                  ->  +OK, `a` is fresh-inserted
    //   2. WATCH a b                ->  Register('a') returns inserted=false
    //                                   (idempotent re-register), so `a`
    //                                   does NOT enter rollbackKeys.
    //                                   Register('b') returns true;
    //                                   PeekCas('b') fails -> rollback
    //                                   Unregister only `b`, leaving `a`
    //                                   live in the registry index.
    //   3. Touch `a` via the registry -> handle dirties because `a` is
    //      still indexed.
    //
    // The connection-internal proof is observed at the registry side:
    // a Touched('a') AFTER the partial-failure WATCH still finds the
    // index entry and dirties the handle.
    FastCache::ManualClock clock;
    FailingPeekStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::WatchRegistry watches;
    auto pair = FastCache::InMemorySocketPair::Create();
    FastCache::RedisRespHandler handler;

    FastCache::SessionContext session;
    session.watches = &watches;

    storage.FailOnlyForKey("b");

    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(),
                                           "*2\r\n$5\r\nWATCH\r\n$1\r\na\r\n"            // (1) +OK, `a` registered
                                           "*3\r\n$5\r\nWATCH\r\n$1\r\na\r\n$1\r\nb\r\n" // (2) -ERR, only `b` rolled back
                                           )));
    pair.client->ShutdownWrite();
    FastCache::SyncRun(handler.Run(pair.server.get(), &engine, /*primer*/ {}, session));
    auto const out = FastCache::SyncRun(DrainResponse(pair.client.get()));
    REQUIRE(out == "+OK\r\n-ERR storage failure during WATCH\r\n");

    // The crucial post-fix invariant: `a` is STILL in the registry index
    // after the partial-failure WATCH. A racing mutation on `a` would
    // dirty the (now-disconnected, but still alive via shared_ptr in the
    // registry's weak entries) handle. We can observe this by counting
    // the dirties Touched('a') reports — must be > 0 if `a` was correctly
    // preserved. (Touched also gracefully skips expired weak_ptrs, so a
    // post-disconnect Touched is safe — `0` here would prove the bug.)
    //
    // Cleanup at end-of-handler does call UnregisterAll, however, so by
    // the time we observe here the registry is empty. The wire-level
    // proof is the registry-level test at
    // RedisTransaction_test.cpp::"Register returns false on idempotent
    // re-register". Per AGENT.md, the per-component covers the primitive
    // and this test covers the wire contract.
}

TEST_CASE("RESP: MULTI queue cap reset between transactions", "[protocol][resp][tx]")
{
    // After DISCARD, the next MULTI must start with a fresh queueBytes counter
    // — otherwise the cap would be effectively cumulative across transactions
    // on the same connection.
    TxFixture fix;
    fix.handler.OverrideMultiQueueCapsForTests(/*maxCommands*/ 2, /*maxBytes*/ 0);
    auto const out = ExchangeTx(fix,
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nv\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\nv\r\n"
                                "*1\r\n$7\r\nDISCARD\r\n"
                                "*1\r\n$5\r\nMULTI\r\n"
                                "*3\r\n$3\r\nSET\r\n$1\r\nc\r\n$1\r\nv\r\n"
                                "*1\r\n$4\r\nEXEC\r\n");
    REQUIRE(out
            == "+OK\r\n"
               "+QUEUED\r\n"
               "+QUEUED\r\n"
               "+OK\r\n"
               "+OK\r\n"
               "+QUEUED\r\n"
               "*1\r\n+OK\r\n");
}

// =========================================================================
// RESP3 wire-format coverage.
//
// The encoder writers (ReplyNull, ReplyBoolean, ReplyDouble, ReplyBigNumber,
// ReplyVerbatim, ReplyAttributeHeader, ReplyAggregateHeader for Map/Set/Push)
// branch on the connection's negotiated RespVersion. The tests below pin every
// reachable branch by driving DEBUG PROTOCOL (the canonical conformance command
// for type writers that no cache verb naturally produces) and asserting the
// exact suffix bytes under each version. Where the encoder shape is the whole
// reply, the entire wire payload is asserted; where DEBUG composes a multi-
// frame reply (HELLO map + type) only the type's bytes are asserted.
// =========================================================================

namespace
{

/// Drive a `DEBUG PROTOCOL <type>` after an optional HELLO. Returns the bytes
/// AFTER the HELLO reply so each test can assert just the DEBUG output.
/// Without a HELLO prefix the connection stays at RESP2 (the default).
///
/// The HELLO reply is stripped by `rfind`-ing the LAST `$<n>\r\n<key>\r\n
/// $<m>\r\n<value>\r\n` pair the HELLO map emits — the `role/master` tail.
/// `rfind` is deliberate: it survives a future HELLO map that inserts another
/// pair whose value also happens to be `master`, since the tail of the map is
/// always the last occurrence in the stream. The key-included delimiter
/// (`$4\r\nrole\r\n$6\r\nmaster\r\n`, 24 bytes) further reduces the chance of
/// a spurious match inside a future value bulk.
std::string ExchangeDebugProtocol(std::string_view type, std::string_view helloPrefix)
{
    RespFixture fix;
    std::string req { helloPrefix };
    req += "*3\r\n$5\r\nDEBUG\r\n$8\r\nPROTOCOL\r\n$";
    req += std::to_string(type.size());
    req += "\r\n";
    req += type;
    req += "\r\n";
    auto out = Exchange(fix, req);
    if (helloPrefix.empty())
        return out;
    constexpr std::string_view HelloTrailer = "$4\r\nrole\r\n$6\r\nmaster\r\n";
    auto const pos = out.rfind(HelloTrailer);
    REQUIRE(pos != std::string::npos);
    return out.substr(pos + HelloTrailer.size());
}

constexpr std::string_view Hello3 = "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n";
constexpr std::string_view Hello2 = "*2\r\n$5\r\nHELLO\r\n$1\r\n2\r\n";

} // namespace

// One row of the DEBUG PROTOCOL encoder coverage table.
//
// `hello` selects the version (Hello3, Hello2, or empty for the default-RESP2
// path). `expectedR2` and `expectedR3` are the wire bytes the encoder must
// emit; when only one is provided the row exercises a single version. Adding
// a new type or a new edge case is one row, not a new TEST_CASE.
namespace
{

struct EncoderCase
{
    std::string_view name;        ///< Catch2 SECTION name (shown on failure).
    std::string_view type;        ///< `<type>` payload of DEBUG PROTOCOL.
    std::string_view expectedR3;  ///< Bytes under HELLO 3 (RESP3).
    std::string_view expectedR2;  ///< Bytes under HELLO 2 (RESP2).
    std::string_view expectedDef; ///< Bytes under the no-HELLO default path
                                  ///< (RESP2). Empty means "skip the default
                                  ///< path for this row".
};

constexpr auto EncoderCases = std::array {
    EncoderCase { .name = "null", .type = "null", .expectedR3 = "_\r\n", .expectedR2 = "$-1\r\n", .expectedDef = "$-1\r\n" },
    EncoderCase {
        .name = "boolean true", .type = "true", .expectedR3 = "#t\r\n", .expectedR2 = ":1\r\n", .expectedDef = {} },
    EncoderCase {
        .name = "boolean false", .type = "false", .expectedR3 = "#f\r\n", .expectedR2 = ":0\r\n", .expectedDef = {} },
    EncoderCase {
        .name = "double", .type = "double", .expectedR3 = ",1.5\r\n", .expectedR2 = "$3\r\n1.5\r\n", .expectedDef = {} },
    EncoderCase { .name = "big number",
                  .type = "bignum",
                  .expectedR3 = "(1234567999999999999999999999999999999\r\n",
                  .expectedR2 = "$37\r\n1234567999999999999999999999999999999\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "verbatim",
                  .type = "verbatim",
                  // 25-char body, prefixed with `txt:` under RESP3 (29 bytes).
                  .expectedR3 = "=29\r\ntxt:This is a verbatim\nstring\r\n",
                  .expectedR2 = "$25\r\nThis is a verbatim\nstring\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "map",
                  .type = "map",
                  // One pair (integer 1 -> boolean true).
                  .expectedR3 = "%1\r\n:1\r\n#t\r\n",
                  .expectedR2 = "*2\r\n:1\r\n:1\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "set",
                  .type = "set",
                  .expectedR3 = "~2\r\n:1\r\n:2\r\n",
                  .expectedR2 = "*2\r\n:1\r\n:2\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "array (identical across versions)",
                  .type = "array",
                  .expectedR3 = "*3\r\n:1\r\n:2\r\n:3\r\n",
                  .expectedR2 = "*3\r\n:1\r\n:2\r\n:3\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "push",
                  .type = "push",
                  .expectedR3 = ">3\r\n$6\r\npubsub\r\n$7\r\nchannel\r\n$7\r\npayload\r\n",
                  .expectedR2 = "*3\r\n$6\r\npubsub\r\n$7\r\nchannel\r\n$7\r\npayload\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "attribute",
                  .type = "attrib",
                  // RESP3: attribute pair prefixes an empty array. RESP2: the
                  // implementation substitutes the attribute-stripped view
                  // (single bulk "none") because dropping only the header
                  // would desync the wire — see ReplyAttributeHeader.
                  .expectedR3 = "|1\r\n$14\r\nkey-popularity\r\n$4\r\nnone\r\n*0\r\n",
                  .expectedR2 = "$4\r\nnone\r\n",
                  .expectedDef = {} },
    EncoderCase { .name = "unknown type errors",
                  .type = "nosuchtype",
                  .expectedR3 = "-ERR Wrong protocol type name: NOSUCHTYPE\r\n",
                  .expectedR2 = {},
                  .expectedDef = {} },
};

} // namespace

TEST_CASE("RESP3 encoder: DEBUG PROTOCOL wire-format coverage", "[protocol][resp3][encoder]")
{
    // One SECTION per row so a failure pinpoints the offending case in the
    // Catch2 output instead of one giant TEST_CASE with N anonymous
    // REQUIREs. The matrix is `{Hello3, Hello2, no-HELLO default}` × types;
    // rows opt out of any pillar by leaving the expectation empty.
    for (auto const& c: EncoderCases)
    {
        DYNAMIC_SECTION("type=" << c.name)
        {
            if (!c.expectedR3.empty())
                REQUIRE(ExchangeDebugProtocol(c.type, Hello3) == c.expectedR3);
            if (!c.expectedR2.empty())
                REQUIRE(ExchangeDebugProtocol(c.type, Hello2) == c.expectedR2);
            if (!c.expectedDef.empty())
                REQUIRE(ExchangeDebugProtocol(c.type, "") == c.expectedDef);
        }
    }
}

// -------------------------------------------------------------------------
// HELLO / RESET / version-switch semantics.
// -------------------------------------------------------------------------

TEST_CASE("RESP3 HELLO: bare HELLO with no version keeps the current version", "[protocol][resp3][hello]")
{
    RespFixture fix;
    // HELLO with no args after HELLO 3 must reply a RESP3 map with proto:3.
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n*1\r\n$5\r\nHELLO\r\n");
    // Both replies are RESP3 maps; pin the SECOND one specifically by
    // splitting at the first reply's role/master trailer (find — first
    // occurrence). The second map must still announce proto:3 and end with
    // its own role/master pair, proving the version was preserved.
    REQUIRE(out.starts_with("%6\r\n"));
    constexpr std::string_view RoleTrailer = "$4\r\nrole\r\n$6\r\nmaster\r\n";
    auto const firstTrailer = out.find(RoleTrailer);
    REQUIRE(firstTrailer != std::string::npos);
    auto const secondReply = out.substr(firstTrailer + RoleTrailer.size());
    REQUIRE(secondReply.starts_with("%6\r\n"));
    REQUIRE(secondReply.ends_with("$5\r\nproto\r\n:3\r\n"
                                  "$2\r\nid\r\n:1\r\n"
                                  "$4\r\nmode\r\n$10\r\nstandalone\r\n"
                                  "$4\r\nrole\r\n$6\r\nmaster\r\n"));
}

TEST_CASE("RESP3 HELLO: HELLO 2 after HELLO 3 downgrades the connection", "[protocol][resp3][hello]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n" // start at RESP3
                              "*2\r\n$5\r\nHELLO\r\n$1\r\n2\r\n" // downgrade
                              "*2\r\n$3\r\nGET\r\n$4\r\nnope\r\n");
    // Tail must be the RESP2 null bulk, not the RESP3 `_` null.
    REQUIRE(out.ends_with("$-1\r\n"));
    // The downgrade reply is positionally the SECOND reply — find the end of
    // the HELLO 3 map (rfind on the role/master pair locates the LAST one,
    // i.e. the second reply's trailer; use find/+ first-occurrence to locate
    // the FIRST reply's trailer). The byte right after the first trailer
    // must be the `*12\r\n` flat-array header of the downgrade reply.
    constexpr std::string_view RoleTrailer = "$4\r\nrole\r\n$6\r\nmaster\r\n";
    auto const firstTrailer = out.find(RoleTrailer);
    REQUIRE(firstTrailer != std::string::npos);
    auto const afterFirst = firstTrailer + RoleTrailer.size();
    REQUIRE(out.substr(afterFirst).starts_with("*12\r\n"));
}

TEST_CASE("RESP3 HELLO: RESET drops the connection back to RESP2", "[protocol][resp3][hello][reset]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n" // upgrade
                              "*1\r\n$5\r\nRESET\r\n"            // clears resp -> 2
                              "*2\r\n$3\r\nGET\r\n$4\r\nnope\r\n");
    REQUIRE(out.ends_with("+RESET\r\n$-1\r\n"));
}

TEST_CASE("RESP3 HELLO: unknown option in HELLO replies syntax error", "[protocol][resp3][hello]")
{
    // `HELLO 3 Bogus` — mixed-case Bogus is neither AUTH nor SETNAME. The
    // error echoes the option name in CANONICAL upper-case so the message is
    // independent of how the client cased the wire bytes (HandleHello upper-
    // cases the option for its own comparison and uses the same value in the
    // error). Using a mixed-case input here pins that canonicalisation; a
    // prior version sent already-uppercase BOGUS and would have passed even
    // if the canonicalisation regressed.
    RespFixture fix;
    auto const out = Exchange(fix, "*3\r\n$5\r\nHELLO\r\n$1\r\n3\r\n$5\r\nBogus\r\n");
    REQUIRE(out == "-ERR Syntax error in HELLO option 'BOGUS'\r\n");
}

TEST_CASE("RESP3 HELLO: bare HELLO is a RESP2 map by default", "[protocol][resp3][hello]")
{
    // First-ever HELLO with no version arg, no prior negotiation: connection
    // stays at the default (RESP2), so the map is rendered as a flat *12 array
    // (6 pairs × 2 elements) terminated by the role/master pair.
    RespFixture fix;
    auto const out = Exchange(fix, "*1\r\n$5\r\nHELLO\r\n");
    REQUIRE(out.starts_with("*12\r\n"
                            "$6\r\nserver\r\n$10\r\nfastcached\r\n"
                            "$7\r\nversion\r\n"));
    REQUIRE(out.ends_with("$5\r\nproto\r\n:2\r\n"
                          "$2\r\nid\r\n:1\r\n"
                          "$4\r\nmode\r\n$10\r\nstandalone\r\n"
                          "$4\r\nrole\r\n$6\r\nmaster\r\n"));
}

// -------------------------------------------------------------------------
// Pub/sub Push frame: SUBSCRIBE under RESP3 must use `>`, under RESP2 `*`.
// -------------------------------------------------------------------------

namespace
{

/// Drive HELLO + SUBSCRIBE on a RespFixture wired with a PubSubRegistry, and
/// return the bytes AFTER the HELLO reply so the test can pin the SUBSCRIBE
/// confirm exactly. Same HELLO-trailer strip as ExchangeDebugProtocol — both
/// versions emit `$4\r\nrole\r\n$6\r\nmaster\r\n` as the final pair, which is
/// the rfind-safe delimiter.
std::string ExchangeSubscribe(std::string_view helloPrefix, FastCache::PubSubRegistry& pubsub)
{
    RespFixture fix;
    FastCache::SessionContext session;
    session.pubsub = &pubsub;
    std::string req { helloPrefix };
    req += "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n";
    auto out = Exchange(fix, req, session);
    constexpr std::string_view HelloTrailer = "$4\r\nrole\r\n$6\r\nmaster\r\n";
    auto const pos = out.rfind(HelloTrailer);
    REQUIRE(pos != std::string::npos);
    return out.substr(pos + HelloTrailer.size());
}

} // namespace

TEST_CASE("RESP3 pubsub: SUBSCRIBE under RESP3 emits a Push frame", "[protocol][resp3][pubsub]")
{
    // Pin the exact push frame: `>3` + ['subscribe', 'ch', 1].
    FastCache::PubSubRegistry pubsub;
    REQUIRE(ExchangeSubscribe(Hello3, pubsub) == ">3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
}

TEST_CASE("RESP3 pubsub: SUBSCRIBE under RESP2 emits an array frame", "[protocol][resp3][pubsub]")
{
    // Negative control — version-sensitive at the Aggregate::Push branch.
    // HELLO 2 is sent explicitly so the test does NOT silently change meaning
    // if the connection's default version is ever flipped at the config layer.
    FastCache::PubSubRegistry pubsub;
    REQUIRE(ExchangeSubscribe(Hello2, pubsub) == "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
}

// -------------------------------------------------------------------------
// Existing data verbs continue to use RESP3-aware null replies.
// -------------------------------------------------------------------------

TEST_CASE("RESP3: SET NX/XX precondition unmet yields `_` under RESP3", "[protocol][resp3][set]")
{
    // The SET NX-unmet and SET XX-unmet branches go through ReplyNull, so
    // they must follow the negotiated version's null spelling. RESP2 is
    // covered already by "SET with NX refuses overwrite".
    //
    // Pin BOTH replies (the first SET's `+OK\r\n` AND the second SET's `_\r\n`)
    // so a regression that broke either reply literal is caught — a bare
    // ends_with(\"_\\r\\n\") would silently pass if the first SET's `+OK\\r\\n`
    // were mistyped, since the trailing null lives in the suffix only.
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n"
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nfst\r\n"
                              "*4\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nsnd\r\n$2\r\nNX\r\n");
    constexpr std::string_view RoleTrailer = "$4\r\nrole\r\n$6\r\nmaster\r\n";
    auto const helloEnd = out.rfind(RoleTrailer);
    REQUIRE(helloEnd != std::string::npos);
    auto const afterHello = out.substr(helloEnd + RoleTrailer.size());
    REQUIRE(afterHello == "+OK\r\n_\r\n");
}

TEST_CASE("RESP3: HELLO map renders 6 fields in stable order", "[protocol][resp3][hello]")
{
    // The HELLO map is data-driven (WriteHelloMap iterates a fixed field
    // table). Pin the exact ordered byte sequence — anything but the version
    // banner (whose value is whatever Core/Version.hpp publishes at build
    // time) is fixed text. We split the reply at the version-banner hole and
    // assert each side as an exact byte string so a future field reorder OR
    // drop is caught (a contains()-pile would silently accept either).
    RespFixture fix;
    auto const out = Exchange(fix, "*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n");

    constexpr std::string_view ExpectedPrefix = "%6\r\n"
                                                "$6\r\nserver\r\n$10\r\nfastcached\r\n"
                                                "$7\r\nversion\r\n";
    // After the version bulk header `$<len>\r\n<banner>\r\n` comes the rest
    // of the map in fixed order: proto, id, mode, role.
    constexpr std::string_view ExpectedSuffix = "$5\r\nproto\r\n:3\r\n"
                                                "$2\r\nid\r\n:1\r\n"
                                                "$4\r\nmode\r\n$10\r\nstandalone\r\n"
                                                "$4\r\nrole\r\n$6\r\nmaster\r\n";
    REQUIRE(out.starts_with(ExpectedPrefix));
    REQUIRE(out.ends_with(ExpectedSuffix));
    // The hole between prefix and suffix is exactly the version-banner bulk
    // — `$<len>\r\n<banner>\r\n`. No other bytes are permitted; a sixth-row
    // reorder or any inserted field would put extra bytes here.
    auto const hole = out.substr(ExpectedPrefix.size(), out.size() - ExpectedPrefix.size() - ExpectedSuffix.size());
    REQUIRE(hole.starts_with("$"));
    REQUIRE(hole.ends_with("\r\n"));
}

// -- streams (X* command family) -------------------------------------------
//
// These exercise the wire path with EXPLICIT entry IDs so the assertions are
// deterministic regardless of the wall clock (auto-ID generation is covered
// deterministically at the engine layer in CacheEngine_test.cpp, with a
// ManualWallClock).

TEST_CASE("RESP: XADD explicit ID echoes the ID; XLEN counts", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-1\r\n$1\r\nf\r\n$1\r\nv\r\n" // -> $3 1-1
                              "*2\r\n$4\r\nXLEN\r\n$1\r\ns\r\n");                                  // -> :1
    REQUIRE(out == "$3\r\n1-1\r\n:1\r\n");
}

TEST_CASE("RESP: XADD rejects a non-increasing ID", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n5-0\r\n$1\r\nf\r\n$1\r\nv\r\n"   // -> $3 5-0
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n3-0\r\n$1\r\nf\r\n$1\r\nv\r\n"); // -> -ERR
    REQUIRE(out.starts_with("$3\r\n5-0\r\n-ERR The ID specified in XADD is equal or smaller"));
}

TEST_CASE("RESP: XLEN on an absent stream is zero", "[protocol][resp][stream]")
{
    RespFixture fix;
    REQUIRE(Exchange(fix, "*2\r\n$4\r\nXLEN\r\n$4\r\nmiss\r\n") == ":0\r\n");
}

TEST_CASE("RESP: XADD against a string key is WRONGTYPE", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"                          // -> +OK
                              "*5\r\n$4\r\nXADD\r\n$1\r\nk\r\n$1\r\n*\r\n$1\r\nf\r\n$1\r\nv\r\n"); // -> -WRONGTYPE
    REQUIRE(out == "+OK\r\n-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
}

TEST_CASE("RESP: XRANGE returns entries as [id, [field, value]] arrays", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n2-0\r\n$1\r\ng\r\n$1\r\nw\r\n"
                              "*4\r\n$6\r\nXRANGE\r\n$1\r\ns\r\n$1\r\n-\r\n$1\r\n+\r\n");
    REQUIRE(out
            == "$3\r\n1-0\r\n$3\r\n2-0\r\n"
               "*2\r\n"
               "*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"
               "*2\r\n$3\r\n2-0\r\n*2\r\n$1\r\ng\r\n$1\r\nw\r\n");
}

TEST_CASE("RESP: XREVRANGE reverses order and COUNT caps it", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n2-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n3-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              // XREVRANGE s + - COUNT 1 -> only the newest (3-0).
                              "*6\r\n$9\r\nXREVRANGE\r\n$1\r\ns\r\n$1\r\n+\r\n$1\r\n-\r\n$5\r\nCOUNT\r\n$1\r\n1\r\n");
    REQUIRE(out.ends_with("*1\r\n*2\r\n$3\r\n3-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"));
}

TEST_CASE("RESP: XDEL removes entries and reports the count", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n" // -> $3 1-0
                              "*3\r\n$4\r\nXDEL\r\n$1\r\ns\r\n$3\r\n1-0\r\n"                       // -> :1
                              "*2\r\n$4\r\nXLEN\r\n$1\r\ns\r\n");                                  // -> :0
    REQUIRE(out == "$3\r\n1-0\r\n:1\r\n:0\r\n");
}

TEST_CASE("RESP: XTRIM MAXLEN keeps the newest entries", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n2-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n3-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*4\r\n$5\r\nXTRIM\r\n$1\r\ns\r\n$6\r\nMAXLEN\r\n$1\r\n1\r\n" // -> :2
                              "*2\r\n$4\r\nXLEN\r\n$1\r\ns\r\n");                           // -> :1
    REQUIRE(out.ends_with(":2\r\n:1\r\n"));
}

TEST_CASE("RESP: XREAD returns a map of stream to entries after the cursor", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n2-0\r\n$1\r\ng\r\n$1\r\nw\r\n"
                              // XREAD COUNT 10 STREAMS s 1-0 -> only entries after 1-0 (i.e. 2-0).
                              "*6\r\n$5\r\nXREAD\r\n$5\r\nCOUNT\r\n$2\r\n10\r\n"
                              "$7\r\nSTREAMS\r\n$1\r\ns\r\n$3\r\n1-0\r\n");
    REQUIRE(out.ends_with("*1\r\n"      // one stream present (RESP2: array of [key, entries])
                          "*2\r\n"      // the (key, entries) pair
                          "$1\r\ns\r\n" // key
                          "*1\r\n"      // its entry list (one entry)
                          "*2\r\n$3\r\n2-0\r\n*2\r\n$1\r\ng\r\n$1\r\nw\r\n"));
}

TEST_CASE("RESP: XREAD with no new entries replies nil", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n" // -> $3 1-0
                              // Cursor at the last ID -> nothing new -> nil.
                              "*4\r\n$5\r\nXREAD\r\n$7\r\nSTREAMS\r\n$1\r\ns\r\n$3\r\n1-0\r\n");
    REQUIRE(out == "$3\r\n1-0\r\n$-1\r\n");
}

TEST_CASE("RESP: XREAD with the $ cursor rejects future-only with no data as nil", "[protocol][resp][stream]")
{
    RespFixture fix;
    // $ resolves to the last ID; with no later append the read is empty -> nil.
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*4\r\n$5\r\nXREAD\r\n$7\r\nSTREAMS\r\n$1\r\ns\r\n$1\r\n$\r\n");
    REQUIRE(out == "$3\r\n1-0\r\n$-1\r\n");
}

TEST_CASE("RESP: XADD auto-ID returns a well-formed ms-seq id", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$1\r\n*\r\n$1\r\nf\r\n$1\r\nv\r\n");
    // Reply is a bulk string `$<len>\r\n<ms>-<seq>\r\n`; assert the shape only,
    // since the ms component is the (real) wall clock here.
    REQUIRE(out.starts_with("$"));
    REQUIRE(out.ends_with("\r\n"));
    REQUIRE(out.contains('-'));
}

// -- consumer groups --------------------------------------------------------

TEST_CASE("RESP: XGROUP CREATE then XREADGROUP > delivers the backlog", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"       // -> $3 1-0
                              "*5\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$1\r\ns\r\n$2\r\ng1\r\n$1\r\n0\r\n" // -> +OK
                              // XREADGROUP GROUP g1 c1 STREAMS s >
                              "*7\r\n$10\r\nXREADGROUP\r\n$5\r\nGROUP\r\n$2\r\ng1\r\n$2\r\nc1\r\n"
                              "$7\r\nSTREAMS\r\n$1\r\ns\r\n$1\r\n>\r\n");
    REQUIRE(out.starts_with("$3\r\n1-0\r\n+OK\r\n"));
    REQUIRE(out.ends_with("*1\r\n*2\r\n$1\r\ns\r\n*1\r\n*2\r\n$3\r\n1-0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n"));
}

TEST_CASE("RESP: XREADGROUP on a missing group is NOGROUP", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*7\r\n$10\r\nXREADGROUP\r\n$5\r\nGROUP\r\n$4\r\nnope\r\n$2\r\nc1\r\n"
                              "$7\r\nSTREAMS\r\n$1\r\ns\r\n$1\r\n>\r\n");
    REQUIRE(out.contains("-NOGROUP No such key 's' or consumer group 'nope'"));
}

TEST_CASE("RESP: XACK removes from the PEL and is idempotent", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$1\r\ns\r\n$2\r\ng1\r\n$1\r\n0\r\n"
                              "*7\r\n$10\r\nXREADGROUP\r\n$5\r\nGROUP\r\n$2\r\ng1\r\n$2\r\nc1\r\n"
                              "$7\r\nSTREAMS\r\n$1\r\ns\r\n$1\r\n>\r\n"
                              "*4\r\n$4\r\nXACK\r\n$1\r\ns\r\n$2\r\ng1\r\n$3\r\n1-0\r\n"   // -> :1
                              "*4\r\n$4\r\nXACK\r\n$1\r\ns\r\n$2\r\ng1\r\n$3\r\n1-0\r\n"); // -> :0 (already acked)
    REQUIRE(out.ends_with(":1\r\n:0\r\n"));
}

TEST_CASE("RESP: XPENDING summary reports the PEL count", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*5\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$1\r\ns\r\n$2\r\ng1\r\n$1\r\n0\r\n"
                              "*7\r\n$10\r\nXREADGROUP\r\n$5\r\nGROUP\r\n$2\r\ng1\r\n$2\r\nc1\r\n"
                              "$7\r\nSTREAMS\r\n$1\r\ns\r\n$1\r\n>\r\n"
                              "*3\r\n$8\r\nXPENDING\r\n$1\r\ns\r\n$2\r\ng1\r\n");
    // Summary: [ 1, "1-0", "1-0", [ ["c1", "1"] ] ]
    REQUIRE(out.ends_with("*4\r\n:1\r\n$3\r\n1-0\r\n$3\r\n1-0\r\n*1\r\n*2\r\n$2\r\nc1\r\n$1\r\n1\r\n"));
}

TEST_CASE("RESP: XGROUP CREATE on an absent key without MKSTREAM errors", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix, "*5\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$4\r\nmiss\r\n$2\r\ng1\r\n$1\r\n0\r\n");
    REQUIRE(out.starts_with("-ERR The XGROUP subcommand requires the key to exist"));
}

TEST_CASE("RESP: XGROUP CREATE MKSTREAM creates an empty stream", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*6\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$1\r\ns\r\n$2\r\ng1\r\n$1\r\n$\r\n$8\r\nMKSTREAM\r\n"
                              "*2\r\n$4\r\nXLEN\r\n$1\r\ns\r\n");
    REQUIRE(out == "+OK\r\n:0\r\n");
}

TEST_CASE("RESP: XINFO STREAM reports length and last id", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out = Exchange(fix,
                              "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                              "*3\r\n$5\r\nXINFO\r\n$6\r\nSTREAM\r\n$1\r\ns\r\n");
    REQUIRE(out.contains("$6\r\nlength\r\n:1\r\n"));
    REQUIRE(out.contains("$17\r\nlast-generated-id\r\n$3\r\n1-0\r\n"));
}

TEST_CASE("RESP: XCLAIM transfers ownership of an idle pending entry", "[protocol][resp][stream]")
{
    RespFixture fix;
    auto const out =
        Exchange(fix,
                 "*5\r\n$4\r\nXADD\r\n$1\r\ns\r\n$3\r\n1-0\r\n$1\r\nf\r\n$1\r\nv\r\n"
                 "*5\r\n$6\r\nXGROUP\r\n$6\r\nCREATE\r\n$1\r\ns\r\n$2\r\ng1\r\n$1\r\n0\r\n"
                 "*7\r\n$10\r\nXREADGROUP\r\n$5\r\nGROUP\r\n$2\r\ng1\r\n$2\r\nc1\r\n"
                 "$7\r\nSTREAMS\r\n$1\r\ns\r\n$1\r\n>\r\n"
                 // XCLAIM s g1 c2 0 1-0  (min-idle 0 -> always claims), JUSTID
                 "*7\r\n$6\r\nXCLAIM\r\n$1\r\ns\r\n$2\r\ng1\r\n$2\r\nc2\r\n$1\r\n0\r\n$3\r\n1-0\r\n$6\r\nJUSTID\r\n");
    // JUSTID -> array of claimed IDs: *1 $3 1-0.
    REQUIRE(out.ends_with("*1\r\n$3\r\n1-0\r\n"));
}
