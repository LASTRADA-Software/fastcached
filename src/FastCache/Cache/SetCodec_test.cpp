// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/SetCodec.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

/// One engine, reached through BOTH protocols.
///
/// The point of the fixture: a memcached client and a redis client are two front
/// ends over one `CacheEngine`, so bytes a memcached `set` planted are bytes a redis
/// set verb decodes. That is the reachability this file exists to demonstrate, and it
/// is invisible from either protocol's own test file.
struct BothProtocols
{
    ManualClock clock;
    InMemoryLruStorage storage;
    CacheEngine engine { storage, clock };
    MemcachedTextHandler text;
    RedisRespHandler resp;
};

[[nodiscard]] Task<bool> WriteString(ISocket* socket, std::string_view payload)
{
    auto const result = co_await socket->Write(AsBytes(payload));
    co_return result.has_value();
}

[[nodiscard]] Task<std::string> ReadAvailable(ISocket* socket)
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

/// Run one request through `handler` against the shared engine and return the reply.
///
/// The half-close and the server-side `Close()` are the pattern every other protocol
/// test here uses, and for the reason they record: the handlers do not close their own
/// write side when their loop ends, so without it the drain relies on a partial-read
/// heuristic that parks forever on a reply whose length is an exact multiple of the
/// chunk size.
[[nodiscard]] std::string Exchange(BothProtocols& fix, IProtocolHandler& handler, std::string_view request)
{
    auto pair = InMemorySocketPair::Create();
    REQUIRE(SyncRun(WriteString(pair.client.get(), request)));
    pair.client->ShutdownWrite();
    SyncRun(handler.Run(pair.server.get(), &fix.engine, /*primingBytes=*/ {}, SessionContext {}));
    pair.server->Close();
    return SyncRun(ReadAvailable(pair.client.get()));
}

/// The six bytes a hostile client stores: a set header declaring 0xFFFFFFFF members
/// and carrying none of them.
[[nodiscard]] std::string HostileSetBlob()
{
    return std::string { "\xFC\x01\xFF\xFF\xFF\xFF", 6 };
}

} // namespace

TEST_CASE("A plain memcached client can plant a set blob a redis verb then decodes", "[cache][setcodec][security]")
{
    // Issue #271, and the reachability IS the finding -- so this drives the real path
    // rather than calling the decoder. What tags a value as a set is its `flags` word,
    // and `set <key> <flags> <exptime> <bytes>` lets an ordinary memcached client
    // choose it. 1584398337 is `SetCodec::FcTypeSet` in decimal.
    //
    // No privilege, no fleet membership, no special client: two stock front ends over
    // one engine. Before the guard, the SMEMBERS below reserved 0xFFFFFFFF
    // `std::string` -- roughly 137 GB -- and the `std::bad_alloc` was caught by nobody.
    static_assert(SetCodec::FcTypeSet == 1584398337U, "the memcached flags word below must name a set");

    BothProtocols fix;
    auto const blob = HostileSetBlob();

    // 1. Plant it as an ordinary memcached value, choosing the flags word.
    auto const stored = Exchange(fix, fix.text, "set evil 1584398337 0 6\r\n" + blob + "\r\n");
    REQUIRE(stored == "STORED\r\n");

    // 2. Read it back through a redis set verb, which routes it to `SetCodec::Decode`.
    auto const members = Exchange(fix, fix.resp, "*2\r\n$8\r\nSMEMBERS\r\n$4\r\nevil\r\n");

    // Refused, and refused as a REPLY: the connection carries an error the client can
    // read rather than being reset, which is what `Corrupt` maps to for a set command.
    CHECK(members == "-ERR storage failure\r\n");
    CHECK_FALSE(members.empty());
}

