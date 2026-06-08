// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{

struct MetaFixture
{
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
        auto const r = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        for (std::size_t i = 0; i < *r; ++i)
            out.push_back(static_cast<char>(chunk[i]));
        if (*r < chunk.size())
            break;
    }
    co_return out;
}

std::string Exchange(MetaFixture& fix, std::string_view req)
{
    REQUIRE(FastCache::SyncRun(WriteString(fix.pair.client.get(), req)));
    fix.pair.client->ShutdownWrite();
    FastCache::SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*priming*/ {}, /*session*/ {}));
    return FastCache::SyncRun(ReadAvailable(fix.pair.client.get()));
}

} // namespace

TEST_CASE("meta mn returns MN", "[protocol][meta]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "mn\r\n");
    REQUIRE(r == "MN\r\n");
}

TEST_CASE("meta mg miss returns EN", "[protocol][meta][mg]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "mg absent\r\n");
    REQUIRE(r == "EN\r\n");
}

TEST_CASE("meta mg with v flag returns the value as a VA block", "[protocol][meta][mg]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 7 0 5\r\nhello\r\nmg k v\r\n");
    REQUIRE(r == "STORED\r\nVA 5\r\nhello\r\n");
}

TEST_CASE("meta mg with c and k echos cas and key", "[protocol][meta][mg]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmg k c k\r\n");
    // CAS == 1 after a fresh set; mg without v emits HD.
    REQUIRE(r.contains("HD"));
    REQUIRE(r.contains(" c1"));
    REQUIRE(r.contains(" kk"));
}

TEST_CASE("meta mg with s and f reports size and flags", "[protocol][meta][mg]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 42 0 3\r\nfoo\r\nmg k s f\r\n");
    REQUIRE(r.contains(" s3"));
    REQUIRE(r.contains(" f42"));
}

TEST_CASE("meta mg with T refreshes TTL", "[protocol][meta][mg][touch]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 1 1\r\nA\r\nmg k T120 v\r\n");
    // The mg should still hit (touch happens, then return), value=A.
    REQUIRE(r.contains("VA 1\r\nA\r\n"));
}

TEST_CASE("meta mg with N auto-vivifies on miss", "[protocol][meta][mg][vivify]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "mg fresh N60 v\r\n");
    // Auto-vivified empty value; the response should be VA 0 with no body.
    REQUIRE(r.contains("VA 0\r\n"));
}

TEST_CASE("meta mg with q on miss emits nothing -- verified via piped mn", "[protocol][meta][mg][quiet]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "mg absent q\r\nmn\r\n");
    REQUIRE(r == "MN\r\n");
}

TEST_CASE("meta mg with O echoes the opaque token", "[protocol][meta][mg][opaque]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmg k Oabc123\r\n");
    REQUIRE(r.contains(" Oabc123"));
}

TEST_CASE("meta ms stores a value via S mode (default)", "[protocol][meta][ms]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms k 3\r\nfoo\r\nget k\r\n");
    // ms default response is HD; then a classic get returns the value.
    REQUIRE(r == "HD\r\nVALUE k 0 3\r\nfoo\r\nEND\r\n");
}

TEST_CASE("meta ms with M=E (add) fails when key exists", "[protocol][meta][ms]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms k 1\r\nA\r\nms k 1 M=E\r\nB\r\n");
    REQUIRE(r == "HD\r\nNS\r\n");
}

TEST_CASE("meta ms with C and a wrong CAS returns EX", "[protocol][meta][ms]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms k 1\r\nA\r\nms k 1 C999\r\nB\r\n");
    REQUIRE(r == "HD\r\nEX\r\n");
}

TEST_CASE("meta ms with q suppresses the response (validated via mn)", "[protocol][meta][ms][quiet]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms k 1 q\r\nA\r\nmn\r\n");
    REQUIRE(r == "MN\r\n");
}

TEST_CASE("meta ms with T sets a TTL", "[protocol][meta][ms]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms k 1 T60\r\nA\r\nmg k v t\r\n");
    REQUIRE(r.contains("VA 1"));
    REQUIRE(r.contains(" t"));
}

TEST_CASE("meta md deletes the key and returns HD", "[protocol][meta][md]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmd k\r\nmg k\r\n");
    REQUIRE(r == "STORED\r\nHD\r\nEN\r\n");
}

