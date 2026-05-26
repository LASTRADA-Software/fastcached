// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/MemcachedBinary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct BinaryFixture
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage;
    FastCache::CacheEngine engine { storage, clock };
    FastCache::InMemorySocketPair pair = FastCache::InMemorySocketPair::Create();
    FastCache::MemcachedBinaryHandler handler;
};

void AppendBe16(std::vector<std::byte>& buf, std::uint16_t v)
{
    std::array<std::byte, 2> tmp {};
    FastCache::WriteBigEndian<std::uint16_t>(tmp, v);
    buf.insert(buf.end(), tmp.begin(), tmp.end());
}

void AppendBe32(std::vector<std::byte>& buf, std::uint32_t v)
{
    std::array<std::byte, 4> tmp {};
    FastCache::WriteBigEndian<std::uint32_t>(tmp, v);
    buf.insert(buf.end(), tmp.begin(), tmp.end());
}

void AppendBe64(std::vector<std::byte>& buf, std::uint64_t v)
{
    std::array<std::byte, 8> tmp {};
    FastCache::WriteBigEndian<std::uint64_t>(tmp, v);
    buf.insert(buf.end(), tmp.begin(), tmp.end());
}

std::vector<std::byte> BuildBinaryFrame(std::uint8_t opcode,
                                        std::string_view key,
                                        std::span<std::byte const> extras,
                                        std::string_view value,
                                        std::uint64_t cas = 0,
                                        std::uint32_t opaque = 0)
{
    std::vector<std::byte> frame;
    frame.push_back(std::byte { 0x80 });
    frame.push_back(std::byte { opcode });
    AppendBe16(frame, static_cast<std::uint16_t>(key.size()));
    frame.push_back(std::byte { static_cast<std::uint8_t>(extras.size()) });
    frame.push_back(std::byte { 0 }); // data type
    AppendBe16(frame, 0);              // vbucket
    AppendBe32(frame, static_cast<std::uint32_t>(extras.size() + key.size() + value.size()));
    AppendBe32(frame, opaque);
    AppendBe64(frame, cas);
    for (auto const b : extras)
        frame.push_back(b);
    for (auto const c : key)
        frame.push_back(static_cast<std::byte>(c));
    for (auto const c : value)
        frame.push_back(static_cast<std::byte>(c));
    return frame;
}

FastCache::Task<bool> Write(FastCache::ISocket& s, std::span<std::byte const> bytes)
{
    auto const r = co_await s.Write(bytes);
    co_return r.has_value();
}

FastCache::Task<std::vector<std::byte>> Drain(FastCache::ISocket& s)
{
    std::vector<std::byte> out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const r = co_await s.Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + *r);
        if (*r < chunk.size())
            break;
    }
    co_return out;
}

std::vector<std::byte> Exchange(BinaryFixture& fix, std::span<std::byte const> request)
{
    REQUIRE(FastCache::SyncRun(Write(*fix.pair.client, request)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(*fix.pair.server, fix.engine, /*primer*/ {}));
    return FastCache::SyncRun(Drain(*fix.pair.client));
}

} // namespace

TEST_CASE("memcached-binary: SET then GET round-trips", "[protocol][binary]")
{
    BinaryFixture fix;
    // SET k=hello, flags=1, exptime=0
    std::array<std::byte, 8> extras {};
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data(), 4 }, 1U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data() + 4, 4 }, 0U);

    auto setReq = BuildBinaryFrame(/*opcode*/ 0x01,
                                   /*key*/ "k",
                                   std::span<std::byte const> { extras.data(), extras.size() },
                                   /*value*/ "hello");
    auto getReq = BuildBinaryFrame(/*opcode*/ 0x00, /*key*/ "k", {}, /*value*/ {});
    setReq.insert(setReq.end(), getReq.begin(), getReq.end());

    auto const response = Exchange(fix, std::span<std::byte const> { setReq.data(), setReq.size() });
    // SET response: 24 bytes header + 0 body, status=Ok.
    // GET response: 24 bytes header + 4-byte extras + 5-byte value (no key).
    REQUIRE(response.size() == 24 + 24 + 4 + 5);

    // Decode the SET response status.
    auto const setStatus =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(setStatus == 0);
    // Decode the GET response status + value.
    auto const getStatus =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 24 + 6, 2 });
    REQUIRE(getStatus == 0);
    std::string value;
    for (std::size_t i = 24 + 24 + 4; i < response.size(); ++i)
        value.push_back(static_cast<char>(response[i]));
    REQUIRE(value == "hello");
}

TEST_CASE("memcached-binary: GET miss yields KeyNotFound", "[protocol][binary]")
{
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(0x00, "absent", {}, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() >= 24);
    auto const status =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 1); // Status::KeyNotFound
}

TEST_CASE("memcached-binary: NoOp echoes opaque", "[protocol][binary]")
{
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(/*opcode*/ 0x0a, {}, {}, {}, /*cas*/ 0, /*opaque*/ 0xDEADBEEFU);
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() == 24);
    auto const opaque =
        FastCache::ReadBigEndian<std::uint32_t>(std::span<std::byte const> { response.data() + 12, 4 });
    REQUIRE(opaque == 0xDEADBEEFU);
}

TEST_CASE("memcached-binary: SASL replies AuthError so non-authing clients fail fast", "[protocol][binary]")
{
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(/*opcode*/ 0x21, "PLAIN", {}, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() >= 24);
    auto const status =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 0x20); // Status::AuthError
}
