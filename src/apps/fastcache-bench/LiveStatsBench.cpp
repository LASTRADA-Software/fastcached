// SPDX-License-Identifier: Apache-2.0
/// What one live-stats tick costs on the wire and at each end, text against binary
/// ([#1399](https://github.com/LASTRADA-Software/fastcached/issues/1399)).
///
/// A dashboard that polled rendered `/metrics` text every interval; a subscription pushes the
/// same `StatsReading` as positional binary. This is the record of what that trade buys, under
/// stated conditions rather than as a figure anybody should cite without them.
///
/// **It measures the shipped encoders, not stand-ins**: `RenderPrometheus` is what `/metrics`
/// serves and `EncodeStatsReading` what a snapshot carries, over one reading, so a change to
/// either moves the number. The frame is the whole push, header and snapshot fields included.
///
/// **Bytes depend on the VALUES as well as the layout**: text spells every digit, binary does
/// not. So each subject is reported twice -- a process that has just started, every counter at
/// zero, and one that has served for months, every counter ten digits long -- and the conditions
/// are part of every line printed.

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

namespace Wire = CompileCacheWire;

/// How long a process has been serving, and so how large its figures have grown.
struct Regime
{
    std::string_view name;   ///< As a line names it.
    std::uint64_t counter;   ///< What every counter reads, before its row's offset.
    std::uint64_t storage;   ///< What every storage figure reads, before its field's offset.
    std::chrono::seconds up; ///< The uptime.
};

/// Fresh: every figure zero. Busy: ten-digit counters, a cache in the tens of gigabytes.
constexpr std::array Regimes {
    Regime { .name = "fresh", .counter = 0, .storage = 0, .up = 0s },
    Regime { .name = "busy", .counter = 3'141'592'653, .storage = 27'182'818'284, .up = 7'776'000s },
};

/// Every storage figure of one tier or of the whole cache, at @p regime's magnitude.
/// @param regime The magnitude.
/// @return The figures, each distinct.
[[nodiscard]] StorageStats StorageAt(Regime const& regime)
{
    auto stats = StorageStats {};
    auto offset = std::uint64_t { 0 };
    for (auto const& field: StatsReadingWire::StorageSizeFields)
        stats.*field.member = static_cast<std::size_t>(regime.storage + (regime.storage == 0 ? 0 : offset++));
    for (auto const& field: StatsReadingWire::StorageCounterFields)
        stats.*field.member = regime.counter + (regime.counter == 0 ? 0 : offset++);
    return stats;
}

/// A reading of every catalogue counter at @p regime's magnitude, over @p snapshot.
/// @param regime The magnitude.
/// @param snapshot The blocks the subject carries.
/// @return The reading.
[[nodiscard]] StatsReading ReadingAt(Regime const& regime, MetricsSnapshot snapshot)
{
    AtomicMetricsSink sink;
    auto offset = std::uint64_t { 0 };
    for (auto const& row: CounterTable)
        sink.Increment(row.counter, regime.counter + (regime.counter == 0 ? 0 : offset++));
    return CaptureStatsReading(sink, snapshot);
}

/// A cache daemon run with `--storage`: the merged cache and both tiers, no host, no consensus.
/// @param regime The magnitude.
/// @return Its snapshot.
[[nodiscard]] MetricsSnapshot CacheDaemonAt(Regime const& regime)
{
    auto tiers = TieredStorageStats {};
    tiers[static_cast<std::size_t>(StorageTier::Memory)] = StorageAt(regime);
    tiers[static_cast<std::size_t>(StorageTier::Disk)] = StorageAt(regime);
    return MetricsSnapshot { .storage = StorageAt(regime),
                             .storageTiers = tiers,
                             .host = std::nullopt,
                             .upstreamConfigured = false,
                             .consensus = std::nullopt,
                             .uptime = Uptime { regime.up } };
}