TEST_CASE("meta md on missing key returns NF", "[protocol][meta][md]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "md absent\r\n");
    REQUIRE(r == "NF\r\n");
}

TEST_CASE("meta md with mismatched C returns EX", "[protocol][meta][md]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmd k C999\r\n");
    REQUIRE(r == "STORED\r\nEX\r\n");
}

TEST_CASE("meta ma increments by default delta of 1", "[protocol][meta][ma]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set c 0 0 2\r\n10\r\nma c v\r\n");
    REQUIRE(r.contains("VA 2\r\n11\r\n"));
}

TEST_CASE("meta ma D mode decrements", "[protocol][meta][ma]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set c 0 0 2\r\n10\r\nma c M=D D3 v\r\n");
    REQUIRE(r.contains("VA 1\r\n7\r\n"));
}

TEST_CASE("meta ma N+J auto-vivifies", "[protocol][meta][ma][vivify]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ma c N60 J42 v\r\n");
    REQUIRE(r.contains("VA 2\r\n42\r\n"));
}

TEST_CASE("meta ma without v returns just HD on success", "[protocol][meta][ma]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set c 0 0 1\r\n5\r\nma c\r\n");
    REQUIRE(r == "STORED\r\nHD\r\n");
}

TEST_CASE("meta me dumps entry metadata", "[protocol][meta][me]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 3\r\nfoo\r\nme k\r\n");
    REQUIRE(r.starts_with("STORED\r\nME k "));
    REQUIRE(r.contains("size=3"));
    REQUIRE(r.contains("cas="));
}

TEST_CASE("meta me on miss returns EN", "[protocol][meta][me]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "me nope\r\n");
    REQUIRE(r == "EN\r\n");
}

TEST_CASE("meta protocol interleaves with classic text on the same connection", "[protocol][meta][mixed]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmg k v\r\ndelete k\r\nmg k\r\n");
    REQUIRE(r == "STORED\r\nVA 1\r\nA\r\nDELETED\r\nEN\r\n");
}

TEST_CASE("meta mg pipelining with mn as a sync barrier", "[protocol][meta][mn]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms k 1\r\nA\r\nms k 1\r\nB\r\nms k 1\r\nC\r\nmn\r\nmg k v\r\n");
    REQUIRE(r.contains("MN\r\n"));
    REQUIRE(r.contains("VA 1\r\nC\r\n"));
}

TEST_CASE("meta mg with c and T returns the POST-touch CAS and TTL", "[protocol][meta][mg][touch][regression]")
{
    // Regression: HandleMg used to render the pre-touch snapshot, so a
    // `c`+`T` read-and-refresh returned the stale CAS — a follow-up `cas`
    // would then spuriously fail. The CAS/TTL must reflect the touch this
    // command just applied.
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 30 1\r\nA\r\nmg k c t T120\r\n");
    REQUIRE(r.contains("HD"));
    REQUIRE(r.contains(" c2"));   // CAS bumped by the touch (set=1 -> touch=2)
    REQUIRE(!r.contains(" c1"));  // never the pre-touch CAS
    REQUIRE(r.contains(" t120")); // the refreshed TTL, not the original 30
}

TEST_CASE("meta md C compare-and-delete: mismatch keeps the entry, match removes it", "[protocol][meta][md][cas]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmd k C999\r\nmg k v\r\nmd k C1\r\nmg k\r\n");
    // STORED; EX (CAS mismatch, not deleted); value still readable; HD
    // (matching CAS deletes); EN (gone).
    REQUIRE(r == "STORED\r\nEX\r\nVA 1\r\nA\r\nHD\r\nEN\r\n");
}

TEST_CASE("meta md I marks the entry stale instead of removing it", "[protocol][meta][md][stale]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmd k I\r\nmg k v\r\n");
    // HD acks the mark; the entry survives and a reader sees the X flag.
    REQUIRE(r.contains("HD\r\n"));
    REQUIRE(r.contains("VA 1 X\r\nA\r\n"));
}

TEST_CASE("meta md I T marks stale and refreshes the TTL", "[protocol][meta][md][stale][touch]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 5 1\r\nA\r\nmd k I T120\r\nmg k t v\r\n");
    REQUIRE(r.contains(" t120")); // TTL refreshed by the I+T combo
    REQUIRE(r.contains(" X"));    // and marked stale
}

