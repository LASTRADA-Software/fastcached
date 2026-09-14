// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanels.hpp"

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <array>

namespace FastCache::Cli
{

namespace
{
    using Counter = IMetricsSink::Counter;

    // ---- cache ------------------------------------------------------------------------------

    // Every figure names the live model (`StatsReading`), never a series name: see `ReadingField`.
    constexpr auto GetHits = StorageField<&StorageStats::getHits>();
    constexpr auto GetMisses = StorageField<&StorageStats::getMisses>();
    constexpr auto CmdGet = StorageField<&StorageStats::cmdGet>();
    constexpr auto CmdSet = StorageField<&StorageStats::cmdSet>();
    constexpr auto BytesUsed = StorageField<&StorageStats::bytesUsed>();
    constexpr auto BytesLimit = StorageField<&StorageStats::bytesLimit>();

    // The figures a cache's rate rows draw and its history chart draws again, stated once.
    constexpr auto HitRate = FigureSpec {
        .field = GetHits, .other = GetMisses, .source = FigureSource::RateRatio, .format = FigureFormat::Percent
    };
    constexpr auto OpsPerSecond =
        FigureSpec { .field = CmdGet, .other = CmdSet, .source = FigureSource::Rate, .format = FigureFormat::Rate };
    constexpr auto ConnsPerSecond = FigureSpec { .field = CounterField<Counter::ConnectionsTotal>(),
                                                 .source = FigureSource::Rate,
                                                 .format = FigureFormat::Rate };
    constexpr auto EvictionsPerSecond = FigureSpec { .field = StorageField<&StorageStats::evictions>(),
                                                     .source = FigureSource::Rate,
                                                     .format = FigureFormat::Rate };
    constexpr auto ExpiredPerSecond = FigureSpec { .field = StorageField<&StorageStats::expirations>(),
                                                   .source = FigureSource::Rate,
                                                   .format = FigureFormat::Rate };
    constexpr auto BytesFill = FigureSpec {
        .field = BytesUsed, .other = BytesLimit, .source = FigureSource::LevelQuotient, .format = FigureFormat::Percent
    };

    constexpr auto HitRateBeside = std::array {
        BesideFigure { .key = "hit_rate_since_start",
                       .figure = { .field = GetHits,
                                   .other = GetMisses,
                                   .source = FigureSource::LevelRatio,
                                   .format = FigureFormat::Percent },
                       .after = "since start",
                       .priority = Priority::Normal },
    };

    constexpr auto OpsBeside = std::array {
        BesideFigure { .key = "get_per_sec",
                       .before = "get",
                       .figure = { .field = CmdGet, .source = FigureSource::Rate, .format = FigureFormat::Rate },
                       .priority = Priority::Low },
        BesideFigure { .key = "set_per_sec",
                       .before = "set",
                       .figure = { .field = CmdSet, .source = FigureSource::Rate, .format = FigureFormat::Rate },
                       .priority = Priority::Low },
    };

    constexpr auto ConnsBeside = std::array {
        BesideFigure { .key = "connections_accepted",
                       .before = "accepted",
                       .figure = { .field = CounterField<Counter::ConnectionsTotal>(), .source = FigureSource::Level },
                       .priority = Priority::Low },
    };

    constexpr auto EvictionsBeside = std::array {
        BesideFigure { .key = "evicted_unfetched_per_sec",
                       .before = "evicted unfetched",
                       .figure = { .field = StorageField<&StorageStats::evictedUnfetched>(),
                                   .suffix = "/s",
                                   .source = FigureSource::Rate,
                                   .format = FigureFormat::Rate },
                       .priority = Priority::Normal },
    };

    constexpr auto ExpiredBeside = std::array {
        BesideFigure { .key = "expired_unfetched_per_sec",
                       .before = "expired unfetched",
                       .figure = { .field = StorageField<&StorageStats::expiredUnfetched>(),
                                   .suffix = "/s",
                                   .source = FigureSource::Rate,
                                   .format = FigureFormat::Rate },
                       .priority = Priority::Normal },
    };

