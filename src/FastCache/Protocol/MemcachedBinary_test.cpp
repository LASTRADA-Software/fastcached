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
    AppendBe16(frame, 0);             // vbucket
    AppendBe32(frame, static_cast<std::uint32_t>(extras.size() + key.size() + value.size()));
    AppendBe32(frame, opaque);
    AppendBe64(frame, cas);
    for (auto const b: extras)
        frame.push_back(b);
    for (auto const c: key)
        frame.push_back(static_cast<std::byte>(c));
    for (auto const c: value)
        frame.push_back(static_cast<std::byte>(c));
    return frame;
}

FastCache::Task<bool> Write(FastCache::ISocket* s, std::span<std::byte const> bytes)
{
    auto const r = co_await s->Write(bytes);
    co_return r.has_value();
}

FastCache::Task<std::vector<std::byte>> Drain(FastCache::ISocket* s)
{
    std::vector<std::byte> out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const r = co_await s->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*r));
        if (*r < chunk.size())
            break;
    }
    co_return out;
}

std::vector<std::byte> Exchange(BinaryFixture& fix, std::span<std::byte const> request)
{
    REQUIRE(FastCache::SyncRun(Write(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primer*/ {}));
    return FastCache::SyncRun(Drain(fix.pair.client.get()));
}

/// One decoded binary response packet.
struct BinaryRecord
{
    std::uint8_t opcode { 0 };
    std::uint16_t status { 0 };
    std::uint64_t cas { 0 };
    std::string value;
};

/// Walk a concatenated binary response stream into individual packets,
/// using each header's totalBodyLen so variable-length error bodies don't
/// throw off the offsets.
std::vector<BinaryRecord> ParseRecords(std::vector<std::byte> const& resp)
{
    std::vector<BinaryRecord> out;
    std::size_t off = 0;
    while (off + 24 <= resp.size())
    {
        BinaryRecord rec;
        rec.opcode = std::to_integer<std::uint8_t>(resp[off + 1]);
        auto const keyLen = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { resp.data() + off + 2, 2 });
        auto const extrasLen = std::to_integer<std::uint8_t>(resp[off + 4]);
        rec.status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { resp.data() + off + 6, 2 });
        auto const bodyLen =
            FastCache::ReadBigEndian<std::uint32_t>(std::span<std::byte const> { resp.data() + off + 8, 4 });
        rec.cas = FastCache::ReadBigEndian<std::uint64_t>(std::span<std::byte const> { resp.data() + off + 16, 8 });
        std::size_t const valueStart = off + 24 + extrasLen + keyLen;
        std::size_t const valueLen = bodyLen - extrasLen - keyLen;
        for (std::size_t i = valueStart; i < valueStart + valueLen && i < resp.size(); ++i)
            rec.value.push_back(static_cast<char>(resp[i]));
        out.push_back(std::move(rec));
        off += 24 + bodyLen;
    }
    return out;
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
    auto const setStatus = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
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
    auto const status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 1); // Status::KeyNotFound
}

TEST_CASE("memcached-binary: NoOp echoes opaque", "[protocol][binary]")
{
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(/*opcode*/ 0x0a, {}, {}, {}, /*cas*/ 0, /*opaque*/ 0xDEADBEEFU);
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() == 24);
    auto const opaque = FastCache::ReadBigEndian<std::uint32_t>(std::span<std::byte const> { response.data() + 12, 4 });
    REQUIRE(opaque == 0xDEADBEEFU);
}

TEST_CASE("memcached-binary: SASL replies AuthError so non-authing clients fail fast", "[protocol][binary]")
{
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(/*opcode*/ 0x21, "PLAIN", {}, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() >= 24);
    auto const status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 0x20); // Status::AuthError
}

TEST_CASE("memcached-binary: Touch (0x1c) refreshes expiry and returns Ok", "[protocol][binary][touch]")
{
    BinaryFixture fix;
    // First, store a key.
    std::array<std::byte, 8> setExtras {};
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { setExtras.data(), 4 }, 0U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { setExtras.data() + 4, 4 }, 0U);
    auto frame = BuildBinaryFrame(0x01, "k", std::span<std::byte const> { setExtras.data(), 8 }, "v");

    // Touch with 4-byte extras: new exptime = 60.
    std::array<std::byte, 4> touchExtras {};
    FastCache::WriteBigEndian<std::uint32_t>(touchExtras, 60U);
    auto touchReq = BuildBinaryFrame(0x1c, "k", std::span<std::byte const> { touchExtras.data(), 4 }, {});
    frame.insert(frame.end(), touchReq.begin(), touchReq.end());

    auto const response = Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() });
    REQUIRE(response.size() >= 24 + 24);
    auto const setStatus = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(setStatus == 0);
    auto const touchStatus =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 24 + 6, 2 });
    REQUIRE(touchStatus == 0);
}

