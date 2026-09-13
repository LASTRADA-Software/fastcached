// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanels.hpp"

#include <FastCache/Metrics/MetricsCatalog.hpp>

#include <array>

namespace FastCache::Cli
{

namespace
{
    using Counter = IMetricsSink::Counter;

    /// A catalogued counter's name in both sources that carry the catalogue.
    /// @param counter The counter.
    /// @return Its names; `INFO` carries no catalogue counter.
    [[nodiscard]] constexpr FieldNames Catalogued(Counter counter) noexcept
    {
        auto const name = DescriptorOf(counter)->prometheusName;
        return FieldNames { .metrics = name, .nodeMetrics = name, .info = {} };
    }

    /// A series only `/metrics` carries.
    /// @param name Its name.
    /// @return Its names.
    [[nodiscard]] constexpr FieldNames MetricsOnly(std::string_view name) noexcept
    {
        return FieldNames { .metrics = name, .nodeMetrics = {}, .info = {} };
    }

    // ---- cache ------------------------------------------------------------------------------

    constexpr auto GetHits =
        FieldNames { .metrics = "fastcached_get_hits_total", .nodeMetrics = {}, .info = "keyspace_hits" };
    constexpr auto GetMisses =
        FieldNames { .metrics = "fastcached_get_misses_total", .nodeMetrics = {}, .info = "keyspace_misses" };
    constexpr auto CmdGet = MetricsOnly("fastcached_cmd_get_total");
    constexpr auto CmdSet = MetricsOnly("fastcached_cmd_set_total");
    constexpr auto BytesUsed = FieldNames { .metrics = "fastcached_bytes_used", .nodeMetrics = {}, .info = "used_memory" };
    constexpr auto BytesLimit = FieldNames { .metrics = "fastcached_bytes_limit", .nodeMetrics = {}, .info = "maxmemory" };

    constexpr auto HitRateBeside = std::array {
        BesideFigure { .figure = { .field = GetHits,
                                   .other = GetMisses,
                                   .source = FigureSource::LevelRatio,
                                   .format = FigureFormat::Percent },
                       .after = "since start",
                       .priority = Priority::Normal },
    };

    constexpr auto OpsBeside = std::array {
        BesideFigure { .before = "get",
                       .figure = { .field = CmdGet, .source = FigureSource::Rate, .format = FigureFormat::Rate },
                       .priority = Priority::Low },
        BesideFigure { .before = "set",
                       .figure = { .field = CmdSet, .source = FigureSource::Rate, .format = FigureFormat::Rate },
                       .priority = Priority::Low },
    };

    constexpr auto ConnsBeside = std::array {
        BesideFigure { .before = "accepted",
                       .figure = { .field = Catalogued(Counter::ConnectionsTotal), .source = FigureSource::Level },
                       .priority = Priority::Low },
    };

    constexpr auto EvictionsBeside = std::array {
        BesideFigure { .before = "evicted unfetched",
                       .figure = { .field = MetricsOnly("fastcached_evicted_unfetched_total"),
                                   .suffix = "/s",
                                   .source = FigureSource::Rate,
                                   .format = FigureFormat::Rate },
                       .priority = Priority::Normal },
    };

    constexpr auto ReclaimedBeside = std::array {
        BesideFigure { .before = "expired unfetched",
                       .figure = { .field = MetricsOnly("fastcached_expired_unfetched_total"),
                                   .suffix = "/s",
                                   .source = FigureSource::Rate,
                                   .format = FigureFormat::Rate },
                       .priority = Priority::Normal },
    };