    constexpr auto CacheRates = std::array {
        RateRow { .label = "hit rate",
                  .key = "hit_rate",
                  .figure = HitRate,
                  .beside = HitRateBeside,
                  .priority = Priority::Essential },
        RateRow { .label = "ops/sec",
                  .key = "ops_per_sec",
                  .figure = OpsPerSecond,
                  .beside = OpsBeside,
                  .priority = Priority::Essential },
        RateRow { .label = "conns/sec",
                  .key = "conns_per_sec",
                  .figure = ConnsPerSecond,
                  .beside = ConnsBeside,
                  .priority = Priority::High },
        RateRow { .label = "evictions/s",
                  .key = "evictions_per_sec",
                  .figure = EvictionsPerSecond,
                  .beside = EvictionsBeside,
                  .priority = Priority::Normal },
        // §3's `expired/s`, over every expiry whichever path found it -- a lookup, a write or the cycle's sweep --
        // so the label says what the figure counts. The cycle's reclaims alone would understate it.
        RateRow { .label = "expired/s",
                  .key = "expired_per_sec",
                  .figure = ExpiredPerSecond,
                  .beside = ExpiredBeside,
                  .priority = Priority::Normal },
    };

    constexpr auto CacheLevels = std::array {
        // No field on purpose: `connections_total` is a TALLY of accepts, and drawn here it would read
        // as how many clients are connected now (§7). The row says so instead of leaving it out.
        LevelRow { .label = "connected",
                   .key = "connected",
                   .value = {},
                   .note = "no level is exported; connections_total is a TALLY",
                   .priority = Priority::Low },
        LevelRow { .label = "items",
                   .key = "items",
                   .value = { .field = StorageField<&StorageStats::itemCount>() },
                   .priority = Priority::High },
        // The percentage outlasts the gauge beside it: the gauge is the same fact drawn, the
        // percentage is the fact.
        LevelRow { .label = "bytes",
                   .key = "bytes_used",
                   .value = { .field = BytesUsed, .format = FigureFormat::Bytes },
                   .limit = FigureSpec { .field = BytesLimit, .format = FigureFormat::Bytes },
                   .limitKey = "bytes_limit",
                   .priority = Priority::Essential,
                   .limitPriority = Priority::High,
                   .gaugePriority = Priority::Low },
    };

    constexpr auto CacheTierColumns = std::array {
        TierColumn { .header = "items",
                     .key = "items",
                     .figure = { .field = StorageField<&StorageStats::itemCount>() },
                     .priority = Priority::Essential },
        TierColumn { .header = "used",
                     .key = "bytes_used",
                     .figure = { .field = StorageField<&StorageStats::bytesUsed>(), .format = FigureFormat::Bytes },
                     .priority = Priority::High },
        TierColumn { .header = "limit",
                     .key = "bytes_limit",
                     .figure = { .field = StorageField<&StorageStats::bytesLimit>(), .format = FigureFormat::Bytes },
                     .priority = Priority::Normal },
        TierColumn { .header = "evict/s",
                     .key = "evictions_per_sec",
                     .figure = { .field = StorageField<&StorageStats::evictions>(),
                                 .source = FigureSource::Rate,
                                 .format = FigureFormat::Rate },
                     .priority = Priority::Low },
        // #175 made visible: a disk tier's key index is RAM no budget covers, so it outlasts evict/s.
        TierColumn { .header = "index (RAM)",
                     .key = "index_bytes",
                     .figure = { .field = StorageField<&StorageStats::indexBytes>(), .format = FigureFormat::Bytes },
                     .priority = Priority::Normal },
    };

    constexpr auto CacheTierNote = std::array<std::string_view, 2> {
        "tier bytes carry per-tier denominations and do not sum; no per-tier",
        "hit rate is published, deliberately.",
    };

    // §3: `fastcached 0.4.1 ───── 127.0.0.1:6379  up 6d04:12  every 2s  q quit`.
    constexpr auto CacheTitle = std::array {
        TitleFactRow { .fact = ChromeFact::Version, .side = TitleSide::Subject },
        TitleFactRow { .fact = ChromeFact::Endpoint, .priority = Priority::High },
        TitleFactRow { .fact = ChromeFact::Uptime },
        TitleFactRow { .fact = ChromeFact::Interval },
        TitleFactRow { .fact = ChromeFact::QuitWord, .priority = Priority::Low },
    };

    // The history a taller terminal draws, in the order it gets rows: whether the cache is doing its job, how much it
    // is asked, how full it is, what that fullness costs, and then the quieter rates. Only fill is in the ramp: a
    // cache near its limit is the one share here where hot means look, and a hit rate is best at its top.
    constexpr auto CacheCharts = std::array {
        ChartRow { .label = "hit rate", .figure = HitRate, .top = 1.0 },
        ChartRow { .label = "ops/sec", .figure = OpsPerSecond },
        ChartRow { .label = "fill", .figure = BytesFill, .top = 1.0, .paint = ChartPaint::Ramp },
        ChartRow { .label = "evictions/s", .figure = EvictionsPerSecond },
        ChartRow { .label = "expired/s", .figure = ExpiredPerSecond },
        ChartRow { .label = "conns/sec", .figure = ConnsPerSecond },
    };

