// SPDX-License-Identifier: Apache-2.0
//
// The live-stats reading: captured once, rendered as `/metrics` text or encoded as binary, and
// decoded by a client into the same struct (#1399). What these cases hold is that the two
// encodings cannot disagree and that a client of another layout refuses rather than misreads.
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A sink that cannot carry one catalogue row, the in-process shape of #1353's build skew.
class SkewedSink final: public IMetricsSink
{
  public:
    explicit SkewedSink(Counter missing) noexcept:
        _missing { missing }
    {
    }

    void Increment(Counter counter, std::uint64_t by = 1) noexcept override
    {
        _inner.Increment(counter, by);
    }

    [[nodiscard]] std::uint64_t Read(Counter counter) const noexcept override
    {
        return _inner.Read(counter);
    }

    [[nodiscard]] bool Carries(Counter counter) const noexcept override
    {
        return counter != _missing;
    }

  private:
    AtomicMetricsSink _inner;
    Counter _missing;
};

/// Every counter a distinct, wide value, so a figure decoded into its neighbour's slot differs.
void GiveEveryCounterItsOwnValue(IMetricsSink& sink)
{
    auto value = std::uint64_t { 1 };
    for (auto const& row: CounterTable)
        sink.Increment(row.counter, (value++ * 1'000'003) + 17);
}

/// Every block present, every field distinct, one tier absent: nothing below passes by
/// round-tripping a default.
[[nodiscard]] MetricsSnapshot EveryBlockPresent()
{
    auto stats = [](std::uint64_t base) {
        StorageStats s;
        auto next = base;
        for (auto const& field: StatsReadingWire::StorageSizeFields)
            s.*field.member = static_cast<std::size_t>(next++);
        for (auto const& field: StatsReadingWire::StorageCounterFields)
            s.*field.member = next++;
        return s;
    };
    TieredStorageStats tiers {};
    tiers[0] = stats(1000);
    return MetricsSnapshot { .storage = stats(100),
                             .storageTiers = tiers,
                             .host = HostCapacity { .logicalCores = 32,
                                                    .configuredSlots = 30,
                                                    .totalMemoryBytes = 68'719'476'736,
                                                    .diskCapacityBytes = 2'000'398'934'016,
                                                    .diskFreeBytes = 442'381'631'488,
                                                    .busySlots = 7 },
                             .upstreamConfigured = false,
                             .consensus = ConsensusStatus { .members = { "node-a1", "node-b2", "" },
                                                            .knownLeader = Consensus::NodeId { "node-b2" },
                                                            .term = Consensus::Term { .value = 41 },
                                                            .commitIndex = Consensus::LogIndex { .value = 918'273 },
                                                            .role = Consensus::Role::PreCandidate },
                             .uptime = Uptime { 864'017s } };
}

[[nodiscard]] StatsReading RichReading()
{
    SkewedSink sink { IMetricsSink::Counter::ConnectionsAdmissionRejected };
    GiveEveryCounterItsOwnValue(sink);
    return CaptureStatsReading(sink, EveryBlockPresent());
}

} // namespace

TEST_CASE("Capturing a reading asks the sink whether it carries each row", "[metrics][livestats]")
{
    // #1353 in the model: a row this build's sink has no slot for is ABSENT in the reading, and
    // every other row holds exactly what the sink reads.
    auto const missing = IMetricsSink::Counter::ConnectionsAdmissionRejected;
    SkewedSink sink { missing };
    GiveEveryCounterItsOwnValue(sink);

    auto const reading = CaptureStatsReading(sink, EveryBlockPresent());

    for (auto const& row: CounterTable)
    {
        auto const& captured = reading.counters[static_cast<std::size_t>(row.counter)];
        INFO(row.prometheusName);
        if (row.counter == missing)
            CHECK_FALSE(captured.has_value());
        else
            CHECK(captured == std::optional { sink.Read(row.counter) });
    }
    CHECK(reading.snapshot == EveryBlockPresent());
}

TEST_CASE("A reading survives the binary form unchanged", "[metrics][livestats]")
{
    auto const reading = RichReading();
    auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == reading);
}

TEST_CASE("An absent block survives the binary form as absent rather than as zero", "[metrics][livestats]")
{
    // The empty reading is the other half of the round trip: every optional block absent and
    // every counter absent. A codec that wrote zeroes for absences would decode a cache with
    // no tiers into a cache whose tiers hold nothing, and `/metrics` would then say so.
    StatsReading const nothing {};
    auto const decoded = DecodeStatsReading(EncodeStatsReading(nothing));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == nothing);
    CHECK(std::ranges::none_of(decoded->counters, [](auto const& value) { return value.has_value(); }));
    CHECK_FALSE(decoded->snapshot.storage.has_value());
    CHECK_FALSE(decoded->snapshot.consensus.has_value());
}