TEST_CASE("meta ms I marks stale instead of storing the supplied payload", "[protocol][meta][ms][stale]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nms k 1 I\r\nB\r\nmg k v\r\n");
    REQUIRE(r.contains("HD\r\n"));
    // Value is unchanged (still A, not the ignored B) and now stale.
    REQUIRE(r.contains("VA 1 X\r\nA\r\n"));
}

TEST_CASE("meta ms N auto-vivifies an append-mode miss", "[protocol][meta][ms][vivify]")
{
    MetaFixture fix;
    auto const r = Exchange(fix, "ms fresh 3 M=A N60\r\nabc\r\nmg fresh v\r\n");
    REQUIRE(r.contains("HD\r\n"));
    REQUIRE(r.contains("VA 3\r\nabc\r\n"));
}

TEST_CASE("meta ma with T reports the post-touch CAS (regression)", "[protocol][meta][ma][touch][regression]")
{
    // set -> cas1; `ma c T120 c` increments (cas2) then touches (cas3); the `c`
    // flag must advertise the post-touch cas3, not the pre-touch cas2, so a
    // client reusing the advertised CAS does not spuriously fail with EX.
    MetaFixture fix;
    auto const r = Exchange(fix, "set c 0 0 2\r\n10\r\nma c T120 c\r\n");
    REQUIRE(r == "STORED\r\nHD c3\r\n");
}

TEST_CASE("meta ma with T and v reports the post-touch CAS in the VA block (regression)",
          "[protocol][meta][ma][touch][regression]")
{
    // Same fix as the HD path, but exercised through the value-returning
    // (`v`) branch, which renders `c` from a separate code path.
    MetaFixture fix;
    auto const r = Exchange(fix, "set c 0 0 2\r\n10\r\nma c T120 c v\r\n");
    REQUIRE(r == "STORED\r\nVA 2 c3\r\n11\r\n");
}

TEST_CASE("meta ma quiet still reports NF on a miss (regression)", "[protocol][meta][ma][quiet][regression]")
{
    // Quiet suppresses only ma's success; a not-found error must still be sent.
    MetaFixture fix;
    auto const r = Exchange(fix, "ma absent q\r\nmn\r\n");
    REQUIRE(r == "NF\r\nMN\r\n");
}

TEST_CASE("meta ms mark-stale reports the unchanged size, not the discarded payload (regression)",
          "[protocol][meta][ms][stale][regression]")
{
    // `ms ... I` discards the payload; with `s` the response must report the
    // existing 3-byte value size, not the 5-byte request datalen.
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 3\r\nfoo\r\nms k 5 I s\r\nWORLD\r\n");
    REQUIRE(r == "STORED\r\nHD s3\r\n");
}

TEST_CASE("meta ms quiet still reports EX on a failed conditional store (regression)",
          "[protocol][meta][ms][quiet][regression]")
{
    // Quiet suppresses only the success line; an EX (cas mismatch) must still
    // be sent so the client learns its conditional store lost the race.
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nms k 1 C999 q\r\nB\r\nmn\r\n");
    REQUIRE(r == "STORED\r\nEX\r\nMN\r\n");
}

TEST_CASE("meta md quiet still reports EX but suppresses a benign NF (regression)",
          "[protocol][meta][md][quiet][regression]")
{
    MetaFixture fix;
    // CAS mismatch under quiet -> EX still sent.
    auto const mismatch = Exchange(fix, "set k 0 0 1\r\nA\r\nmd k C999 q\r\nmn\r\n");
    REQUIRE(mismatch == "STORED\r\nEX\r\nMN\r\n");

    // Not-found delete under quiet -> suppressed.
    MetaFixture fix2;
    auto const notFound = Exchange(fix2, "md absent q\r\nmn\r\n");
    REQUIRE(notFound == "MN\r\n");
}

TEST_CASE("meta ms append reports the resulting item size, not the request length (regression)",
          "[protocol][meta][ms][regression]")
{
    // `s` must report the stored size; for append that is existing + new, not
    // the request datalen.
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 5\r\nhello\r\nms k 3 M=A s\r\n123\r\n");
    REQUIRE(r == "STORED\r\nHD s8\r\n"); // 5 + 3 = 8, not s3
}