    constexpr auto CacheSpec = PanelSpec { .title = "fastcached",
                                           .rates = CacheRates,
                                           .levels = CacheLevels,
                                           .tierColumns = CacheTierColumns,
                                           .tierNote = CacheTierNote,
                                           .tierPriority = Priority::Normal,
                                           .tierNotePriority = Priority::Low,
                                           .sourcePriority = Priority::High,
                                           .fillsHeight = true,
                                           .titleFacts = CacheTitle,
                                           .charts = CacheCharts };

    // ---- node -------------------------------------------------------------------------------

    constexpr auto CompilesBeside = std::array {
        BesideFigure { .key = "compiles_completed",
                       .before = "completed",
                       .figure = { .field = CounterField<Counter::WorkerJobsCompleted>(), .source = FigureSource::Level },
                       .priority = Priority::Normal },
    };

    /// A refusal counter's rate per minute, as the split under the total writes it.
    /// @tparam Row The counter.
    /// @return The figure.
    template <Counter Row>
    [[nodiscard]] constexpr FigureSpec RefusalsPerMinute() noexcept
    {
        return FigureSpec { .field = CounterField<Row>(),
                            .scale = 60.0,
                            .alertAbove = 0.0,
                            .suffix = "/min",
                            .source = FigureSource::Rate,
                            .format = FigureFormat::Rate };
    }

    constexpr auto RefusalAddends = std::array {
        CounterField<Counter::WorkerJobsRefusedLeaseExpired>(),
        CounterField<Counter::WorkerJobsRefusedUnknownFingerprint>(),
    };

    constexpr auto RefusalSplit = std::array {
        BesideFigure { .key = "no_slot_per_min",
                       .before = "no-slot",
                       .figure = RefusalsPerMinute<Counter::WorkerJobsRefusedNoSlot>(),
                       .priority = Priority::High },
        BesideFigure { .key = "lease_expired_per_min",
                       .before = "lease-expired",
                       .figure = RefusalsPerMinute<Counter::WorkerJobsRefusedLeaseExpired>(),
                       .priority = Priority::Normal },
        BesideFigure { .key = "unknown_fingerprint_per_min",
                       .before = "unknown-fingerprint",
                       .figure = RefusalsPerMinute<Counter::WorkerJobsRefusedUnknownFingerprint>(),
                       .priority = Priority::Normal },
    };

    /// Compiles finished per minute: the rate row's figure, and its chart band's.
    constexpr auto CompilesPerMinute = FigureSpec { .field = CounterField<Counter::WorkerJobsCompleted>(),
                                                    .scale = 60.0,
                                                    .source = FigureSource::Rate,
                                                    .format = FigureFormat::Rate };

    /// A `_sum` over its `_count`: the mean compile, as the rate row and the chart band both draw it.
    constexpr auto MeanCompile = FigureSpec { .field = CounterField<Counter::WorkerCompileMillisTotal>(),
                                              .other = CounterField<Counter::WorkerJobsCompleted>(),
                                              .scale = 0.001,
                                              .source = FigureSource::RateQuotient,
                                              .format = FigureFormat::Seconds };

    /// Every refusal per minute: the three counters' rates added.
    constexpr auto RefusedPerMinute = FigureSpec { .field = CounterField<Counter::WorkerJobsRefusedNoSlot>(),
                                                   .scale = 60.0,
                                                   .alertAbove = 0.0,
                                                   .addends = RefusalAddends,
                                                   .source = FigureSource::Rate,
                                                   .format = FigureFormat::Rate };

    /// Host-wide CPU busy: the rate of busy ticks over the rate of every tick.
    constexpr auto CpuBusy = FigureSpec { .field = CpuTicksField<&CpuTicks::busy>(),
                                          .other = CpuTicksField<&CpuTicks::total>(),
                                          .source = FigureSource::RateQuotient,
                                          .format = FigureFormat::Percent };

    constexpr auto NodeRates = std::array {
        RateRow { .label = "compiles/min",
                  .key = "compiles_per_min",
                  .figure = CompilesPerMinute,
                  .beside = CompilesBeside,
                  .priority = Priority::Essential },
        // A `_sum` over its `_count`: a MEAN and nothing else. No histogram exists, so no percentile
        // can be shown, and the row says so where somebody would look for one.
        RateRow { .label = "mean compile",
                  .key = "mean_compile_seconds",
                  .figure = MeanCompile,
                  .note = "sum/count over this interval; no histogram exists, so no p50/p95 can be shown",
                  .trend = Trend::None,
                  .priority = Priority::High,
                  .notePriority = Priority::High },
        // ONE total with its trend, and the split under it: each refusal has a different fix, so the total
        // is never drawn alone (§4). The total is the three counters' rates added, not a fourth counter.
        RateRow { .label = "refused/min",
                  .key = "refused_per_min",
                  .figure = RefusedPerMinute,
                  .split = RefusalSplit,
                  .priority = Priority::High },
    };