    constexpr auto CacheRates = std::array {
        RateRow { .label = "hit rate",
                  .figure = { .field = GetHits,
                              .other = GetMisses,
                              .source = FigureSource::RateRatio,
                              .format = FigureFormat::Percent },
                  .beside = HitRateBeside,
                  .priority = Priority::Essential },
        // `INFO` has one command total rather than the get/set split, so it stands in for the sum.
        RateRow { .label = "ops/sec",
                  .figure = { .field = { .metrics = CmdGet.metrics, .nodeMetrics = {}, .info = "total_commands_processed" },
                              .other = CmdSet,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = OpsBeside,
                  .priority = Priority::Essential },
        RateRow { .label = "conns/sec",
                  .figure = { .field = Catalogued(Counter::ConnectionsTotal),
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = ConnsBeside,
                  .priority = Priority::High },
        RateRow { .label = "evictions/s",
                  .figure = { .field = MetricsOnly("fastcached_evictions_total"),
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = EvictionsBeside,
                  .priority = Priority::Normal },
        // Named for what the counter counts: the active cycle's reclaims. A key that lapsed and was
        // found on read is not in it, so calling this row `expired/s` would understate expiry.
        RateRow { .label = "reclaimed/s",
                  .figure = { .field = Catalogued(Counter::ExpiryKeysReclaimed),
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = ReclaimedBeside,
                  .priority = Priority::Normal },
    };

    constexpr auto CacheLevels = std::array {
        // No field on purpose: `connections_total` is a TALLY of accepts, and drawn here it would read
        // as how many clients are connected now (§7). The row says so instead of leaving it out.
        LevelRow { .label = "connected",
                   .value = {},
                   .note = "no level is exported; connections_total is a TALLY",
                   .priority = Priority::Low },
        LevelRow { .label = "items", .value = { .field = MetricsOnly("fastcached_items") }, .priority = Priority::High },
        // The percentage outlasts the gauge beside it: the gauge is the same fact drawn, the
        // percentage is the fact.
        LevelRow { .label = "bytes",
                   .value = { .field = BytesUsed, .format = FigureFormat::Bytes },
                   .limit = FigureSpec { .field = BytesLimit, .format = FigureFormat::Bytes },
                   .priority = Priority::Essential,
                   .limitPriority = Priority::High,
                   .gaugePriority = Priority::Low },
    };

    constexpr auto CacheTierColumns = std::array {
        TierColumn { .header = "items",
                     .figure = { .field = MetricsOnly("fastcached_tier_items") },
                     .priority = Priority::Essential },
        TierColumn { .header = "used",
                     .figure = { .field = MetricsOnly("fastcached_tier_bytes_used"), .format = FigureFormat::Bytes },
                     .priority = Priority::High },
        TierColumn { .header = "limit",
                     .figure = { .field = MetricsOnly("fastcached_tier_bytes_limit"), .format = FigureFormat::Bytes },
                     .priority = Priority::Normal },
        TierColumn { .header = "evict/s",
                     .figure = { .field = MetricsOnly("fastcached_tier_evictions_total"),
                                 .source = FigureSource::Rate,
                                 .format = FigureFormat::Rate },
                     .priority = Priority::Low },
        // #175 made visible: a disk tier's key index is RAM no budget covers, so it outlasts evict/s.
        TierColumn { .header = "index (RAM)",
                     .figure = { .field = MetricsOnly("fastcached_tier_index_bytes"), .format = FigureFormat::Bytes },
                     .priority = Priority::Normal },
    };

    constexpr auto CacheTierNote = std::array<std::string_view, 2> {
        "tier bytes carry per-tier denominations and do not sum; no per-tier",
        "hit rate is published, deliberately.",
    };

    constexpr auto CacheSpec = PanelSpec { .title = "fastcached",
                                           .rates = CacheRates,
                                           .levels = CacheLevels,
                                           .tierColumns = CacheTierColumns,
                                           .tierNote = CacheTierNote,
                                           .tierPriority = Priority::Normal,
                                           .tierNotePriority = Priority::Low,
                                           .sourcePriority = Priority::High };

    // ---- node -------------------------------------------------------------------------------

    constexpr auto CompilesBeside = std::array {
        BesideFigure { .before = "completed",
                       .figure = { .field = Catalogued(Counter::WorkerJobsCompleted), .source = FigureSource::Level },
                       .priority = Priority::Normal },
    };

    constexpr auto NodeRates = std::array {
        RateRow { .label = "compiles/min",
                  .figure = { .field = Catalogued(Counter::WorkerJobsCompleted),
                              .scale = 60.0,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = CompilesBeside,
                  .priority = Priority::Essential },
        // A `_sum` over its `_count`: a MEAN and nothing else. No histogram exists, so no percentile
        // can be shown, and the row says so where somebody would look for one.
        RateRow { .label = "mean compile",
                  .figure = { .field = Catalogued(Counter::WorkerCompileMillisTotal),
                              .other = Catalogued(Counter::WorkerJobsCompleted),
                              .scale = 0.001,
                              .source = FigureSource::RateQuotient,
                              .format = FigureFormat::Seconds },
                  .note = "sum/count this interval; no percentile",
                  .trend = Trend::None,
                  .priority = Priority::High },
        // The refusals are a SPLIT, never a total: each has a different fix (§4).
        RateRow { .label = "no-slot/min",
                  .figure = { .field = Catalogued(Counter::WorkerJobsRefusedNoSlot),
                              .scale = 60.0,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .priority = Priority::Normal },
        RateRow { .label = "lease-exp/min",
                  .figure = { .field = Catalogued(Counter::WorkerJobsRefusedLeaseExpired),
                              .scale = 60.0,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .priority = Priority::Normal },
        RateRow { .label = "unknown-fp/min",
                  .figure = { .field = Catalogued(Counter::WorkerJobsRefusedUnknownFingerprint),
                              .scale = 60.0,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .priority = Priority::Normal },
        RateRow { .label = "cache hits",
                  .figure = { .field = Catalogued(Counter::NodeCacheHits),
                              .other = Catalogued(Counter::NodeCacheMisses),
                              .source = FigureSource::RateRatio,
                              .format = FigureFormat::Percent },
                  .priority = Priority::High },
    };

    constexpr auto NodeLevels = std::array {
        LevelRow {
            .label = "cores", .value = { .field = MetricsOnly("fastcache_node_logical_cores") }, .priority = Priority::Low },
        LevelRow { .label = "slots busy",
                   .value = { .field = MetricsOnly("fastcache_node_slots_busy") },
                   .limit = FigureSpec { .field = MetricsOnly("fastcache_node_slots_configured") },
                   .priority = Priority::Essential,
                   .limitPriority = Priority::High,
                   .gaugePriority = Priority::Low },
        LevelRow { .label = "memory",
                   .value = { .field = MetricsOnly("fastcache_node_memory_total_bytes"), .format = FigureFormat::Bytes },
                   .priority = Priority::Low },
        LevelRow { .label = "scratch free",
                   .value = { .field = MetricsOnly("fastcache_node_disk_free_bytes"), .format = FigureFormat::Bytes },
                   .limit = FigureSpec { .field = MetricsOnly("fastcache_node_disk_capacity_bytes"),
                                         .format = FigureFormat::Bytes },
                   .priority = Priority::Normal },
    };

    constexpr auto NodeSpec = PanelSpec { .title = "fastcache-compile-node",
                                          .rates = NodeRates,
                                          .levels = NodeLevels,
                                          .tierColumns = {},
                                          .tierNote = {},
                                          .tierPriority = Priority::Normal,
                                          .tierNotePriority = Priority::Low,
                                          .sourcePriority = Priority::High };

} // namespace

PanelSpec const& CachePanel() noexcept
{
    return CacheSpec;
}

PanelSpec const& NodePanel() noexcept
{
    return NodeSpec;
}

} // namespace FastCache::Cli