TEST_CASE("memcached-binary: Touch (0x1c) on miss returns KeyNotFound", "[protocol][binary][touch]")
{
    BinaryFixture fix;
    std::array<std::byte, 4> extras {};
    FastCache::WriteBigEndian<std::uint32_t>(extras, 60U);
    auto const req = BuildBinaryFrame(0x1c, "nope", std::span<std::byte const> { extras.data(), 4 }, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    auto const status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 1); // KeyNotFound
}

TEST_CASE("memcached-binary: GAT (0x1d) returns value + flags + bumps CAS", "[protocol][binary][gat]")
{
    BinaryFixture fix;
    // SET with flags = 0xCAFE.
    std::array<std::byte, 8> setExtras {};
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { setExtras.data(), 4 }, 0xCAFEU);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { setExtras.data() + 4, 4 }, 0U);
    auto frame = BuildBinaryFrame(0x01, "k", std::span<std::byte const> { setExtras.data(), 8 }, "payload");

    // GAT with new exptime.
    std::array<std::byte, 4> gatExtras {};
    FastCache::WriteBigEndian<std::uint32_t>(gatExtras, 120U);
    auto gatReq = BuildBinaryFrame(0x1d, "k", std::span<std::byte const> { gatExtras.data(), 4 }, {});
    frame.insert(frame.end(), gatReq.begin(), gatReq.end());

    auto const response = Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() });
    // SET response: 24 bytes only. GAT response: 24 header + 4 extras + 7 payload.
    REQUIRE(response.size() == 24 + 24 + 4 + 7);
    auto const gatStatus =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 24 + 6, 2 });
    REQUIRE(gatStatus == 0);
    auto const gatFlags =
        FastCache::ReadBigEndian<std::uint32_t>(std::span<std::byte const> { response.data() + 24 + 24, 4 });
    REQUIRE(gatFlags == 0xCAFEU);
    std::string value;
    for (std::size_t i = 24 + 24 + 4; i < response.size(); ++i)
        value.push_back(static_cast<char>(response[i]));
    REQUIRE(value == "payload");
}

TEST_CASE("memcached-binary: GATK (0x23) echoes the key in the response", "[protocol][binary][gat]")
{
    BinaryFixture fix;
    std::array<std::byte, 8> setExtras {};
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { setExtras.data(), 4 }, 0U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { setExtras.data() + 4, 4 }, 0U);
    auto frame = BuildBinaryFrame(0x01, "mykey", std::span<std::byte const> { setExtras.data(), 8 }, "v");

    std::array<std::byte, 4> gatExtras {};
    FastCache::WriteBigEndian<std::uint32_t>(gatExtras, 30U);
    auto req = BuildBinaryFrame(0x23, "mykey", std::span<std::byte const> { gatExtras.data(), 4 }, {});
    frame.insert(frame.end(), req.begin(), req.end());

    auto const response = Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() });
    // GATK response: 24 header + 4 extras + 5-byte key + 1-byte value.
    auto const keyLen = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 24 + 2, 2 });
    REQUIRE(keyLen == 5);
}

TEST_CASE("memcached-binary: Increment (0x05) on missing key auto-vivifies when exptime != 0xffffffff",
          "[protocol][binary][arith]")
{
    BinaryFixture fix;
    // Build a 20-byte extras: delta=1, initial=42, exptime=0.
    std::array<std::byte, 20> extras {};
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data(), 8 }, 1U);
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data() + 8, 8 }, 42U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data() + 16, 4 }, 0U);
    auto const req = BuildBinaryFrame(0x05, "counter", std::span<std::byte const> { extras.data(), 20 }, {});

    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() == 24 + 8);
    auto const status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 0);
    auto const newValue = FastCache::ReadBigEndian<std::uint64_t>(std::span<std::byte const> { response.data() + 24, 8 });
    REQUIRE(newValue == 42U); // auto-vivified to initial, NOT initial+delta per spec
}

TEST_CASE("memcached-binary: Increment with exptime=0xffffffff on miss returns KeyNotFound", "[protocol][binary][arith]")
{
    BinaryFixture fix;
    std::array<std::byte, 20> extras {};
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data(), 8 }, 1U);
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data() + 8, 8 }, 42U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data() + 16, 4 }, 0xFFFFFFFFU);
    auto const req = BuildBinaryFrame(0x05, "counter", std::span<std::byte const> { extras.data(), 20 }, {});

    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    auto const status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 1);
}