/// A compile node with a two-tier cache, its machine, and a three-member cluster.
/// @param regime The magnitude.
/// @return Its snapshot.
[[nodiscard]] MetricsSnapshot CompileNodeAt(Regime const& regime)
{
    auto snapshot = CacheDaemonAt(regime);
    snapshot.host = HostCapacity { .logicalCores = 32,
                                   .configuredSlots = 30,
                                   .totalMemoryBytes = 68'719'476'736,
                                   .diskCapacityBytes = 2'000'398'934'016,
                                   .diskFreeBytes = 442'381'631'488,
                                   .busySlots = 7 };
    snapshot.upstreamConfigured = true;
    snapshot.consensus = ConsensusStatus { .members = { "build-01", "build-02", "build-03" },
                                           .knownLeader = Consensus::NodeId { "build-01" },
                                           .term = Consensus::Term { .value = 41 },
                                           .commitIndex = Consensus::LogIndex { .value = 918'273 },
                                           .role = Consensus::Role::Follower };
    return snapshot;
}

/// One subject's snapshot builder.
struct Subject
{
    std::string_view name;                ///< As a line names it.
    MetricsSnapshot (*at)(Regime const&); ///< Its blocks at a magnitude.
};

constexpr std::array Subjects {
    Subject { .name = "cache daemon (--storage)", .at = &CacheDaemonAt },
    Subject { .name = "compile node (tiers, host, cluster)", .at = &CompileNodeAt },
};

/// The bytes one tick puts on the wire for @p reading, text and binary.
struct TickBytes
{
    std::size_t text { 0 };      ///< The `/metrics` body.
    std::size_t binary { 0 };    ///< The `EncodeStatsReading` field.
    std::size_t pushFrame { 0 }; ///< The whole push carrying the binary: reply header, kind, tick, body.
};

/// @param reading One reading.
/// @return What a tick of it costs.
[[nodiscard]] TickBytes Measure(StatsReading const& reading)
{
    auto const text = RenderPrometheus(reading);
    auto const binary = EncodeStatsReading(reading);
    auto const frame = Wire::EncodeReply(Wire::Status::Push, Wire::EncodeLiveSnapshot(1, binary));
    return TickBytes { .text = text.size(), .binary = binary.size(), .pushFrame = frame.size() };
}

} // namespace

TEST_CASE("bench: one live-stats tick, text against binary", "[!benchmark][livestats]")
{
    std::cout << std::format(
        "live-stats bench: {} catalogue counters, layout {:#018x}\n", CounterTable.size(), StatsReadingLayout);
    std::cout
        << "| subject | regime | /metrics text (bytes) | binary reading (bytes) | push frame (bytes) | text / frame |\n"
        << "|---|---|---:|---:|---:|---:|\n";
    for (auto const& subject: Subjects)
        for (auto const& regime: Regimes)
        {
            auto const reading = ReadingAt(regime, subject.at(regime));
            // The positive control: a fixture that does not round-trip measures the refusal path.
            auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
            REQUIRE(decoded.has_value());
            REQUIRE(*decoded == reading);
            auto const bytes = Measure(reading);
            std::cout << std::format("| {} | {} | {} | {} | {} | {:.1f}x |\n",
                                     subject.name,
                                     regime.name,
                                     bytes.text,
                                     bytes.binary,
                                     bytes.pushFrame,
                                     static_cast<double>(bytes.text) / static_cast<double>(bytes.pushFrame));
        }

    // Timed over the busier of the two, the compile node: the one a dashboard watches longest.
    auto const reading = ReadingAt(Regimes.back(), CompileNodeAt(Regimes.back()));
    auto const encoded = EncodeStatsReading(reading);

    BENCHMARK("RenderPrometheus (the /metrics text a poll rendered)")
    {
        return RenderPrometheus(reading).size();
    };

    BENCHMARK("EncodeStatsReading (what a snapshot carries)")
    {
        return EncodeStatsReading(reading).size();
    };

    BENCHMARK("DecodeStatsReading (what a client does with it)")
    {
        auto const back = DecodeStatsReading(encoded);
        return back.has_value() ? back->counters.size() : std::size_t { 0 };
    };
}
