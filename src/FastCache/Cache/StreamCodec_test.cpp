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