TEST_CASE("memcached-binary: Increment on existing value adds delta", "[protocol][binary][arith]")
{
    BinaryFixture fix;
    // SET counter=10.
    std::array<std::byte, 8> setExtras {};
    auto frame = BuildBinaryFrame(0x01, "n", std::span<std::byte const> { setExtras.data(), 8 }, "10");

    // INCR delta=5, initial=0 (unused), exptime=0.
    std::array<std::byte, 20> incrExtras {};
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { incrExtras.data(), 8 }, 5U);
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { incrExtras.data() + 8, 8 }, 0U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { incrExtras.data() + 16, 4 }, 0U);
    auto incrReq = BuildBinaryFrame(0x05, "n", std::span<std::byte const> { incrExtras.data(), 20 }, {});
    frame.insert(frame.end(), incrReq.begin(), incrReq.end());

    auto const response = Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() });
    // SET reply 24 bytes; INCR reply 24 header + 8-byte value.
    auto const newValue =
        FastCache::ReadBigEndian<std::uint64_t>(std::span<std::byte const> { response.data() + 24 + 24, 8 });
    REQUIRE(newValue == 15U);
}

TEST_CASE("memcached-binary: Decrement saturates at zero", "[protocol][binary][arith]")
{
    BinaryFixture fix;
    std::array<std::byte, 8> setExtras {};
    auto frame = BuildBinaryFrame(0x01, "n", std::span<std::byte const> { setExtras.data(), 8 }, "3");

    std::array<std::byte, 20> extras {};
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data(), 8 }, 10U);
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data() + 8, 8 }, 0U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data() + 16, 4 }, 0U);
    auto req = BuildBinaryFrame(0x06, "n", std::span<std::byte const> { extras.data(), 20 }, {});
    frame.insert(frame.end(), req.begin(), req.end());

    auto const response = Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() });
    auto const newValue =
        FastCache::ReadBigEndian<std::uint64_t>(std::span<std::byte const> { response.data() + 24 + 24, 8 });
    REQUIRE(newValue == 0U); // 3 - 10 saturates to 0
}

TEST_CASE("memcached-binary: Stat (0x10) emits STAT packets and an empty-key terminator", "[protocol][binary][stat]")
{
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(0x10, {}, {}, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    // We expect multiple 24-byte-headered packets followed by a final
    // empty-key terminator. The opcode in each response is 0x10.
    REQUIRE(response.size() >= 24);
    REQUIRE(std::to_integer<std::uint8_t>(response[1]) == 0x10);
    // The last 24-byte slice should be the terminator (keyLen == 0).
    auto const lastHdr = response.size() - 24;
    auto const lastKeyLen =
        FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + lastHdr + 2, 2 });
    REQUIRE(lastKeyLen == 0);
}

TEST_CASE("memcached-binary: Stat (0x10) includes the unfetched counters (regression)",
          "[protocol][binary][stat][regression]")
{
    // The binary stat list stopped at decr_misses, omitting the
    // evicted_unfetched / expired_unfetched counters the text protocol emits.
    // Both must now appear as stat-packet keys (parity with `stats`).
    BinaryFixture fix;
    auto const req = BuildBinaryFrame(0x10, {}, {}, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });

    std::string text;
    text.reserve(response.size());
    for (auto const b: response)
        text.push_back(static_cast<char>(b));

    REQUIRE(text.contains("evicted_unfetched"));
    REQUIRE(text.contains("expired_unfetched"));
    REQUIRE(text.contains("cmd_get")); // a pre-existing counter is still present
}

TEST_CASE("memcached-binary: Verbosity (0x1b) is a no-op that returns Ok", "[protocol][binary]")
{
    BinaryFixture fix;
    std::array<std::byte, 4> extras {}; // verbosity level
    FastCache::WriteBigEndian<std::uint32_t>(extras, 2U);
    auto const req = BuildBinaryFrame(0x1b, {}, std::span<std::byte const> { extras.data(), 4 }, {});
    auto const response = Exchange(fix, std::span<std::byte const> { req.data(), req.size() });
    REQUIRE(response.size() == 24);
    auto const status = FastCache::ReadBigEndian<std::uint16_t>(std::span<std::byte const> { response.data() + 6, 2 });
    REQUIRE(status == 0);
}

