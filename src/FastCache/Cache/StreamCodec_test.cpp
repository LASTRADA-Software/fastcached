// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StreamCodec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using FastCache::StreamCodec::ConsumerGroup;
using FastCache::StreamCodec::PendingEntry;
using FastCache::StreamCodec::Stream;
using FastCache::StreamCodec::StreamEntry;
using FastCache::StreamCodec::StreamId;

namespace
{

[[nodiscard]] Stream SampleStream()
{
    Stream s;
    s.lastId = StreamId { .ms = 100, .seq = 2 };
    s.maxDeletedId = StreamId { .ms = 50, .seq = 0 };
    s.entriesAdded = 5;
    s.entries.push_back(
        StreamEntry { .id = StreamId { .ms = 99, .seq = 0 }, .fields = { { "field1", "value1" }, { "f2", "" } } });
    s.entries.push_back(StreamEntry { .id = StreamId { .ms = 100, .seq = 2 }, .fields = { { "k", "v" } } });
    ConsumerGroup g;
    g.name = "g1";
    g.lastDelivered = StreamId { .ms = 99, .seq = 0 };
    g.entriesRead = 1;
    g.consumers = { "alice", "bob" };
    g.pel.push_back(PendingEntry {
        .id = StreamId { .ms = 99, .seq = 0 }, .consumer = "alice", .deliveryTimeMs = 12345, .deliveryCount = 3 });
    s.groups.push_back(std::move(g));
    return s;
}

} // namespace

TEST_CASE("StreamCodec: round-trips a populated stream", "[cache][stream]")
{
    auto const original = SampleStream();
    auto const blob = StreamCodec::Encode(original);

    Stream decoded;
    REQUIRE(StreamCodec::Decode(std::span<std::byte const> { blob }, decoded));

    REQUIRE(decoded.lastId == original.lastId);
    REQUIRE(decoded.maxDeletedId == original.maxDeletedId);
    REQUIRE(decoded.entriesAdded == original.entriesAdded);
    REQUIRE(decoded.entries.size() == 2);
    REQUIRE(decoded.entries[0].id == StreamId { .ms = 99, .seq = 0 });
    REQUIRE(decoded.entries[0].fields.size() == 2);
    REQUIRE(decoded.entries[0].fields[0] == std::pair<std::string, std::string> { "field1", "value1" });
    REQUIRE(decoded.entries[0].fields[1] == std::pair<std::string, std::string> { "f2", "" });
    REQUIRE(decoded.entries[1].fields[0] == std::pair<std::string, std::string> { "k", "v" });

    REQUIRE(decoded.groups.size() == 1);
    auto const& g = decoded.groups[0];
    REQUIRE(g.name == "g1");
    REQUIRE(g.lastDelivered == StreamId { .ms = 99, .seq = 0 });
    REQUIRE(g.entriesRead == 1);
    REQUIRE(g.consumers == std::vector<std::string> { "alice", "bob" });
    REQUIRE(g.pel.size() == 1);
    REQUIRE(g.pel[0].id == StreamId { .ms = 99, .seq = 0 });
    REQUIRE(g.pel[0].consumer == "alice");
    REQUIRE(g.pel[0].deliveryTimeMs == 12345);
    REQUIRE(g.pel[0].deliveryCount == 3);
}

TEST_CASE("StreamCodec: round-trips an empty stream", "[cache][stream]")
{
    Stream const empty;
    auto const blob = StreamCodec::Encode(empty);
    Stream decoded;
    REQUIRE(StreamCodec::Decode(std::span<std::byte const> { blob }, decoded));
    REQUIRE(decoded.entries.empty());
    REQUIRE(decoded.groups.empty());
    REQUIRE(decoded.entriesAdded == 0);
}

