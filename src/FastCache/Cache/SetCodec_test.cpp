// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/SetCodec.hpp>
#include <FastCache/Cache/StreamCodec.hpp>
#include <FastCache/Cache/WriteErrorReportingStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

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

/// One engine, reached through BOTH front ends.
///
/// The point of the fixture: a memcached client and a redis client are two protocols
/// over one `CacheEngine`, so bytes a memcached `set` planted are bytes a redis set
/// verb decodes. That sharing is the reachability this file exists to demonstrate, and
/// it is invisible from either protocol's own test file.
struct BothProtocols
{
    ManualClock clock;
    InMemoryLruStorage storage;
    CacheEngine engine { storage, clock };
    MemcachedTextHandler text;
    RedisRespHandler resp;

    /// Run one request through the memcached front end.
    [[nodiscard]] std::string Memcached(std::string_view request)
    {
        return Run(text, request);
    }

    /// Run one request through the redis front end, against the same engine.
    [[nodiscard]] std::string Redis(std::string_view request)
    {
        return Run(resp, request);
    }

  private:
    /// Drive one request/reply cycle on a connection of its own.
    ///
    /// A fresh socket pair per exchange rather than one on the fixture, because each
    /// cycle half-closes and then closes its pair -- the sequence every protocol test
    /// here uses, and for the reason they record: the handlers do not close their own
    /// write side when their loop ends, so without it the drain relies on a
    /// partial-read heuristic that parks on a reply that is an exact multiple of the
    /// chunk size.
    [[nodiscard]] std::string Run(IProtocolHandler& handler, std::string_view request)
    {
        auto pair = InMemorySocketPair::Create();
        REQUIRE(SyncRun(WriteString(pair.client.get(), request)));
        pair.client->ShutdownWrite();
        SyncRun(handler.Run(pair.server.get(), &engine, /*primingBytes=*/ {}, SessionContext {}));
        pair.server->Close();
        return SyncRun(ReadAvailable(pair.client.get()));
    }
};

/// A set blob declaring `count` members and carrying `trailing` bytes for them.
///
/// Built from the codec's own `Magic`/`TypeSet` rather than a hex literal, so it
/// cannot drift from the format it exercises.
[[nodiscard]] std::string SetBlob(std::uint32_t count, std::size_t trailing = 0)
{
    std::string out;
    out.push_back(static_cast<char>(SetCodec::Magic));
    out.push_back(static_cast<char>(SetCodec::TypeSet));
    for (auto const shift: { 24, 16, 8, 0 })
        out.push_back(static_cast<char>((count >> shift) & 0xFFU));
    out.append(trailing, '\0');
    return out;
}

/// Store `blob` under `key` as a SET, exactly the way an ordinary memcached client
/// would -- which is the whole point: no privilege, no special verb.
///
/// The flags word is rendered from `SetCodec::FcTypeSet` rather than typed out. A
/// hand-written decimal drifted once already, and a wrong one would store a plain
/// string and make every assertion below pass for the wrong reason.
void PlantSet(BothProtocols& fix, std::string_view key, std::string_view blob)
{
    auto const command = "set " + std::string { key } + " " + std::to_string(SetCodec::FcTypeSet) + " 0 "
                         + std::to_string(blob.size()) + "\r\n" + std::string { blob } + "\r\n";
    REQUIRE(fix.Memcached(command) == "STORED\r\n");
}

} // namespace

TEST_CASE("A plain memcached client can plant a set blob a redis verb then decodes", "[cache][setcodec][security]")
{
    // Issue #271, and the reachability IS the finding -- so this drives the real path
    // rather than calling the decoder. What tags a value as a set is its `flags` word,
    // and `set <key> <flags> <exptime> <bytes>` lets an ordinary memcached client
    // choose it.
    //
    // No privilege, no fleet membership, no special client: two stock front ends over
    // one engine. Before the guard, the SMEMBERS below reserved 0xFFFFFFFF
    // `std::string` -- roughly 137 GB -- and the `std::bad_alloc` escaped the RESP
    // handler uncaught, which under a 2 GiB address-space cap aborts the process.
    BothProtocols fix;
    PlantSet(fix, "evil", SetBlob(0xFFFFFFFFU));

    // Refused, and refused as a REPLY: the client reads an error on a live connection
    // rather than seeing a reset, which is what `Corrupt` maps to for a set command.
    CHECK(fix.Redis("*2\r\n$8\r\nSMEMBERS\r\n$4\r\nevil\r\n") == "-ERR storage failure\r\n");
}

