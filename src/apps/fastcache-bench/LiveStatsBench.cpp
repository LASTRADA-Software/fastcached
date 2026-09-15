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
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>
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
#include <ranges>
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
[[nodiscard]] StatsReading ReadingAt(Regime const& regime, MetricsSnapshot const& snapshot)
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
    std::cerr << std::format(
        "live-stats bench: {} catalogue counters, layout {:#018x}\n", CounterTable.size(), StatsReadingLayout);
    std::cerr
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
            std::cerr << std::format("| {} | {} | {} | {} | {} | {:.1f}x |\n",
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
        return back.has_value() ? back->counters.Positional().size() : std::size_t { 0 };
    };
}

namespace
{

/// A leader's snapshot of @p machines machines, one toolchain each, every one of them working.
/// @param machines How many.
/// @return The snapshot `RenderFleetText` renders.
[[nodiscard]] Distributed::FleetSnapshot FleetOf(std::size_t machines)
{
    auto snapshot = Distributed::FleetSnapshot {};
    snapshot.role = Distributed::SchedulerRole::Leader;
    snapshot.leaderEndpoint = "10.0.0.1:7100";
    snapshot.leases = std::vector<std::uint64_t>(Distributed::LeaseOutcomeTable.size(), 918'273);
    snapshot.liveLeases = machines;
    snapshot.registrations = machines;
    for (auto const index: std::views::iota(std::size_t { 0 }, machines))
    {
        auto const endpoint = std::format("10.0.{}.{}:7100", index / 250, (index % 250) + 2);
        auto const capacity = Distributed::NodeCapacity { .logicalCores = 32, .totalMemoryBytes = 64ULL << 30 };
        auto const load = Distributed::NodeLoad {
            .inFlight = 7, .cpuBusyPermille = 612, .availableMemoryBytes = 21ULL << 30, .freeScratchBytes = 400ULL << 30
        };
        snapshot.nodes.push_back(Distributed::NodeReport { .endpoint = endpoint,
                                                           .fingerprints = { "gcc-14-2f7c9a1e" },
                                                           .capacity = capacity,
                                                           .load = load,
                                                           .registeredSlots = 30,
                                                           .fleetJobsInFlight = 7,
                                                           .heartbeatAge = 250ms });
        snapshot.workers.push_back(
            Distributed::WorkerReport { .info = Distributed::WorkerInfo { .id = std::format("w{}", index),
                                                                          .fingerprint = "gcc-14-2f7c9a1e",
                                                                          .endpoint = endpoint,
                                                                          .slots = 30,
                                                                          .inFlight = 7,
                                                                          .capacity = capacity,
                                                                          .load = load,
                                                                          .codecs = {} },
                                        .heartbeatAge = 30ms });
    }
    return snapshot;
}

/// The mean cost of @p work, over enough runs to take a quarter of a second.
/// @param work What to time.
/// @return Microseconds per run.
[[nodiscard]] double MicrosPerRun(auto const& work)
{
    constexpr auto Budget = std::chrono::milliseconds { 250 };
    auto runs = std::size_t { 0 };
    auto const start = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::steady_clock::duration {};
    while (elapsed < Budget)
    {
        work();
        ++runs;
        elapsed = std::chrono::steady_clock::now() - start;
    }
    return std::chrono::duration<double, std::micro> { elapsed }.count() / static_cast<double>(runs);
}

} // namespace

TEST_CASE("bench: the fleet document one interval costs a leader, polled against subscribed", "[!benchmark][livestats]")
{
    // A polling dashboard asked `/fleet.txt` once per interval EACH, so the leader rendered M documents for M
    // watchers. A subscription renders one document per TICK, however many watch, and each subscriber sends
    // those bytes once per cadence. Both are timed with the shipped renderer and encoder over one synthetic
    // fleet; the HTTP exchange a poll also paid is NOT counted, so the polled column is a floor.
    constexpr auto Interval = std::chrono::milliseconds { 5000 }; // The CLI's default `fleet` interval.
    auto const floor = Wire::LiveSubjectTable.at(static_cast<std::size_t>(Wire::LiveSubject::Fleet)).floor;
    auto const rendersPerInterval = static_cast<double>(Interval / floor);
    constexpr auto Watchers = std::to_array<std::size_t>({ 1, 4, 16, 64 });

    std::cerr << std::format("fleet bench: interval {} ms, fleet floor {} ms ({} renders per interval subscribed)\n",
                             Interval.count(),
                             floor.count(),
                             rendersPerInterval);
    std::cerr << "| machines N | document (bytes) | render (us) | push encode (us) |";
    for (auto const watchers: Watchers)
        std::cerr << std::format(" M={} polled (us) | M={} subscribed (us) |", watchers, watchers);
    std::cerr << "\n|---:|---:|---:|---:|";
    for ([[maybe_unused]] auto const watchers: Watchers)
        std::cerr << "---:|---:|";
    std::cerr << '\n';

    for (auto const machines: std::to_array<std::size_t>({ 1, 16, 64, 256 }))
    {
        auto const snapshot = FleetOf(machines);
        auto const history = Distributed::FleetHistoryView {};
        auto const document = Distributed::RenderFleetText(snapshot, history, std::nullopt);
        // The positive control: a fleet that renders no machine rows measures an empty table.
        REQUIRE(document.contains(snapshot.nodes.back().endpoint));

        auto const render =
            MicrosPerRun([&] { return Distributed::RenderFleetText(snapshot, history, std::nullopt).size(); });
        auto const push = MicrosPerRun([&] {
            return Wire::EncodeReply(Wire::Status::Push, Wire::EncodeLiveSnapshot(1, Wire::AsBytes(document))).size();
        });
        std::cerr << std::format("| {} | {} | {:.1f} | {:.2f} |", machines, document.size(), render, push);
        for (auto const watchers: Watchers)
        {
            auto const polled = static_cast<double>(watchers) * render;
            auto const subscribed = (rendersPerInterval * render) + (static_cast<double>(watchers) * push);
            std::cerr << std::format(" {:.1f} | {:.1f} |", polled, subscribed);
        }
        std::cerr << '\n';
    }
}