TEST_CASE("StreamCodec: rejects a foreign / corrupt blob", "[cache][stream]")
{
    Stream decoded;

    SECTION("empty input")
    {
        REQUIRE_FALSE(StreamCodec::Decode({}, decoded));
    }
    SECTION("wrong magic")
    {
        std::vector<std::byte> bad { std::byte { 0x00 }, StreamCodec::TypeStream };
        REQUIRE_FALSE(StreamCodec::Decode(std::span<std::byte const> { bad }, decoded));
    }
    SECTION("wrong type tag (a set blob)")
    {
        std::vector<std::byte> bad { StreamCodec::Magic, std::byte { 0x01 } };
        REQUIRE_FALSE(StreamCodec::Decode(std::span<std::byte const> { bad }, decoded));
    }
    SECTION("truncated mid-entry")
    {
        auto blob = StreamCodec::Encode(SampleStream());
        blob.resize(blob.size() - 4);
        REQUIRE_FALSE(StreamCodec::Decode(std::span<std::byte const> { blob }, decoded));
    }
    SECTION("huge entry count on a tiny blob fails cleanly without a giant allocation")
    {
        // magic, type, lastId(16), maxDeletedId(16), entriesAdded(8), then a
        // bogus entryCount of 0xFFFFFFFF with no entry bytes following. The
        // decoder must clamp the reserve to the remaining bytes and return false
        // on the missing entry data, not attempt a ~100+GB allocation.
        std::vector<std::byte> blob;
        blob.push_back(StreamCodec::Magic);
        blob.push_back(StreamCodec::TypeStream);
        blob.insert(blob.end(), 40, std::byte { 0 }); // lastId + maxDeletedId + entriesAdded
        for (int i = 0; i < 4; ++i)
            blob.push_back(std::byte { 0xFF }); // entryCount = 0xFFFFFFFF
        REQUIRE_FALSE(StreamCodec::Decode(std::span<std::byte const> { blob }, decoded));
    }
}

TEST_CASE("StreamCodec: IsStream tag discriminates the type", "[cache][stream]")
{
    REQUIRE(StreamCodec::IsStream(StreamCodec::FcTypeStream));
    REQUIRE_FALSE(StreamCodec::IsStream(0));
    REQUIRE_FALSE(StreamCodec::IsStream(0x5E700001U)); // FcTypeSet
}

TEST_CASE("StreamCodec: StreamId formats and compares", "[cache][stream]")
{
    REQUIRE(StreamId { .ms = 100, .seq = 5 }.Format() == "100-5");
    REQUIRE(StreamId { .ms = 0, .seq = 0 }.Format() == "0-0");
    REQUIRE(StreamId { .ms = 1, .seq = 0 } < StreamId { .ms = 1, .seq = 1 });
    REQUIRE(StreamId { .ms = 1, .seq = 9 } < StreamId { .ms = 2, .seq = 0 });
    REQUIRE(StreamId::Min() < StreamId::Max());
}

TEST_CASE("StreamCodec: StreamId::Next steps the sequence then the ms", "[cache][stream]")
{
    REQUIRE(StreamId { .ms = 5, .seq = 0 }.Next() == StreamId { .ms = 5, .seq = 1 });
    auto const maxSeq = StreamId { .ms = 5, .seq = ~std::uint64_t { 0 } };
    REQUIRE(maxSeq.Next() == StreamId { .ms = 6, .seq = 0 });
    REQUIRE(StreamId::Max().Next() == StreamId::Max()); // saturates
}

TEST_CASE("StreamCodec: ParseId accepts ms and ms-seq, rejects junk", "[cache][stream]")
{
    REQUIRE(StreamCodec::ParseId("100-5") == StreamId { .ms = 100, .seq = 5 });
    REQUIRE(StreamCodec::ParseId("100") == StreamId { .ms = 100, .seq = 0 });
    REQUIRE(StreamCodec::ParseId("100", /*seqDefault*/ ~std::uint64_t { 0 })
            == StreamId { .ms = 100, .seq = ~std::uint64_t { 0 } });
    REQUIRE_FALSE(StreamCodec::ParseId("").has_value());
    REQUIRE_FALSE(StreamCodec::ParseId("abc").has_value());
    REQUIRE_FALSE(StreamCodec::ParseId("1-").has_value());
    REQUIRE_FALSE(StreamCodec::ParseId("1-2-3").has_value());
    REQUIRE_FALSE(StreamCodec::ParseId("-5").has_value());
}