TEST_CASE("Every redis set verb refuses the planted blob, not just the read ones", "[cache][setcodec][security]")
{
    // `Decode` is reached from the read path (`WithSet`, used by SMEMBERS/SCARD) and
    // from the read-modify-write path (`LoadSet`, used by SADD/SREM). A guard on one
    // would leave the other reserving, so both are asserted.
    SECTION("SCARD, through the read path")
    {
        BothProtocols fix;
        PlantSet(fix, "evil", SetBlob(0xFFFFFFFFU));
        CHECK(fix.Redis("*2\r\n$5\r\nSCARD\r\n$4\r\nevil\r\n") == "-ERR storage failure\r\n");
    }

    SECTION("SADD, through the read-modify-write path")
    {
        BothProtocols fix;
        PlantSet(fix, "evil", SetBlob(0xFFFFFFFFU));
        CHECK(fix.Redis("*3\r\n$4\r\nSADD\r\n$4\r\nevil\r\n$1\r\nx\r\n") == "-ERR storage failure\r\n");
    }
}

TEST_CASE("A set planted through memcached and well-formed still works", "[cache][setcodec]")
{
    // The guard must refuse impossible claims, not sets that merely arrived by an
    // unusual route -- otherwise the cases above would pass for the wrong reason and
    // prove only that the path was broken.
    BothProtocols fix;

    std::vector<std::string> const members { "a", "bb" };
    PlantSet(fix, "good", AsStringView(SetCodec::Encode(members)));
    CHECK(fix.Redis("*2\r\n$5\r\nSCARD\r\n$4\r\ngood\r\n") == ":2\r\n");
}

TEST_CASE("SetCodec::Decode refuses a member count the blob cannot supply", "[cache][setcodec]")
{
    // The decoder's own boundary, from both sides, so the guard is neither off by one
    // nor a number somebody picked. A member costs its four-byte length prefix at
    // minimum, so `n` bytes after the header can carry at most `n / 4` members. No
    // protocol path can express this, which is why it is a case of its own.
    std::vector<std::string> members;
    auto const decode = [&members](std::string const& blob) {
        return SetCodec::Decode(AsBytes(blob), members);
    };

    // The shape that mattered: a huge count against no bytes at all.
    CHECK_FALSE(decode(SetBlob(0xFFFFFFFFU)));

    // Three members cannot fit in eight bytes, and that is decided on the count alone.
    CHECK_FALSE(decode(SetBlob(3, 8)));

    // Two can, and those eight zero bytes really are two members: each a zero length
    // and no text. So the guard admits exactly what the blob can carry rather than
    // capping it at something smaller.
    REQUIRE(decode(SetBlob(2, 8)));
    CHECK(members.size() == 2);
    CHECK(members[0].empty());

    // And an empty set is ordinary.
    REQUIRE(decode(SetBlob(0)));
    CHECK(members.empty());
}