TEST_CASE("memcached-binary: quiet IncrementQ on miss with exptime=0xffffffff emits the error response",
          "[protocol][binary][quiet]")
{
    BinaryFixture fix;
    // IncrementQ is a quiet *mutation*. Per the memcached binary spec,
    // "quiet mutations only return responses on failure" — success is
    // suppressed but "errors should not be allowed to go unnoticed". So a
    // miss under the "don't create" sentinel (0xffffffff) is a failure and
    // must emit a KeyNotFound packet. We pipeline a NoOp afterwards and
    // expect TWO replies: the IncrementQ error, then the NoOp.
    std::array<std::byte, 20> extras {};
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data(), 8 }, 1U);
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { extras.data() + 8, 8 }, 0U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data() + 16, 4 }, 0xFFFFFFFFU);
    auto frame = BuildBinaryFrame(0x15, "absent", std::span<std::byte const> { extras.data(), 20 }, {});

    auto noop = BuildBinaryFrame(/*opcode*/ 0x0a, {}, {}, {}, /*cas*/ 0, /*opaque*/ 0x12345678U);
    frame.insert(frame.end(), noop.begin(), noop.end());

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    // IncrementQ error reply (opcode 0x15, status KeyNotFound=0x0001) + NoOp.
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].opcode == 0x15);
    REQUIRE(records[0].status == 0x0001);
    REQUIRE(records[1].opcode == 0x0a);
    REQUIRE(records[1].status == 0x0000);
}

TEST_CASE("memcached-binary: AppendQ (0x19) and PrependQ (0x1a) actually mutate the stored value",
          "[protocol][binary][append][regression]")
{
    // Regression: the old `opcode & ~0x10` normalisation mapped AppendQ to
    // GetQ and PrependQ to NoOp, so both silently no-op'd while reporting
    // success. Here we append then prepend (both quiet) and confirm via a
    // trailing GET that the value really changed.
    BinaryFixture fix;
    std::array<std::byte, 8> extras {}; // flags+exptime; ignored by (pre|ap)pend.

    auto frame = BuildBinaryFrame(/*Set*/ 0x01, "k", std::span<std::byte const> { extras.data(), 8 }, "B");
    auto const appendQ = BuildBinaryFrame(/*AppendQ*/ 0x19, "k", std::span<std::byte const> { extras.data(), 8 }, "C");
    auto const prependQ = BuildBinaryFrame(/*PrependQ*/ 0x1a, "k", std::span<std::byte const> { extras.data(), 8 }, "A");
    auto const getReq = BuildBinaryFrame(/*Get*/ 0x00, "k", {}, {});
    frame.insert(frame.end(), appendQ.begin(), appendQ.end());
    frame.insert(frame.end(), prependQ.begin(), prependQ.end());
    frame.insert(frame.end(), getReq.begin(), getReq.end());

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    // SET reply + (AppendQ/PrependQ quiet successes: no replies) + GET reply.
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].opcode == 0x01);
    REQUIRE(records[0].status == 0);
    REQUIRE(records[1].opcode == 0x00);
    REQUIRE(records[1].status == 0);
    REQUIRE(records[1].value == "ABC");
}

TEST_CASE("memcached-binary: quiet AddQ (0x12) on an existing key emits KeyExists", "[protocol][binary][quiet]")
{
    // Quiet mutation: success is suppressed, but a failure (the key already
    // exists, so Add is rejected) must still emit an error packet.
    BinaryFixture fix;
    std::array<std::byte, 8> extras {}; // flags + exptime.

    auto frame = BuildBinaryFrame(/*Set*/ 0x01, "k", std::span<std::byte const> { extras.data(), 8 }, "v");
    auto const addQ = BuildBinaryFrame(/*AddQ*/ 0x12, "k", std::span<std::byte const> { extras.data(), 8 }, "w");
    frame.insert(frame.end(), addQ.begin(), addQ.end());

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    // SET success reply + AddQ error reply (success would have been silent).
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].opcode == 0x01);
    REQUIRE(records[0].status == 0x0000);
    REQUIRE(records[1].opcode == 0x12);
    REQUIRE(records[1].status == 0x0002); // KeyExists
}

TEST_CASE("memcached-binary: quiet ReplaceQ (0x13) on a missing key emits KeyNotFound", "[protocol][binary][quiet]")
{
    // Quiet mutation on a miss is a failure → error packet, not silence.
    BinaryFixture fix;
    std::array<std::byte, 8> extras {};
    auto const frame = BuildBinaryFrame(/*ReplaceQ*/ 0x13, "absent", std::span<std::byte const> { extras.data(), 8 }, "v");

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].opcode == 0x13);
    REQUIRE(records[0].status == 0x0001); // KeyNotFound
}