TEST_CASE("Every redis set verb refuses the planted blob, not just the read ones", "[cache][setcodec][security]")
{
    // `Decode` is reached from the read path (`WithSet`, used by SMEMBERS/SCARD) and
    // from the read-modify-write path (`LoadSet`, used by SADD/SREM). A guard on one
    // would leave the other reserving, so both are asserted.
    auto const blob = HostileSetBlob();

    auto planted = [&blob](BothProtocols& fix) {
        REQUIRE(Exchange(fix, fix.text, "set evil 1584398337 0 6\r\n" + blob + "\r\n") == "STORED\r\n");
    };

    SECTION("SCARD, through the read path")
    {
        BothProtocols fix;
        planted(fix);
        CHECK(Exchange(fix, fix.resp, "*2\r\n$5\r\nSCARD\r\n$4\r\nevil\r\n") == "-ERR storage failure\r\n");
    }

    SECTION("SADD, through the read-modify-write path")
    {
        BothProtocols fix;
        planted(fix);
        CHECK(Exchange(fix, fix.resp, "*3\r\n$4\r\nSADD\r\n$4\r\nevil\r\n$1\r\nx\r\n") == "-ERR storage failure\r\n");
    }
}

TEST_CASE("A set planted through memcached and well-formed still works", "[cache][setcodec]")
{
    // The guard must refuse impossible claims, not sets that merely arrived by an
    // unusual route -- otherwise this test would pass for the wrong reason and prove
    // only that the path was broken.
    BothProtocols fix;

    std::vector<std::string> const members { "a", "bb" };
    auto const encoded = SetCodec::Encode(members);
    std::string blob;
    for (auto const byte: encoded)
        blob.push_back(static_cast<char>(byte));

    REQUIRE(Exchange(fix, fix.text, "set good 1584398337 0 " + std::to_string(blob.size()) + "\r\n" + blob + "\r\n")
            == "STORED\r\n");
    CHECK(Exchange(fix, fix.resp, "*2\r\n$5\r\nSCARD\r\n$4\r\ngood\r\n") == ":2\r\n");
}

TEST_CASE("SetCodec::Decode refuses a member count the blob cannot supply", "[cache][setcodec]")
{
    // The decoder's own boundary, from both sides, so the guard is neither off by one
    // nor a number somebody picked. A member costs its four-byte length prefix at
    // minimum, so `n` bytes after the header can carry at most `n / 4` members.
    auto const blobDeclaring = [](std::uint32_t count, std::size_t trailing) {
        std::vector<std::byte> out { SetCodec::Magic, SetCodec::TypeSet };
        for (auto const shift: { 24, 16, 8, 0 })
            out.push_back(static_cast<std::byte>((count >> shift) & 0xFFU));
        out.insert(out.end(), trailing, std::byte { 0 });
        return out;
    };

    std::vector<std::string> members;

    // The shape that mattered: a huge count against no bytes at all.
    CHECK_FALSE(SetCodec::Decode(blobDeclaring(0xFFFFFFFFU, 0), members));

    // Three members cannot fit in eight bytes, and that is decided on the count alone.
    CHECK_FALSE(SetCodec::Decode(blobDeclaring(3, 8), members));

    // Two can, and those eight zero bytes really are two members: each a zero length
    // and no text. So the guard admits exactly what the blob can carry rather than
    // capping it at something smaller.
    REQUIRE(SetCodec::Decode(blobDeclaring(2, 8), members));
    CHECK(members.size() == 2);
    CHECK(members[0].empty());

    // And an empty set is ordinary.
    REQUIRE(SetCodec::Decode(blobDeclaring(0, 0), members));
    CHECK(members.empty());
}

TEST_CASE("MinMemberBytes tracks the encoder rather than a comment", "[cache][setcodec]")
{
    // The guard's per-element minimum is derived by hand from `Encode`'s loop, which is
    // comment discipline. This pins it structurally: the cost of one EMPTY member is
    // measured from the encoder itself, so a field added to that loop fails here rather
    // than quietly leaving the guard weaker than the format it guards.
    std::vector<std::string> const none {};
    std::vector<std::string> const one { "" };

    CHECK(SetCodec::Encode(one).size() - SetCodec::Encode(none).size() == SetCodec::MinMemberBytes);
}

TEST_CASE("A set round-trips through its own codec", "[cache][setcodec]")
{
    std::vector<std::string> const members { "alpha", "beta", "gamma" };
    std::vector<std::string> back;
    REQUIRE(SetCodec::Decode(SetCodec::Encode(members), back));
    CHECK(back == members);
}
