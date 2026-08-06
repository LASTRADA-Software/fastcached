// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using PathCanon::Grammar;

namespace
{

/// Fixture: a paired in-memory socket, an engine over in-memory storage, and
/// the compile-cache handler driving the server end.
struct CcFixture
{
    ManualClock clock;
    InMemoryLruStorage storage { 0 };
    CacheEngine engine { storage, clock };
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
};

/// Append a big-endian u32 to a byte buffer.
void AppendU32(std::vector<std::byte>& out, std::uint32_t n)
{
    std::array<std::byte, sizeof(std::uint32_t)> buf {};
    WriteBigEndian<std::uint32_t>(buf, n);
    out.insert(out.end(), buf.begin(), buf.end());
}

/// Append a length-prefixed string field to a byte buffer.
void AppendField(std::vector<std::byte>& out, std::string_view s)
{
    AppendU32(out, static_cast<std::uint32_t>(s.size()));
    auto const* p = reinterpret_cast<std::byte const*>(s.data());
    out.insert(out.end(), p, p + s.size());
}

/// Append a length-prefixed raw byte field.
void AppendField(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    AppendU32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// @param payload Bytes to write. Taken by value: a coroutine must not hold a
///                reference parameter across a suspend point.
Task<bool> WriteBytes(ISocket* socket, std::vector<std::byte> payload)
{
    auto const r = co_await socket->Write(std::span<std::byte const> { payload.data(), payload.size() });
    co_return r.has_value();
}

Task<std::vector<std::byte>> ReadAvailable(ISocket* socket)
{
    std::vector<std::byte> out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const r = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*r));
        if (*r < chunk.size())
            break;
    }
    co_return out;
}

/// Drive one batch of request bytes through the handler and return the raw
/// reply bytes. Half-closes the client write side so the handler sees EOF.
std::vector<std::byte> Exchange(CcFixture& fix, std::vector<std::byte> const& request)
{
    REQUIRE(SyncRun(WriteBytes(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primingBytes*/ {}, session));
    return SyncRun(ReadAvailable(fix.pair.client.get()));
}

/// The four string fields of a STORE frame. Named rather than positional so the
/// same-typed fields cannot be transposed at a call site.
struct StoreFields
{
    std::string_view key;
    std::string_view cohort;
    std::string_view srcRoot;
    std::string_view buildTree;
};

/// Build a STORE frame.
std::vector<std::byte> StoreFrame(StoreFields const& fields, CompileValue const& value)
{
    std::vector<std::byte> f;
    f.push_back(CompileCacheMagic);
    f.push_back(static_cast<std::byte>(CompileCacheHandler::Op::Store));
    AppendField(f, fields.key);
    AppendField(f, fields.cohort);
    AppendField(f, fields.srcRoot);
    AppendField(f, fields.buildTree);
    auto const encoded = EncodeCompileValue(value);
    AppendField(f, std::span<std::byte const> { encoded.data(), encoded.size() });
    return f;
}

/// Build a FETCH frame.
std::vector<std::byte> FetchFrame(std::string_view key)
{
    std::vector<std::byte> f;
    f.push_back(CompileCacheMagic);
    f.push_back(static_cast<std::byte>(CompileCacheHandler::Op::Fetch));
    AppendField(f, key);
    return f;
}

/// Decode a FETCH reply into a CompileValue. Expects [0x01][u32 len][bytes].
std::optional<CompileValue> DecodeFetchHit(std::span<std::byte const> reply)
{
    if (reply.size() < 5 || reply[0] != static_cast<std::byte>(CompileCacheHandler::Status::Ok))
        return std::nullopt;
    auto const len = ReadBigEndian<std::uint32_t>(reply.subspan(1, 4));
    if (reply.size() < 5 + len)
        return std::nullopt;
    auto decoded = DecodeCompileValue(reply.subspan(5, len));
    if (!decoded.has_value())
        return std::nullopt;
    return *decoded;
}

} // namespace

TEST_CASE("STORE canonicalizes showIncludes; FETCH returns the canonical form", "[compile-cache][handler]")
{
    CcFixture fix;

    CompileValue v;
    v.objectBlob = { std::byte { 0xDE }, std::byte { 0xAD } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\ci\deep\src\a.h)"
                                       "\r\n" });

    // STORE from a deep "CI" layout.
    auto const storeReply = Exchange(
        fix,
        StoreFrame(
            { .key = "obj-hash", .cohort = "envCI", .srcRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" },
            v));
    REQUIRE(storeReply.size() == 1);
    CHECK(storeReply[0] == static_cast<std::byte>(CompileCacheHandler::Status::Ok));

    // FETCH: the value must be canonical (no machine path), object blob intact.
    // The first handler consumed EOF and returned, so drive a fresh handler
    // over the SAME engine (same storage) for the fetch.
    InMemorySocketPair pair2 = InMemorySocketPair::Create();
    CompileCacheHandler handler2;
    REQUIRE(SyncRun(WriteBytes(pair2.client.get(), FetchFrame("obj-hash"))));
    pair2.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler2.Run(pair2.server.get(), &fix.engine, {}, session));
    auto const fetchReply = SyncRun(ReadAvailable(pair2.client.get()));

    // value_or keeps the access unguarded-safe: static analysis cannot see a
    // has_value() guard through Catch2's REQUIRE macro.
    auto const decoded = DecodeFetchHit(fetchReply);
    REQUIRE(decoded.has_value());
    auto const fetched = decoded.value_or(CompileValue {});
    REQUIRE(fetched.textRegions.size() == 1);
    CHECK(fetched.textRegions[0].bytes.contains("<SRCROOT>/a.h"));
    CHECK_FALSE(fetched.textRegions[0].bytes.contains(R"(ci\deep)"));
    CHECK(fetched.objectBlob == std::vector<std::byte> { std::byte { 0xDE }, std::byte { 0xAD } });
}