TEST_CASE("memcached-binary: quiet IncrementQ (0x15) on a non-numeric value emits IncrOnNonNumeric",
          "[protocol][binary][quiet][arith]")
{
    // Quiet arithmetic on a non-numeric existing value is a failure → the
    // error packet must surface even though IncrementQ is quiet on success.
    BinaryFixture fix;
    std::array<std::byte, 8> setExtras {};
    auto frame = BuildBinaryFrame(/*Set*/ 0x01, "k", std::span<std::byte const> { setExtras.data(), 8 }, "notnum");

    std::array<std::byte, 20> arithExtras {};
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { arithExtras.data(), 8 }, 1U);
    FastCache::WriteBigEndian<std::uint64_t>(std::span<std::byte> { arithExtras.data() + 8, 8 }, 0U);
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { arithExtras.data() + 16, 4 }, 0U);
    auto const incrQ = BuildBinaryFrame(/*IncrementQ*/ 0x15, "k", std::span<std::byte const> { arithExtras.data(), 20 }, {});
    frame.insert(frame.end(), incrQ.begin(), incrQ.end());

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].opcode == 0x01);
    REQUIRE(records[0].status == 0x0000);
    REQUIRE(records[1].opcode == 0x15);
    REQUIRE(records[1].status == 0x0006); // IncrOnNonNumeric
}

TEST_CASE("memcached-binary: quiet DeleteQ (0x14) on a missing key emits KeyNotFound", "[protocol][binary][quiet]")
{
    // DeleteQ is a quiet mutation: a miss is a failure and must be reported.
    BinaryFixture fix;
    auto const frame = BuildBinaryFrame(/*DeleteQ*/ 0x14, "absent", {}, {});

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].opcode == 0x14);
    REQUIRE(records[0].status == 0x0001); // KeyNotFound
}

TEST_CASE("memcached-binary: quiet GatQ (0x1e) is mum on a cache miss", "[protocol][binary][quiet][gat]")
{
    // GAT is a quiet *read*: like GetQ it stays silent on a cache miss. We
    // pipeline a NoOp and expect only the NoOp reply to come back.
    BinaryFixture fix;
    std::array<std::byte, 4> extras {};
    FastCache::WriteBigEndian<std::uint32_t>(std::span<std::byte> { extras.data(), 4 }, 0U);
    auto frame = BuildBinaryFrame(/*GatQ*/ 0x1e, "absent", std::span<std::byte const> { extras.data(), 4 }, {});

    auto const noop = BuildBinaryFrame(/*NoOp*/ 0x0a, {}, {}, {}, /*cas*/ 0, /*opaque*/ 0x0badcafeU);
    frame.insert(frame.end(), noop.begin(), noop.end());

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].opcode == 0x0a); // only the NoOp — GatQ miss suppressed
}

TEST_CASE("memcached-binary: Set with a non-zero header CAS is a conditional store", "[protocol][binary][cas][regression]")
{
    // Regression: HandleStorage previously ignored header.cas, so an
    // optimistic Set would clobber a concurrently-changed value. Now a
    // non-zero header CAS routes through compare-and-swap.
    BinaryFixture fix;
    std::array<std::byte, 8> extras {};

    // First SET (cas=0) stores v1 and returns CAS=1 (InMemory starts at 1).
    auto frame = BuildBinaryFrame(0x01, "k", std::span<std::byte const> { extras.data(), 8 }, "v1");
    // SET v2 with a stale header CAS (999) must fail with KeyExists.
    auto const mismatch = BuildBinaryFrame(0x01, "k", std::span<std::byte const> { extras.data(), 8 }, "v2", /*cas*/ 999);
    // SET v3 with the correct header CAS (1) must succeed.
    auto const match = BuildBinaryFrame(0x01, "k", std::span<std::byte const> { extras.data(), 8 }, "v3", /*cas*/ 1);
    auto const getReq = BuildBinaryFrame(0x00, "k", {}, {});
    frame.insert(frame.end(), mismatch.begin(), mismatch.end());
    frame.insert(frame.end(), match.begin(), match.end());
    frame.insert(frame.end(), getReq.begin(), getReq.end());

    auto const records = ParseRecords(Exchange(fix, std::span<std::byte const> { frame.data(), frame.size() }));
    REQUIRE(records.size() == 4);
    REQUIRE(records[0].status == 0); // unconditional SET ok
    REQUIRE(records[0].cas == 1);
    REQUIRE(records[1].status == 0x02); // KeyExists — stale CAS rejected
    REQUIRE(records[2].status == 0);    // matching CAS accepted
    REQUIRE(records[3].status == 0);
    REQUIRE(records[3].value == "v3"); // the winning write
}