    constexpr auto IdentityCells = std::array {
        FactCell { .label = "node-id", .fact = StatusFact::NodeId },
        FactCell { .label = "components", .fact = StatusFact::Components },
    };
    constexpr auto WorkingCells = std::array {
        FactCell { .label = "toolchains", .fact = StatusFact::Toolchains },
        FactCell { .label = "registrars", .fact = StatusFact::Registrars },
    };
    constexpr auto ConsensusCells = std::array {
        FactCell { .label = "consensus", .fact = StatusFact::Consensus },
        FactCell { .label = "leader", .fact = StatusFact::Leader },
    };
    // `6 in flight / 12 available / 16 registered`: every number a figure, so a piped stream carries each.
    constexpr auto SlotFigures = std::array {
        BesideFigure { .key = "slots_in_flight",
                       .figure = { .field = HostField<&HostCapacity::busySlots>(), .format = FigureFormat::Count },
                       .after = "in flight /",
                       .priority = Priority::Essential },
        BesideFigure { .key = "slots_available",
                       .figure = { .source = FigureSource::SlotsAvailable, .format = FigureFormat::Count },
                       .after = "available /",
                       .lead = " ",
                       .priority = Priority::Essential },
        BesideFigure { .key = "slots_registered",
                       .figure = { .field = HostField<&HostCapacity::configuredSlots>(), .format = FigureFormat::Count },
                       .after = "registered",
                       .lead = " ",
                       .priority = Priority::Essential },
    };
    constexpr auto SlotCells =
        std::array { FactCell { .label = "slots", .fact = StatusFact::Slots, .figures = SlotFigures } };

    constexpr auto CacheHits = std::array {
        BesideFigure { .key = "cache_hit_rate",
                       .before = "hits",
                       .figure = { .field = CounterField<Counter::NodeCacheHits>(),
                                   .other = CounterField<Counter::NodeCacheMisses>(),
                                   .source = FigureSource::RateRatio,
                                   .format = FigureFormat::Percent },
                       .priority = Priority::High },
        // `2.00 GiB / 8.00 GiB  ████░░░░░░░░░░░░  25.0 %`: the node's cache, used against its limit.
        BesideFigure { .key = "cache_used_bytes",
                       .figure = { .field = StorageField<&StorageStats::bytesUsed>(), .format = FigureFormat::Bytes } },
        BesideFigure { .key = "cache_limit_bytes",
                       .before = "/",
                       .figure = { .field = StorageField<&StorageStats::bytesLimit>(), .format = FigureFormat::Bytes },
                       .lead = " " },
        BesideFigure { .key = "cache_fill_ratio",
                       .figure = { .field = StorageField<&StorageStats::bytesUsed>(),
                                   .other = StorageField<&StorageStats::bytesLimit>(),
                                   .source = FigureSource::LevelQuotient,
                                   .format = FigureFormat::Percent },
                       .lead = "  ",
                       .gauge = true },
    };
    constexpr auto CacheTierCells =
        std::array { FactCell { .label = "cache tier", .fact = StatusFact::CacheTier, .figures = CacheHits } };

    // `cpu-busy 62.5 %   mem free 32.00 GiB   scratch free 41.8 GiB`: the figures the slot ceilings are made of.
    constexpr auto HostFigures = std::array {
        BesideFigure { .key = "cpu_busy_ratio", .before = "cpu-busy", .figure = CpuBusy, .priority = Priority::High },
        BesideFigure {
            .key = "mem_free_bytes",
            .before = "mem free",
            .figure = { .field = HostLoadField<&HostLoadReading::availableMemoryBytes>(), .format = FigureFormat::Bytes } },
        BesideFigure { .key = "scratch_free_bytes",
                       .before = "scratch free",
                       .figure = { .field = HostField<&HostCapacity::diskFreeBytes>(), .format = FigureFormat::Bytes },
                       .priority = Priority::Normal },
    };
    constexpr auto HostCells = std::array { FactCell { .label = "host", .fact = StatusFact::Host, .figures = HostFigures } };