namespace
{

/// Assembles a stream blob field by field, so a case can stop at any declared count.
///
/// Built from the codec's own `Magic`/`TypeStream` and its big-endian convention
/// rather than a hex literal, so it cannot drift from the format it exercises.
class BlobBuilder
{
  public:
    BlobBuilder()
    {
        _out.push_back(StreamCodec::Magic);
        _out.push_back(StreamCodec::TypeStream);
    }

    BlobBuilder& U32(std::uint32_t v)
    {
        for (auto const shift: { 24, 16, 8, 0 })
            _out.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
        return *this;
    }

    BlobBuilder& U64(std::uint64_t v)
    {
        for (auto const shift: { 56, 48, 40, 32, 24, 16, 8, 0 })
            _out.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
        return *this;
    }

    /// A stream id: two u64s, as `AppendId` writes it.
    BlobBuilder& Id()
    {
        return U64(0).U64(0);
    }

    /// A length-prefixed empty string, the cheapest a field can be on the wire.
    BlobBuilder& EmptyField()
    {
        return U32(0);
    }

    /// Everything before the entry count: the two ids and the entries-added counter.
    BlobBuilder& StreamHeader()
    {
        return Id().Id().U64(0);
    }

    BlobBuilder& Pad(std::size_t n)
    {
        _out.insert(_out.end(), n, std::byte { 0 });
        return *this;
    }

    [[nodiscard]] std::span<std::byte const> Bytes() const
    {
        return _out;
    }

  private:
    std::vector<std::byte> _out;
};

/// The largest count the field can hold -- the shape every instance of this defect
/// class was reported with.
constexpr std::uint32_t Impossible = 0xFFFFFFFFU;

/// Trailing bytes to leave after a hostile count.
///
/// Not zero, and that is the point: the old clamp was `min(count, remainingBytes)`,
/// so with nothing trailing it clamped to zero and looked like a refusal. It is the
/// bytes being PRESENT that exposes the one-byte-per-element assumption -- forty
/// trailing bytes bought forty reserved elements where the format can hold at most
/// two.
constexpr std::size_t TrailingBytes = 40;

} // namespace

TEST_CASE("StreamCodec: every declared count is refused when the blob cannot supply it", "[cache][stream][security]")
{
    // Issue #269. All five counts went through `BoundedReserve`, which CLAMPED rather
    // than refusing and assumed one byte per element -- so a blob with ten bytes left
    // and a 0xFFFFFFFF count still reserved ten elements and only failed afterwards,
    // inside the loop, on bytes that were never there.
    //
    // Reachable the same way `SetCodec` is: `FcTypeStream` is a flags word an ordinary
    // memcached `set` can choose, after which any stream verb decodes the value.
    //
    // One section per site, because a guard added to four of five leaves the fifth
    // reserving and every one of these would still pass.
    //
    // `Decode` returned false BEFORE this change too -- the clamped reserve happened
    // and then the loop failed on the missing bytes -- so a bare `CHECK_FALSE` proves
    // nothing about the defect. What distinguishes refusing from reserving-then-failing
    // is whether the reserve happened at all, and for the two counts that size a vector
    // the CALLER owns, that is observable: `Decode` starts with `out = Stream {}`, so a
    // non-zero capacity afterwards can only have come from the reserve.
    //
    // The other three size vectors local to the loop, which are discarded on failure
    // and cannot be observed from outside without an allocator hook. What pins those is
    // that all five now go through the same `ReadCount` call, plus the minimums case
    // below -- and the boundary case, which catches a missing guard at the entry count
    // by a decoded entry that should not exist.
    Stream out;

    SECTION("the entry count")
    {
        auto const blob = BlobBuilder {}.StreamHeader().U32(Impossible).Pad(TrailingBytes);
        CHECK_FALSE(StreamCodec::Decode(blob.Bytes(), out));
        CHECK(out.entries.capacity() == 0); // refused, not reserved-then-failed
    }

    SECTION("an entry's field count")
    {
        auto const blob = BlobBuilder {}.StreamHeader().U32(1).Id().U32(Impossible);
        CHECK_FALSE(StreamCodec::Decode(blob.Bytes(), out));
    }

    SECTION("the group count")
    {
        auto const blob = BlobBuilder {}.StreamHeader().U32(0).U32(Impossible).Pad(TrailingBytes);
        CHECK_FALSE(StreamCodec::Decode(blob.Bytes(), out));
        CHECK(out.groups.capacity() == 0); // refused, not reserved-then-failed
    }

    SECTION("a group's consumer count")
    {
        // The trailing padding is what makes this section reach the site it names, and
        // it is exactly the pel-count field this blob stops short of: a group's minimum
        // counts BOTH of its counts, so without those four bytes the GROUP count is
        // refused first and the consumer-count guard is never consulted -- delete that
        // guard and the case still goes green.
        auto const blob = BlobBuilder {}
                              .StreamHeader()
                              .U32(0)
                              .U32(1)
                              .EmptyField()
                              .Id()
                              .U64(0)
                              .U32(Impossible)
                              .Pad(StreamCodec::detail::CountBytes);
        CHECK_FALSE(StreamCodec::Decode(blob.Bytes(), out));
    }

    SECTION("a group's pending-entry count")
    {
        auto const blob = BlobBuilder {}.StreamHeader().U32(0).U32(1).EmptyField().Id().U64(0).U32(0).U32(Impossible);
        CHECK_FALSE(StreamCodec::Decode(blob.Bytes(), out));
    }
}

