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
                       .figure = { .field = Catalogued(Counter::ConnectionsTotal), .source = FigureSource::Level },
                       .priority = Priority::Low },
    };

    constexpr auto EvictionsBeside = std::array {
        BesideFigure { .key = "evicted_unfetched_per_sec",
                       .before = "evicted unfetched",
                       .figure = { .field = MetricsOnly("fastcached_evicted_unfetched_total"),
                                   .suffix = "/s",
                                   .source = FigureSource::Rate,
                                   .format = FigureFormat::Rate },
                       .priority = Priority::Normal },
    };

    constexpr auto ReclaimedBeside = std::array {
        BesideFigure { .key = "expired_unfetched_per_sec",
                       .before = "expired unfetched",
                       .figure = { .field = MetricsOnly("fastcached_expired_unfetched_total"),
                                   .suffix = "/s",
                                   .source = FigureSource::Rate,
                                   .format = FigureFormat::Rate },
                       .priority = Priority::Normal },
    };

    constexpr auto CacheRates = std::array {
        RateRow { .label = "hit rate",
                  .key = "hit_rate",
                  .figure = { .field = GetHits,
                              .other = GetMisses,
                              .source = FigureSource::RateRatio,
                              .format = FigureFormat::Percent },
                  .beside = HitRateBeside,
                  .priority = Priority::Essential },
        // `INFO` has one command total rather than the get/set split, so it stands in for the sum.
        RateRow { .label = "ops/sec",
                  .key = "ops_per_sec",
                  .figure = { .field = { .metrics = CmdGet.metrics, .nodeMetrics = {}, .info = "total_commands_processed" },
                              .other = CmdSet,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = OpsBeside,
                  .priority = Priority::Essential },
        RateRow { .label = "conns/sec",
                  .key = "conns_per_sec",
                  .figure = { .field = Catalogued(Counter::ConnectionsTotal),
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = ConnsBeside,
                  .priority = Priority::High },
        RateRow { .label = "evictions/s",
                  .key = "evictions_per_sec",
                  .figure = { .field = MetricsOnly("fastcached_evictions_total"),
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = EvictionsBeside,
                  .priority = Priority::Normal },
        // Named for what the counter counts: the active cycle's reclaims. A key that lapsed and was
        // found on read is not in it, so calling this row `expired/s` would understate expiry.
        RateRow { .label = "reclaimed/s",
                  .key = "reclaimed_per_sec",
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
                   .key = "connected",
                   .value = {},
                   .note = "no level is exported; connections_total is a TALLY",
                   .priority = Priority::Low },
        LevelRow { .label = "items",
                   .key = "items",
                   .value = { .field = MetricsOnly("fastcached_items") },
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
                     .figure = { .field = MetricsOnly("fastcached_tier_items") },
                     .priority = Priority::Essential },
        TierColumn { .header = "used",
                     .key = "bytes_used",
                     .figure = { .field = MetricsOnly("fastcached_tier_bytes_used"), .format = FigureFormat::Bytes },
                     .priority = Priority::High },
        TierColumn { .header = "limit",
                     .key = "bytes_limit",
                     .figure = { .field = MetricsOnly("fastcached_tier_bytes_limit"), .format = FigureFormat::Bytes },
                     .priority = Priority::Normal },
        TierColumn { .header = "evict/s",
                     .key = "evictions_per_sec",
                     .figure = { .field = MetricsOnly("fastcached_tier_evictions_total"),
                                 .source = FigureSource::Rate,
                                 .format = FigureFormat::Rate },
                     .priority = Priority::Low },
        // #175 made visible: a disk tier's key index is RAM no budget covers, so it outlasts evict/s.
        TierColumn { .header = "index (RAM)",
                     .key = "index_bytes",
                     .figure = { .field = MetricsOnly("fastcached_tier_index_bytes"), .format = FigureFormat::Bytes },
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

    constexpr auto CacheSpec = PanelSpec { .title = "fastcached",
                                           .rates = CacheRates,
                                           .levels = CacheLevels,
                                           .tierColumns = CacheTierColumns,
                                           .tierNote = CacheTierNote,
                                           .tierPriority = Priority::Normal,
                                           .tierNotePriority = Priority::Low,
                                           .sourcePriority = Priority::High,
                                           .titleFacts = CacheTitle };

    // ---- node -------------------------------------------------------------------------------

    constexpr auto CompilesBeside = std::array {
        BesideFigure { .key = "compiles_completed",
                       .before = "completed",
                       .figure = { .field = Catalogued(Counter::WorkerJobsCompleted), .source = FigureSource::Level },
                       .priority = Priority::Normal },
    };

    /// A refusal counter's rate per minute, as the split under the total writes it.
    /// @param counter The counter.
    /// @return The figure.
    [[nodiscard]] constexpr FigureSpec RefusalsPerMinute(Counter counter) noexcept
    {
        return FigureSpec { .field = Catalogued(counter),
                            .scale = 60.0,
                            .alertAbove = 0.0,
                            .suffix = "/min",
                            .source = FigureSource::Rate,
                            .format = FigureFormat::Rate };
    }

    constexpr auto RefusalAddends = std::array {
        Catalogued(Counter::WorkerJobsRefusedLeaseExpired),
        Catalogued(Counter::WorkerJobsRefusedUnknownFingerprint),
    };

    constexpr auto RefusalSplit = std::array {
        BesideFigure { .key = "no_slot_per_min",
                       .before = "no-slot",
                       .figure = RefusalsPerMinute(Counter::WorkerJobsRefusedNoSlot),
                       .priority = Priority::High },
        BesideFigure { .key = "lease_expired_per_min",
                       .before = "lease-expired",
                       .figure = RefusalsPerMinute(Counter::WorkerJobsRefusedLeaseExpired),
                       .priority = Priority::Normal },
        BesideFigure { .key = "unknown_fingerprint_per_min",
                       .before = "unknown-fingerprint",
                       .figure = RefusalsPerMinute(Counter::WorkerJobsRefusedUnknownFingerprint),
                       .priority = Priority::Normal },
    };

    constexpr auto NodeRates = std::array {
        RateRow { .label = "compiles/min",
                  .key = "compiles_per_min",
                  .figure = { .field = Catalogued(Counter::WorkerJobsCompleted),
                              .scale = 60.0,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
                  .beside = CompilesBeside,
                  .priority = Priority::Essential },
        // A `_sum` over its `_count`: a MEAN and nothing else. No histogram exists, so no percentile
        // can be shown, and the row says so where somebody would look for one.
        RateRow { .label = "mean compile",
                  .key = "mean_compile_seconds",
                  .figure = { .field = Catalogued(Counter::WorkerCompileMillisTotal),
                              .other = Catalogued(Counter::WorkerJobsCompleted),
                              .scale = 0.001,
                              .source = FigureSource::RateQuotient,
                              .format = FigureFormat::Seconds },
                  .note = "sum/count over this interval; no histogram exists, so no p50/p95 can be shown",
                  .trend = Trend::None,
                  .priority = Priority::High,
                  .notePriority = Priority::High },
        // ONE total with its trend, and the split under it: each refusal has a different fix, so the total
        // is never drawn alone (§4). The total is the three counters' rates added, not a fourth counter.
        RateRow { .label = "refused/min",
                  .key = "refused_per_min",
                  .figure = { .field = Catalogued(Counter::WorkerJobsRefusedNoSlot),
                              .scale = 60.0,
                              .alertAbove = 0.0,
                              .addends = RefusalAddends,
                              .source = FigureSource::Rate,
                              .format = FigureFormat::Rate },
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
    constexpr auto SlotCells = std::array { FactCell { .label = "slots", .fact = StatusFact::Slots } };

    constexpr auto CacheHits = std::array {
        BesideFigure { .key = "cache_hit_rate",
                       .before = "hits",
                       .figure = { .field = Catalogued(Counter::NodeCacheHits),
                                   .other = Catalogued(Counter::NodeCacheMisses),
                                   .source = FigureSource::RateRatio,
                                   .format = FigureFormat::Percent },
                       .priority = Priority::High },
    };
    constexpr auto CacheTierCells =
        std::array { FactCell { .label = "cache tier", .fact = StatusFact::CacheTier, .figures = CacheHits } };

    constexpr auto ScratchFree = std::array {
        BesideFigure { .key = "scratch_free_bytes",
                       .before = "scratch free",
                       .figure = { .field = MetricsOnly("fastcache_node_disk_free_bytes"), .format = FigureFormat::Bytes },
                       .priority = Priority::Normal },
    };
    constexpr auto HostCells = std::array { FactCell { .label = "host", .fact = StatusFact::Host, .figures = ScratchFree } };

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
                                          .titleFacts = NodeTitle,
                                          .facts = NodeFacts };

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