    // §4's order: who the node is; whether it is WORKING rather than merely up; its slots and what limits
    // them; then the rates; then its cache tier and the machine.
    constexpr auto IdentityLines = std::array { FactLine { .cells = IdentityCells, .priority = Priority::Normal } };
    constexpr auto WorkingLines = std::array {
        FactLine { .cells = WorkingCells, .priority = Priority::High },
        FactLine { .cells = ConsensusCells, .priority = Priority::Normal },
    };
    constexpr auto SlotLines = std::array { FactLine { .cells = SlotCells, .priority = Priority::High } };
    constexpr auto CacheTierLines = std::array { FactLine { .cells = CacheTierCells, .priority = Priority::Normal } };
    constexpr auto HostLines = std::array { FactLine { .cells = HostCells, .priority = Priority::Low } };

    constexpr auto NodeFacts = std::array {
        FactBlock { .lines = IdentityLines, .place = FactPlace::AboveRates },
        FactBlock { .lines = WorkingLines, .place = FactPlace::AboveRates },
        FactBlock { .lines = SlotLines, .place = FactPlace::AboveRates },
        FactBlock { .lines = CacheTierLines, .place = FactPlace::BelowRates },
        FactBlock { .lines = HostLines, .place = FactPlace::BelowRates },
    };

    // The history a taller terminal draws, in the order it gets rows: whether the node is doing work, whether it is
    // turning work away, how loaded its machine is, and how long a compile takes. Only cpu-busy is in the ramp: the
    // rates are scaled to their own peak, where the top is the busiest moment rather than a warning.
    constexpr auto NodeCharts = std::array {
        ChartRow { .label = "compiles/min", .figure = CompilesPerMinute },
        ChartRow { .label = "refused/min", .figure = RefusedPerMinute },
        ChartRow { .label = "cpu-busy", .figure = CpuBusy, .top = 1.0, .paint = ChartPaint::Ramp },
        ChartRow { .label = "mean compile", .figure = MeanCompile },
    };

    // §4: `fastcache-compile-node 0.4.1 ───── build-07:7070  up 2d11:48  every 2s  q`.
    constexpr auto NodeTitle = std::array {
        TitleFactRow { .fact = ChromeFact::Version, .side = TitleSide::Subject },
        TitleFactRow { .fact = ChromeFact::Endpoint, .priority = Priority::High },
        TitleFactRow { .fact = ChromeFact::Uptime },
        TitleFactRow { .fact = ChromeFact::Interval },
        TitleFactRow { .fact = ChromeFact::Quit, .priority = Priority::Low },
    };

    constexpr auto NodeSpec = PanelSpec { .title = "fastcache-compile-node",
                                          .rates = NodeRates,
                                          .levels = {},
                                          .tierColumns = {},
                                          .tierNote = {},
                                          .tierPriority = Priority::Normal,
                                          .tierNotePriority = Priority::Low,
                                          .sourcePriority = Priority::High,
                                          .fillsHeight = true,
                                          .titleFacts = NodeTitle,
                                          .facts = NodeFacts,
                                          .charts = NodeCharts };

    // ---- fleet ------------------------------------------------------------------------------

    // §5: `fleet ───── leader build-01:7071  12 machines  every 5s  q`.
    constexpr auto FleetTitle = std::array {
        TitleFactRow { .fact = ChromeFact::Leader, .priority = Priority::High },
        TitleFactRow { .fact = ChromeFact::Machines },
        TitleFactRow { .fact = ChromeFact::Interval },
        TitleFactRow { .fact = ChromeFact::Quit, .priority = Priority::Low },
    };

    constexpr auto FleetSpec = PanelSpec { .title = "fleet",
                                           .rates = {},
                                           .levels = {},
                                           .tierColumns = {},
                                           .tierNote = {},
                                           .tierPriority = Priority::Normal,
                                           .tierNotePriority = Priority::Low,
                                           .sourcePriority = Priority::High,
                                           .fillsHeight = true,
                                           .document = DocumentSpec {},
                                           .titleFacts = FleetTitle };

    static_assert(PanelKeysAreWhole(CacheSpec), "every cache panel figure needs its own machine key");
    static_assert(PanelKeysAreWhole(NodeSpec), "every node panel figure needs its own machine key");
    // No figure rows today; asserted anyway, so a row added to it states its key like any other panel's.
    static_assert(PanelKeysAreWhole(FleetSpec), "every fleet panel figure needs its own machine key");

} // namespace

PanelSpec const& CachePanel() noexcept
{
    return CacheSpec;
}

PanelSpec const& NodePanel() noexcept
{
    return NodeSpec;
}

PanelSpec const& FleetPanel() noexcept
{
    return FleetSpec;
}

} // namespace FastCache::Cli