TEST_CASE("StreamCodec: a declared count is bounded by the bytes actually left", "[cache][stream]")
{
    // The boundary, so each guard is neither off by one nor a number somebody picked.
    // An entry costs twenty wire bytes at minimum -- its id and its field count -- so
    // twenty trailing bytes can carry exactly one.
    Stream out;

    // One entry is achievable, and those twenty zero bytes really are one: a zero id
    // and a zero field count. The blob then ends before the group count, so the decode
    // still fails -- but on TRUNCATION, further in, not on the claim.
    auto const oneFits = BlobBuilder {}.StreamHeader().U32(1).Pad(StreamCodec::detail::MinEntryBytes);
    CHECK_FALSE(StreamCodec::Decode(oneFits.Bytes(), out));
    CHECK(out.entries.size() == 1); // it got past the count and decoded the entry

    // Two cannot fit in the same twenty bytes, and that is decided on the count alone
    // -- so nothing is decoded at all.
    auto const twoDoNot = BlobBuilder {}.StreamHeader().U32(2).Pad(StreamCodec::detail::MinEntryBytes);
    CHECK_FALSE(StreamCodec::Decode(twoDoNot.Bytes(), out));
    CHECK(out.entries.empty());
}

TEST_CASE("StreamCodec: the per-element minimums track the encoder", "[cache][stream]")
{
    // Each minimum is a security bound derived by hand from `Encode`. This pins every
    // one of them to that encoder: the cost of one EMPTY element of each kind is
    // measured from the encoder itself, so a field added there fails here rather than
    // silently leaving a guard weaker than the format it guards.
    auto const sizeOf = [](Stream const& s) {
        return StreamCodec::Encode(s).size();
    };

    Stream base;
    Stream withEntry = base;
    withEntry.entries.push_back(StreamEntry {});
    CHECK(sizeOf(withEntry) - sizeOf(base) == StreamCodec::detail::MinEntryBytes);

    Stream withField = withEntry;
    withField.entries[0].fields.emplace_back("", "");
    CHECK(sizeOf(withField) - sizeOf(withEntry) == StreamCodec::detail::MinFieldBytes);

    Stream withGroup = base;
    withGroup.groups.push_back(ConsumerGroup {});
    CHECK(sizeOf(withGroup) - sizeOf(base) == StreamCodec::detail::MinGroupBytes);

    Stream withConsumer = withGroup;
    withConsumer.groups[0].consumers.emplace_back("");
    CHECK(sizeOf(withConsumer) - sizeOf(withGroup) == StreamCodec::detail::MinConsumerBytes);

    Stream withPending = withGroup;
    withPending.groups[0].pel.push_back(PendingEntry {});
    CHECK(sizeOf(withPending) - sizeOf(withGroup) == StreamCodec::detail::MinPendingBytes);
}