TEST_CASE("FETCH of an absent key replies miss", "[compile-cache][handler]")
{
    CcFixture fix;
    auto const reply = Exchange(fix, FetchFrame("nope"));
    REQUIRE(reply.size() == 1);
    CHECK(reply[0] == static_cast<std::byte>(CompileCacheHandler::Status::Err));
}

TEST_CASE("Cross-depth: value stored from a deep layout localizes for a shallow consumer",
          "[compile-cache][handler][crossdepth]")
{
    CcFixture fix;

    CompileValue v;
    v.objectBlob = { std::byte { 0x01 } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\ci\deep\runner\src\inc\x.h)"
                                       "\r\n" });

    // Produced at a deep path (CI runner nested deeper).
    auto const storeReply = Exchange(fix,
                                     StoreFrame({ .key = "k",
                                                  .cohort = "envCI",
                                                  .srcRoot = R"(C:\ci\deep\runner\src)",
                                                  .buildTree = R"(C:\ci\deep\runner\build)" },
                                                v));
    REQUIRE(storeReply[0] == static_cast<std::byte>(CompileCacheHandler::Status::Ok));

    // Fetch the canonical value and localize it as a SHALLOW consumer would.
    InMemorySocketPair pair2 = InMemorySocketPair::Create();
    CompileCacheHandler handler2;
    REQUIRE(SyncRun(WriteBytes(pair2.client.get(), FetchFrame("k"))));
    pair2.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler2.Run(pair2.server.get(), &fix.engine, {}, session));
    auto const reply = SyncRun(ReadAvailable(pair2.client.get()));

    auto const decoded = DecodeFetchHit(reply);
    REQUIRE(decoded.has_value());
    auto const fetched = decoded.value_or(CompileValue {});
    REQUIRE(fetched.textRegions.size() == 1);

    PathCanon::Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    auto const localized = PathCanon::LocalizeRegion(fetched.textRegions[0].bytes, Grammar::ShowIncludes, consumer);
    REQUIRE(localized.has_value());
    CHECK(localized->contains(R"(D:\project\inc\x.h)"));
    CHECK_FALSE(localized->contains(R"(ci\deep)"));
}

namespace
{

/// A single valid compile value for cohort tests.
CompileValue SampleValue()
{
    CompileValue v;
    v.objectBlob = { std::byte { 0x7F } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\src\h.h)"
                                       "\r\n" });
    return v;
}

/// Store `key` under `cohort` via a fresh handler over `engine`.
void StoreVia(CacheEngine& engine, std::string_view key, std::string_view cohort)
{
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
    auto const frame =
        StoreFrame({ .key = key, .cohort = cohort, .srcRoot = R"(C:\src)", .buildTree = R"(C:\build)" }, SampleValue());
    REQUIRE(SyncRun(WriteBytes(pair.client.get(), frame)));
    pair.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler.Run(pair.server.get(), &engine, {}, session));
    (void) SyncRun(ReadAvailable(pair.client.get()));
}

} // namespace

TEST_CASE("FETCH of a cohort member warms the rest of the cohort into L1", "[compile-cache][handler][prefetch]")
{
    ManualClock clock;
    auto l1 = std::make_unique<InMemoryLruStorage>(0);
    auto l2 = std::make_unique<InMemoryLruStorage>(0);
    LayeredStorage layered { std::move(l1), std::move(l2) };
    CacheEngine engine { layered, clock };

    // Store three cohort members.
    StoreVia(engine, "k1", "envCI");
    StoreVia(engine, "k2", "envCI");
    StoreVia(engine, "k3", "envCI");

    // Evict the L1 mirror so members live only in L2 (cold L1).
    layered.L1().EraseIfPresent("k1");
    layered.L1().EraseIfPresent("k2");
    layered.L1().EraseIfPresent("k3");
    REQUIRE_FALSE(layered.L1().Peek("k2", clock.Now())->found);
    REQUIRE_FALSE(layered.L1().Peek("k3", clock.Now())->found);

    // Fetch the leading key: triggers a cohort prefetch of k2/k3 into L1.
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
    REQUIRE(SyncRun(WriteBytes(pair.client.get(), FetchFrame("k1"))));
    pair.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler.Run(pair.server.get(), &engine, {}, session));
    (void) SyncRun(ReadAvailable(pair.client.get()));

    // k2 and k3 are now warm in L1 (a direct L1 Peek finds them).
    CHECK(layered.L1().Peek("k2", clock.Now())->found);
    CHECK(layered.L1().Peek("k3", clock.Now())->found);
}