TEST_CASE("meta ms append honours a CAS precondition instead of replacing (regression)",
          "[protocol][meta][ms][cas][regression]")
{
    // `ms key len M=A C<cas>` is a CAS-checked APPEND, not a full replace.
    SECTION("wrong CAS leaves the value untouched")
    {
        MetaFixture fix;
        auto const r = Exchange(fix, "set k 0 0 2\r\nhi\r\nms k 1 M=A C999\r\nX\r\nmg k v\r\n");
        REQUIRE(r == "STORED\r\nEX\r\nVA 2\r\nhi\r\n");
    }
    SECTION("matching CAS appends (does not replace)")
    {
        MetaFixture fix;
        auto const r = Exchange(fix, "set k 0 0 2\r\nhi\r\nms k 1 M=A C1\r\nX\r\nmg k v\r\n");
        REQUIRE(r == "STORED\r\nHD\r\nVA 3\r\nhiX\r\n"); // appended to "hiX", not replaced with "X"
    }
}

TEST_CASE("meta mg with T counts a single get hit, not two (regression)", "[protocol][meta][mg][touch][stats][regression]")
{
    // HandleMg used to Get then GetAndTouch, double-counting get_hits and
    // copying the value twice; now it issues one GetAndTouch.
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmg k v T60\r\nstats\r\n");
    REQUIRE(r.contains("VA 1\r\nA\r\n"));
    REQUIRE(r.contains("STAT get_hits 1\r\n"));
}

TEST_CASE("meta mg with u does not count a get hit (no-bump)", "[protocol][meta][mg][nobump][regression]")
{
    // `u` reads via the non-mutating Peek, so it must not bump get_hits or
    // promote the LRU. (The lastAccess mechanic `l` depends on is covered by
    // InMemoryLruStorage's pre-access-lastAccess test.)
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmg k u\r\nstats\r\n");
    REQUIRE(r.contains("STAT get_hits 0\r\n"));
}

TEST_CASE("meta mg l reports seconds since the previous read (regression)", "[protocol][meta][mg][last-access][regression]")
{
    // engine->Get used to stamp lastAccess before `l` was computed, so `l`
    // was always 0. Drive two reads 10s apart on the same engine. Strict mode
    // so the stored lastAccess advances on every read (the Approximate policy
    // defers that and would report l0 here); the since-previous-read semantics
    // are what this regression guards.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage storage { 0, 0, FastCache::LruMode::Strict };
    FastCache::CacheEngine engine { storage, clock };
    FastCache::MemcachedTextHandler handler;

    auto runOnce = [&](std::string_view req) {
        auto pair = FastCache::InMemorySocketPair::Create();
        REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), req)));
        pair.client->ShutdownWrite();
        FastCache::SyncRun(handler.Run(pair.server.get(), &engine, /*priming*/ {}, /*session*/ {}));
        return FastCache::SyncRun(ReadAvailable(pair.client.get()));
    };

    std::ignore = runOnce("set k 0 0 1\r\nA\r\nmg k\r\n"); // first read stamps lastAccess at T0
    clock.Advance(std::chrono::seconds { 10 });
    auto const r = runOnce("mg k l\r\n"); // read at T0+10 -> l10, not l0
    REQUIRE(r.contains(" l10"));
}

TEST_CASE("meta ma with t does not pollute get hits (regression)", "[protocol][meta][ma][stats][regression]")
{
    // HandleMa re-read the post-touch TTL via Get, inflating get_hits and
    // stamping lastAccess on the counter; it must use a non-mutating Peek.
    MetaFixture fix;
    auto const r = Exchange(fix, "set c 0 30 2\r\n10\r\nma c t v\r\nstats\r\n");
    REQUIRE(r.contains("STAT get_hits 0\r\n"));
}

TEST_CASE("meta mg opaque token starting with '=' round-trips verbatim (regression)",
          "[protocol][meta][mg][opaque][regression]")
{
    // ParseFlag stripped a leading '=' from every flag token; it must do so
    // only for the M mode flag, leaving an opaque `O=abc` intact.
    MetaFixture fix;
    auto const r = Exchange(fix, "set k 0 0 1\r\nA\r\nmg k O=abc\r\n");
    REQUIRE(r.contains(" O=abc"));
}