TEST_CASE("A decoded reading renders the metrics body the node itself serves", "[metrics][livestats]")
{
    // The property that makes one model worth having: a client holding the decoded struct and
    // the node's own scrape describe the same state, figure for figure, including the skewed
    // row the scrape names and does not render.
    auto const reading = RichReading();
    auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
    REQUIRE(decoded.has_value());

    auto const served = RenderPrometheus(reading);
    CHECK(RenderPrometheus(*decoded) == served);
    auto const* const skewed = DescriptorOf(IMetricsSink::Counter::ConnectionsAdmissionRejected);
    REQUIRE(skewed != nullptr);
    CHECK(served.contains(std::format("# SKEW {} is", skewed->prometheusName)));
}

TEST_CASE("A reading laid out by another build is refused by name", "[metrics][livestats]")
{
    auto bytes = EncodeStatsReading(RichReading());
    REQUIRE(DeclaredStatsReadingLayout(bytes) == std::optional { StatsReadingLayout });

    // The control first: the untouched bytes decode.
    REQUIRE(DecodeStatsReading(bytes).has_value());

    bytes[7] ^= std::byte { 0x01 };
    auto const refused = DecodeStatsReading(bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == StatsReadingFault::ForeignLayout);
}

TEST_CASE("The layout digest moves when a catalogue row moves", "[metrics][livestats]")
{
    // Position is the whole identity of a counter on the wire, so an insertion, a removal or a
    // swap between two builds must change the digest they compare. Asked of the digest function
    // over a copy of the catalogue, so the case proves the MECHANISM without editing the tree.
    auto shifted = CounterTable;
    std::swap(shifted[3], shifted[4]);
    CHECK(StatsReadingWire::LayoutDigest(shifted) != StatsReadingLayout);

    auto renamed = CounterTable;
    renamed[0].prometheusName = "fastcached_a_series_this_build_does_not_have";
    CHECK(StatsReadingWire::LayoutDigest(renamed) != StatsReadingLayout);

    // And the control: the catalogue itself digests to the published layout.
    CHECK(StatsReadingWire::LayoutDigest(CounterTable) == StatsReadingLayout);
}

TEST_CASE("This build's live-stats layout is the pinned one", "[metrics][livestats]")
{
    // The digest follows the tables by itself, so the wire stays safe without this pin. What the
    // pin adds is that a layout change is SEEN: adding, moving or renaming a counter, a storage
    // or host field, a tier or a role changes what every live-stats client can read, and a
    // client built before the change will refuse this node. Update the constant in the same
    // change, and say in its message that clients and nodes upgrade together.
    INFO(std::format("StatsReadingLayout is 0x{:016x}", StatsReadingLayout));
    CHECK(StatsReadingLayout == 0x556228629f723481ULL);
}

TEST_CASE("A truncated or padded reading is refused and never half-read", "[metrics][livestats]")
{
    auto const bytes = EncodeStatsReading(RichReading());

    for (auto const length: std::views::iota(std::size_t { 0 }, bytes.size()))
    {
        auto const refused = DecodeStatsReading(std::span { bytes }.first(length));
        INFO("prefix of " << length << " of " << bytes.size() << " bytes");
        CHECK_FALSE(refused.has_value());
    }

    auto padded = bytes;
    padded.push_back(std::byte { 0 });
    auto const trailing = DecodeStatsReading(padded);
    REQUIRE_FALSE(trailing.has_value());
    CHECK(trailing.error() == StatsReadingFault::TrailingBytes);
}