TEST_CASE("SetCodec::Decode refuses a member LENGTH the blob cannot supply", "[cache][setcodec]")
{
    // The count guard is necessary and not sufficient: a count that passes it says
    // nothing about the individual lengths, and the per-member walk is the second
    // bound. These are the inputs that reach it, which the count cases above cannot.
    std::vector<std::string> members;
    auto const decode = [&members](std::string const& blob) {
        return SetCodec::Decode(AsBytes(blob), members);
    };

    // Append a big-endian u32 to `s`.
    auto const appendU32 = [](std::string& s, std::uint32_t v) {
        for (auto const shift: { 24, 16, 8, 0 })
            s.push_back(static_cast<char>((v >> shift) & 0xFFU));
    };

    // One member declaring 0xFFFFFFFF bytes it does not carry. The count is a legal
    // claim (one member fits in the four bytes present), so only the length check can
    // refuse it -- and it must do so by subtracting from the size rather than adding
    // to the offset, which wraps wherever `std::size_t` is 32-bit.
    std::string overlong = SetBlob(1);
    appendU32(overlong, 0xFFFFFFFFU);
    CHECK_FALSE(decode(overlong));
    CHECK(members.empty());

    // A length prefix cut short, which only the SECOND member can reach: the count
    // guard prices every member at four bytes, so a short prefix is possible only
    // once an earlier member has spent more than its minimum. Two members against
    // nine bytes passes the count check (9 / 4 == 2); the first then eats seven of
    // them and leaves two for a four-byte prefix.
    std::string shortPrefix = SetBlob(2);
    appendU32(shortPrefix, 3);
    shortPrefix.append("abc");
    shortPrefix.append(2, '\0');
    CHECK_FALSE(decode(shortPrefix));

    // A member whose text is one byte short of its declared length -- the ordinary
    // truncation, and the shape that commits nothing before it is discovered.
    std::string truncated = SetBlob(1);
    appendU32(truncated, 4);
    truncated.append(3, 'x');
    CHECK_FALSE(decode(truncated));

    // One more byte and it is a well-formed set, so the refusals above are the length
    // check firing rather than the blob being unreachable.
    truncated.push_back('x');
    REQUIRE(decode(truncated));
    CHECK(members == std::vector<std::string> { "xxxx" });
}

TEST_CASE("SetCodec round-trips, and its per-member minimum tracks the encoder", "[cache][setcodec]")
{
    std::vector<std::string> const members { "alpha", "beta", "gamma" };
    std::vector<std::string> back;
    REQUIRE(SetCodec::Decode(SetCodec::Encode(members), back));
    CHECK(back == members);

    // `MinMemberBytes` is a security bound derived by hand from `Encode`'s loop. This
    // pins it to that encoder: a field added there fails here rather than silently
    // leaving `Decode`'s guard weaker than the format it guards.
    std::vector<std::string> const none {};
    std::vector<std::string> const one { "" };
    CHECK(SetCodec::Encode(one).size() - SetCodec::Encode(none).size() == SetCodec::MinMemberBytes);
}

// ---------------------------------------------------------------------------
// #296: a client's malformed value is not a failing disk.
// ---------------------------------------------------------------------------

namespace
{
/// One engine with the two decorators an operator's dashboard is actually fed from.
///
/// `WriteErrorReportingStorage` is what publishes `fastcached_write_errors_total`
/// and writes the "storage write failed" line, and the metrics sink is what
/// publishes the counters. Both are here because the whole of #296 is a claim about
/// what those two SHOW -- asserting the error code alone would leave the operator-
/// visible half untested, and the operator-visible half is the ticket.
struct Observed
{
    ManualClock clock;
    InMemoryLruStorage raw;
    NullLogger logger;
    WriteErrorReportingStorage storage { raw, logger };
    AtomicMetricsSink metrics;
    CacheEngine engine { storage, clock, DefaultSystemWallClock(), &metrics };
    MemcachedTextHandler text;

    /// Plant `blob` under `key` tagged with `flags`, as an ordinary memcached client.
    void Plant(std::string_view key, std::uint32_t flags, std::string_view blob)
    {
        auto const command = "set " + std::string { key } + " " + std::to_string(flags) + " 0 " + std::to_string(blob.size())
                             + "\r\n" + std::string { blob } + "\r\n";
        auto pair = InMemorySocketPair::Create();
        REQUIRE(SyncRun(WriteString(pair.client.get(), command)));
        pair.client->ShutdownWrite();
        SyncRun(text.Run(pair.server.get(), &engine, /*primingBytes=*/ {}, SessionContext {}));
        pair.server->Close();
        REQUIRE(SyncRun(ReadAvailable(pair.client.get())) == "STORED\r\n");
    }

    [[nodiscard]] std::uint64_t Malformed() const
    {
        return metrics.Read(IMetricsSink::Counter::CacheMalformedValues);
    }

    /// What `fastcached_write_errors_total` would export.
    [[nodiscard]] std::uint64_t WriteErrors() const
    {
        return storage.Snapshot().writeErrors;
    }
};
} // namespace

