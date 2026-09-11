// SPDX-License-Identifier: Apache-2.0
/// Decode cost for the stream value blob, which is what
/// [#310](https://github.com/LASTRADA-Software/fastcached/issues/310) is a claim about.
///
/// #309 deleted `StreamCodec`'s five `reserve(count)` calls rather than tightening
/// them, and reported the cost as 34-47% on decoding large honest streams with short
/// SSO field strings. That figure is a quantity UNDER CONDITIONS taken once, on a tree
/// that has moved since, so it is re-derived here rather than cited -- and this
/// benchmark is what any replacement bound has to be argued against.
///
/// **It measures the shipped `Decode`, not a stand-in.** The blob comes from the real
/// `Encode`, so a change to either side moves this number; a hand-rolled byte string
/// would keep reporting after the format moved under it.
///
/// The shape is the ticket's: MANY small elements, because the cost it describes is
/// repeated geometric growth at small sizes rather than the final capacity. Field
/// strings are deliberately inside libstdc++'s 15-char SSO buffer, which is what makes
/// the in-memory cost several times the wire cost -- a 4-byte wire field becoming a
/// 32-byte `std::string` is the amplification that stops `reserve(count)` from being
/// the free answer even though #309 made `count` safe.

#include <FastCache/Cache/StreamCodec.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <vector>

using namespace FastCache;

namespace
{

/// How many entries the decoded stream carries.
///
/// Large enough that geometric growth dominates the fixed cost of the header walk,
/// which is the effect under measurement; small enough that the blob stays a
/// realistic stored value rather than a stress case nobody has.
constexpr std::uint32_t BenchEntries = 2000;

/// Fields per entry, and consumers per group.
constexpr std::uint32_t BenchFieldsPerEntry = 4;

/// A stream of `BenchEntries` entries, every string short enough to live in SSO.
///
/// SSO is the point rather than an incidental: the amplification #310 weighs is a
/// short wire field becoming a `std::string` whose inline buffer is far larger, so a
/// fixture with long strings would measure heap traffic instead and report a smaller
/// regression for the wrong reason.
[[nodiscard]] StreamCodec::Stream MakeStream()
{
    StreamCodec::Stream stream;
    stream.entriesAdded = BenchEntries;
    stream.lastId = StreamCodec::StreamId { .ms = BenchEntries, .seq = 0 };
    for (auto i = std::uint32_t { 0 }; i < BenchEntries; ++i)
    {
        StreamCodec::StreamEntry entry;
        entry.id = StreamCodec::StreamId { .ms = i, .seq = 0 };
        for (auto f = std::uint32_t { 0 }; f < BenchFieldsPerEntry; ++f)
            entry.fields.emplace_back(std::format("f{}", f), std::format("v{}", f));
        stream.entries.push_back(std::move(entry));
    }
    return stream;
}

} // namespace

TEST_CASE("bench: StreamCodec::Decode over a large honest stream", "[!benchmark][stream][codec]")
{
    auto const blob = StreamCodec::Encode(MakeStream());

    // The fixture is asserted rather than assumed: a benchmark over a blob that does
    // not decode measures the refusal path and reports a confident, meaningless
    // number. This is the positive control for everything below it.
    StreamCodec::Stream check;
    REQUIRE(StreamCodec::Decode(blob, check).has_value());
    REQUIRE(check.entries.size() == BenchEntries);

    std::cout << std::format(
        "stream bench: {} entries x {} fields, blob {} bytes\n", BenchEntries, BenchFieldsPerEntry, blob.size());

    BENCHMARK("Decode")
    {
        StreamCodec::Stream out;
        auto const ok = StreamCodec::Decode(blob, out);
        return ok.has_value() ? out.entries.size() : std::size_t { 0 };
    };
}