TEST_CASE("(#296) a malformed set from a client is not reported as disk corruption",
          "[cache][setcodec][security][malformed-value]")
{
    // THE TICKET. `SetCodec::Decode` returned `bool` and every caller collapsed it
    // into `StorageErrorCode::Corrupt` -- documented in this tree as "on-disk record
    // failed CRC32C verification". So six bytes from an unprivileged client made the
    // daemon report a failing disk against a store whose every record still verified,
    // and `Corrupt` is what gets a healthy cache deleted by whoever is on call.
    //
    // The blob below is planted the way the reachability case above establishes: an
    // ordinary memcached `set` choosing the flags word that tags a value as a set.
    Observed fix;
    fix.Plant("bad-set", SetCodec::FcTypeSet, SetBlob(/*count=*/4, /*trailing=*/0));

    REQUIRE(fix.Malformed() == 0);
    REQUIRE(fix.WriteErrors() == 0);

    SECTION("the read path")
    {
        auto const members = fix.engine.SetMembers("bad-set");
        REQUIRE_FALSE(members.has_value());

        // The whole ticket, in one line.
        CHECK(members.error().code == StorageErrorCode::MalformedValue);
        CHECK(members.error().code != StorageErrorCode::Corrupt);

        // And it says WHICH claim the bytes did not honour, which `Corrupt` could
        // not: the code was the entire diagnosis and it was the wrong one.
        CHECK(members.error().context.contains("set value blob"));

        CHECK(fix.Malformed() == 1);
    }

    SECTION("the write path, which is the one an operator's dashboard sees")
    {
        std::array<std::string, 1> const add { "member" };
        auto const added = fix.engine.SetAdd("bad-set", add);
        REQUIRE_FALSE(added.has_value());
        CHECK(added.error().code == StorageErrorCode::MalformedValue);
        CHECK(fix.Malformed() == 1);

        // The half that made this priority/high rather than an error-string fix.
        // `MalformedValue` is not a persistence failure, so it moves neither the
        // write-error counter nor the "storage write failed" log line. Before #296
        // this was 1, and an unprivileged client could drive it at will -- the
        // disk-failure signal, on demand, on a healthy store.
        CHECK(fix.WriteErrors() == 0);
    }
}

TEST_CASE("(#296) a malformed STREAM is the same answer, because it was the same bug",
          "[cache][streamcodec][security][malformed-value]")
{
    // `StreamCodec::Decode` had the identical shape -- `bool` in, `Corrupt` chosen by
    // the caller -- and is reachable identically, because a stream is also just a
    // value blob tagged by a flags word a memcached client may choose. A fix that
    // covered only the set codec would be one somebody would reasonably believe
    // covered both.
    Observed fix;

    // Magic and type byte, then nothing: a header that cannot supply the ids the
    // walk immediately reads.
    std::string blob;
    blob.push_back(static_cast<char>(StreamCodec::Magic));
    blob.push_back(static_cast<char>(StreamCodec::TypeStream));
    fix.Plant("bad-stream", StreamCodec::FcTypeStream, blob);

    auto const length = fix.engine.StreamLen("bad-stream");
    REQUIRE_FALSE(length.has_value());
    CHECK(length.error().code == StorageErrorCode::MalformedValue);
    CHECK(length.error().context.contains("stream value blob"));
    CHECK(fix.Malformed() == 1);
    CHECK(fix.WriteErrors() == 0);
}

TEST_CASE("(#296) a well-formed value moves neither counter", "[cache][setcodec][malformed-value]")
{
    // The control. Without it, a "fix" that reported MalformedValue for everything --
    // or that counted on every decode -- would pass both cases above, and the counter
    // an operator is meant to alert on would be noise from the first request.
    Observed fix;
    std::vector<std::string> const members { "alpha", "beta" };
    fix.Plant("good-set", SetCodec::FcTypeSet, AsStringView(SetCodec::Encode(members)));

    auto const read = fix.engine.SetMembers("good-set");
    REQUIRE(read.has_value());
    CHECK(*read == members);
    CHECK(fix.Malformed() == 0);
    CHECK(fix.WriteErrors() == 0);
}
