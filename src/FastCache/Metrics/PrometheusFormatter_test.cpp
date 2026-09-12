// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
// For `Consensus::RoleTable`, which the role state-set's expected labels are read
// from rather than written out here: a list restated in the test is a second thing
// to keep in step, and it would agree with a renderer that had lost a row.
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{
/// How many times @p needle appears in @p haystack.
///
/// A count and not a `contains`, because the exposition format's rule is that a
/// series carries its `# HELP` and `# TYPE` exactly once with every sample under
/// them -- so the failure worth catching is a header emitted per label value, which
/// `contains` reports as present and a scraper rejects outright.
/// @param haystack The rendered exposition.
/// @param needle What to count.
/// @return The number of non-overlapping occurrences.
[[nodiscard]] std::size_t Occurrences(std::string_view haystack, std::string_view needle)
{
    auto found = std::size_t { 0 };
    for (auto at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size()))
        ++found;
    return found;
}
} // namespace

TEST_CASE("RenderPrometheus emits HELP/TYPE/value triples", "[metrics][prometheus]")
{
    AtomicMetricsSink metrics;
    metrics.Increment(IMetricsSink::Counter::ConnectionsTotal, 7);
    metrics.Increment(IMetricsSink::Counter::ConnectionsAdmissionRejected, 2);

    StorageStats stats;
    stats.cmdGet = 100;
    stats.getHits = 80;
    stats.getMisses = 20;
    stats.itemCount = 5;
    stats.bytesUsed = 4096;
    stats.bytesLimit = 65536;
    stats.evictions = 3;
    stats.writeErrors = 9;

    auto const body =
        RenderPrometheus(metrics, MetricsSnapshot { .storage = stats, .host = std::nullopt, .uptime = Uptime { 42s } });

    SECTION("counter from the storage snapshot")
    {
        CHECK(body.contains("# TYPE fastcached_cmd_get_total counter\n"));
        CHECK(body.contains("fastcached_cmd_get_total 100\n"));
        CHECK(body.contains("fastcached_get_hits_total 80\n"));
        CHECK(body.contains("fastcached_get_misses_total 20\n"));
        CHECK(body.contains("fastcached_evictions_total 3\n"));
        CHECK(body.contains("# TYPE fastcached_write_errors_total counter\n"));
        CHECK(body.contains("fastcached_write_errors_total 9\n"));
    }
    SECTION("gauges from the storage snapshot")
    {
        CHECK(body.contains("# TYPE fastcached_bytes_used gauge\n"));
        CHECK(body.contains("fastcached_bytes_used 4096\n"));
        CHECK(body.contains("fastcached_items 5\n"));
        CHECK(body.contains("fastcached_bytes_limit 65536\n"));
    }
    SECTION("connection counters from the sink")
    {
        CHECK(body.contains("fastcached_connections_total 7\n"));
        CHECK(body.contains("fastcached_connections_rejected_total 2\n"));
    }
    SECTION("uptime gauge")
    {
        CHECK(body.contains("# TYPE fastcached_uptime_seconds gauge\n"));
        CHECK(body.contains("fastcached_uptime_seconds 42\n"));
    }
    SECTION("every metric carries HELP text")
    {
        CHECK(body.contains("# HELP fastcached_cmd_get_total "));
    }
}

TEST_CASE("Every counter the sink knows reaches the scrape", "[metrics][prometheus]")
{
    // The case that would have caught the defect this table exists to prevent.
    // Seven of the nine live counters were absent from the renderer — both TLS
    // splits and all five `dispatch_*`, the last of which
    // docs/getting-started/distributed-compilation.md names one by one as what to
    // read off /metrics when distribution misbehaves. Every one of those series
    // was missing from the endpoint that guide sends an operator to.
    //
    // Asserted over `CounterTable` rather than against a list written out here,
    // because a hand-written list is the thing that went stale: it would have to
    // be updated by the same person who forgot the renderer.
    AtomicMetricsSink metrics;

    // A distinct value per counter, so a row rendering another row's value cannot
    // pass — which a table of near-identical rows makes the likely slip.
    for (auto const& row: CounterTable)
        metrics.Increment(row.counter, static_cast<std::uint64_t>(row.counter) + 1);

    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = StorageStats {}, .host = std::nullopt, .uptime = Uptime { 0s } });

    for (auto const& row: CounterTable)
    {
        INFO("counter " << row.prometheusName);
        CHECK(body.contains(std::format("# HELP {} ", row.prometheusName)));
        CHECK(body.contains(std::format("# TYPE {} {}\n", row.prometheusName, TypeName(row.type))));
        CHECK(body.contains(std::format("\n{} {}\n", row.prometheusName, static_cast<std::uint64_t>(row.counter) + 1)));
    }
}

TEST_CASE("A counter's exported name is unique", "[metrics][prometheus]")
{
    // Two rows sharing a name render two samples for one series, which a scraper
    // rejects outright — the whole body is dropped, so one duplicated row takes
    // every other metric with it. Cheap to assert, and the natural slip when a row
    // is added by copying its neighbour.
    for (auto const& row: CounterTable)
    {
        auto matches = 0;
        for (auto const& other: CounterTable)
            if (other.prometheusName == row.prometheusName)
                ++matches;

        INFO("counter " << row.prometheusName);
        CHECK(matches == 1);
    }
}

TEST_CASE("A process with no cache renders no cache metrics", "[metrics][prometheus]")
{
    // `fastcache-compile-node` serves this same endpoint and has no storage. A
    // default-constructed `StorageStats` would report `fastcached_items 0` and
    // `fastcached_bytes_limit 0` -- an empty, unbounded cache -- which a dashboard
    // reads as a fact rather than as an absence. Absent means absent.
    AtomicMetricsSink metrics;
    metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted, 3);

    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Uptime { 9s } });

    CHECK_FALSE(body.contains("fastcached_items"));
    CHECK_FALSE(body.contains("fastcached_bytes_limit"));
    CHECK_FALSE(body.contains("fastcached_cmd_get_total"));

    // What it does carry: every sink counter, and the uptime every process has.
    CHECK(body.contains("fastcache_worker_jobs_completed_total 3\n"));
    CHECK(body.contains("fastcached_uptime_seconds 9\n"));
    for (auto const& row: CounterTable)
        CHECK(body.contains(std::format("# TYPE {} ", row.prometheusName)));
}

TEST_CASE("A compile node reports its size, and a cache daemon does not", "[metrics][prometheus]")
{
    // "Is this node pulling its weight" is unanswerable without knowing how big it
    // is, so a worker's capacity is what an operator most wants off its scrape --
    // and it is what PR 8's resource-aware scheduling will weigh.
    AtomicMetricsSink metrics;

    auto const withHost = RenderPrometheus(metrics,
                                           MetricsSnapshot { .storage = std::nullopt,
                                                             .host = HostCapacity { .logicalCores = 16,
                                                                                    .configuredSlots = 14,
                                                                                    .totalMemoryBytes = 68719476736ULL,
                                                                                    .diskCapacityBytes = 500107862016ULL,
                                                                                    .diskFreeBytes = 123456789ULL,
                                                                                    .busySlots = 3 },
                                                             .uptime = Uptime { 1s } });

    CHECK(withHost.contains("# TYPE fastcache_node_logical_cores gauge"));
    CHECK(withHost.contains("fastcache_node_logical_cores 16"));
    CHECK(withHost.contains("fastcache_node_memory_total_bytes 68719476736"));
    CHECK(withHost.contains("fastcache_node_disk_free_bytes 123456789"));
    CHECK(withHost.contains("fastcache_node_slots_configured 14"));
    CHECK(withHost.contains("fastcache_node_slots_busy 3"));
    // The positive control for the `# TYPE` assertions below: a negative check
    // on a spelling nothing ever emits would pass whatever the renderer did.
    CHECK(withHost.contains("# TYPE fastcache_node_slots_busy"));

    // The daemon leaves it absent rather than reporting cores it does not schedule
    // against. Absent means the series is missing, not present and zero -- a zero
    // would read as a machine with no cores.
    auto const withoutHost = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = StorageStats {}, .host = std::nullopt, .uptime = Uptime { 1s } });
    // Asserted on the `# TYPE` line rather than as a bare substring. A counter's
    // HELP text may legitimately NAME another series -- `jobs_started_total`'s now
    // names this gauge, to point an operator at the real in-flight signal (#307) --
    // and `contains()` cannot tell a mention from an exported series. A `# TYPE`
    // line is emitted for every rendered metric and for nothing else, so its
    // absence is the series being absent.
    CHECK_FALSE(withoutHost.contains("# TYPE fastcache_node_logical_cores"));
    CHECK_FALSE(withoutHost.contains("# TYPE fastcache_node_slots_busy"));
}

TEST_CASE("A tiered cache renders one labelled sample per tier", "[metrics][prometheus][storage-tier]")
{
    // The numbers `MetricsSnapshot::storage` cannot carry. `LayeredStorage`
    // reports its canonical lower tier's item count, bytes and budget with the
    // composite's hit/miss patched over them, so a node's whole in-memory tier is
    // missing from a scrape that reads only that field.
    AtomicMetricsSink metrics;

    TieredStorageStats tiers {};
    tiers[static_cast<std::size_t>(StorageTier::Memory)] =
        StorageStats { .itemCount = 11, .bytesUsed = 2048, .bytesLimit = 4096, .evictions = 7 };
    tiers[static_cast<std::size_t>(StorageTier::Disk)] =
        StorageStats { .itemCount = 900, .bytesUsed = 1'000'000, .bytesLimit = 0, .evictions = 1 };

    auto const body = RenderPrometheus(
        metrics,
        MetricsSnapshot { .storage = std::nullopt, .storageTiers = tiers, .host = std::nullopt, .uptime = Uptime { 1s } });

    SECTION("each tier's own numbers, under its own label")
    {
        // Every value distinct, so a row rendering its neighbour's field cannot
        // pass -- the slip a table of near-identical rows invites.
        CHECK(body.contains("fastcached_tier_items{tier=\"memory\"} 11\n"));
        CHECK(body.contains("fastcached_tier_items{tier=\"disk\"} 900\n"));
        CHECK(body.contains("fastcached_tier_bytes_used{tier=\"memory\"} 2048\n"));
        CHECK(body.contains("fastcached_tier_bytes_used{tier=\"disk\"} 1000000\n"));
        CHECK(body.contains("fastcached_tier_bytes_limit{tier=\"memory\"} 4096\n"));
        CHECK(body.contains("fastcached_tier_bytes_limit{tier=\"disk\"} 0\n"));
        CHECK(body.contains("fastcached_tier_evictions_total{tier=\"memory\"} 7\n"));
        CHECK(body.contains("fastcached_tier_evictions_total{tier=\"disk\"} 1\n"));
    }
    SECTION("HELP and TYPE appear once per series, not once per label")
    {
        // Repeating them per label value is what a scraper rejects, and it takes
        // the whole body with it rather than the one series.
        for (auto const& name: { "fastcached_tier_items",
                                 "fastcached_tier_bytes_used",
                                 "fastcached_tier_bytes_limit",
                                 "fastcached_tier_evictions_total" })
        {
            INFO("series " << name);
            auto count = 0;
            for (auto pos = body.find(std::format("# TYPE {} ", name)); pos != std::string::npos;
                 pos = body.find(std::format("# TYPE {} ", name), pos + 1))
                ++count;
            CHECK(count == 1);
        }
    }
}

TEST_CASE("A memory-only cache renders no disk tier at all", "[metrics][prometheus][storage-tier]")
{
    // Absent is not zero, one level below the rule `MetricsSnapshot::storage`
    // already follows: `fastcached_tier_items{tier="disk"} 0` says a disk tier is
    // standing empty, and a node configured without one has no such tier to be
    // empty. A dashboard cannot tell those apart from a zero.
    AtomicMetricsSink metrics;

    TieredStorageStats tiers {};
    tiers[static_cast<std::size_t>(StorageTier::Memory)] = StorageStats { .itemCount = 4 };

    auto const body = RenderPrometheus(
        metrics,
        MetricsSnapshot { .storage = std::nullopt, .storageTiers = tiers, .host = std::nullopt, .uptime = Uptime { 1s } });

    CHECK(body.contains("fastcached_tier_items{tier=\"memory\"} 4\n"));
    CHECK_FALSE(body.contains("tier=\"disk\""));
}

TEST_CASE("A process with no cache renders no tier series", "[metrics][prometheus][storage-tier]")
{
    // The default `MetricsSnapshot` leaves every tier absent, so a worker that
    // never started a cache tier emits no `fastcached_tier_*` line -- not even a
    // `# HELP` with nothing under it, which a scraper accepts and a dashboard
    // draws as a series that has stopped reporting.
    AtomicMetricsSink metrics;
    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Uptime { 1s } });

    CHECK_FALSE(body.contains("fastcached_tier_"));
}

TEST_CASE("Whether a node has a shared cache is named, not inferred from a counter", "[metrics][prometheus]")
{
    // #214's operator-facing half. The upstream store counters are cumulative, so a
    // node with no shared cache and a node with one it has not written to yet both
    // report zero -- neither counter can answer "is there an upstream at all".
    //
    // A gauge rather than omitting the counter rows: the counters are exported in
    // full on purpose, and a per-counter "does this apply?" predicate is the
    // mechanism that once left seven of nine live counters unexported.
    AtomicMetricsSink metrics;

    // Matched on the TYPE line rather than the bare name: the failure counter's own
    // help text names this gauge, so a substring check finds it in a scrape that
    // never rendered it.
    auto const absent = RenderPrometheus(metrics, MetricsSnapshot {});
    CHECK_FALSE(absent.contains("# TYPE fastcache_node_upstream_configured"));

    auto const without = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .upstreamConfigured = false });
    CHECK(without.contains("# TYPE fastcache_node_upstream_configured gauge"));
    CHECK(without.contains("fastcache_node_upstream_configured 0"));

    auto const with = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .upstreamConfigured = true });
    CHECK(with.contains("fastcache_node_upstream_configured 1"));

    // The counters stay exported either way -- absent is modelled in the snapshot,
    // never by dropping a counter row.
    CHECK(absent.contains("fastcache_node_cache_upstream_store_failures_total"));
    CHECK(without.contains("fastcache_node_cache_upstream_store_failures_total"));
}

TEST_CASE("A process with no consensus renders no consensus series", "[metrics][prometheus][consensus]")
{
    // #435's absence, and the one this rule is stated for: a process that runs no
    // consensus has no cluster to describe, so a member count of 0 would be a
    // reading about a quorum it is not part of. The daemon is always in this state;
    // so is a compile node started without `--listen-raft`.
    //
    // Matched on the SERIES PREFIX rather than one name, because the block is five
    // series and a check naming one of them would go on passing while the other four
    // leaked out of a process with nothing to say.
    AtomicMetricsSink metrics;
    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Uptime { 1s } });

    CHECK_FALSE(body.contains("fastcache_node_consensus_"));
    // The positive control. Without it this case passes over a renderer that emits
    // nothing at all, which is a different defect wearing the same green.
    CHECK(body.contains("fastcached_uptime_seconds 1\n"));
}

TEST_CASE("A node that holds no configuration says so rather than vanishing", "[metrics][prometheus][consensus]")
{
    // The state #388 is: a node admitted into `ClusterState` that never adopted the
    // CONFIGURATION entry. `HasCluster()` is false, so `NextDeadline()` excuses it
    // from every deadline -- it campaigns in no election, grants no pre-vote, and
    // logs nothing while doing it. Invisible while the leader lives and fatal the
    // moment it dies.
    //
    // So an empty member set RENDERS. It is a reading, not an absence: the node runs
    // consensus, and what it counts is nobody. Suppressing it -- the obvious reading
    // of "absent is not zero" -- would hide exactly the state this ticket exists to
    // expose, which is why the absence is spelled one level up, by the whole block
    // being gone.
    AtomicMetricsSink metrics;
    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .consensus = ConsensusStatus {} });

    CHECK(body.contains("# TYPE fastcache_node_consensus_members gauge"));
    CHECK(body.contains("fastcache_node_consensus_members 0\n"));
    CHECK(body.contains("fastcache_node_consensus_term 0\n"));

    // And no member samples, because there are no members to name -- the tier rule
    // one axis over. The three unconditional series above are what make this
    // absence readable rather than indistinguishable from a process with no
    // consensus at all.
    CHECK_FALSE(body.contains("fastcache_node_consensus_member{"));
    CHECK_FALSE(body.contains("# TYPE fastcache_node_consensus_member gauge"));

    // Nor a leader, which a node in this state legitimately does not know.
    CHECK_FALSE(body.contains("fastcache_node_consensus_leader"));

    // A follower in term 0 that counts nobody is what a joiner looks like, so the
    // role has to be there too or the reading is half of one.
    CHECK(body.contains("fastcache_node_consensus_role{role=\"follower\"} 1\n"));
}

TEST_CASE("A node's consensus block names every member and who leads", "[metrics][prometheus][consensus]")
{
    // What `--cluster-status` could never answer: the QUORUM this node operates
    // under, from this node, whether or not it leads. `ClusterState.members` is the
    // fleet's member record and a different set, and only a leader answers it -- so
    // the node whose view matters during a stall is the one that redirects you.
    AtomicMetricsSink metrics;
    auto const body =
        RenderPrometheus(metrics,
                         MetricsSnapshot { .storage = std::nullopt,
                                           .host = std::nullopt,
                                           .consensus = ConsensusStatus { .members = { "n1", "n2", "n3" },
                                                                          .knownLeader = Consensus::NodeId { "n2" },
                                                                          .term = Consensus::Term { .value = 4 },
                                                                          .commitIndex = Consensus::LogIndex { .value = 11 },
                                                                          .role = Consensus::Role::Follower } });

    CHECK(body.contains("fastcache_node_consensus_members 3\n"));
    CHECK(body.contains("fastcache_node_consensus_term 4\n"));
    CHECK(body.contains("fastcache_node_consensus_commit_index 11\n"));

    // The set, not only its size: "which members does this node count" is the
    // question asked when a cluster will not re-elect, and a count cannot answer it.
    CHECK(body.contains("fastcache_node_consensus_member{member=\"n1\"} 1\n"));
    CHECK(body.contains("fastcache_node_consensus_member{member=\"n2\"} 1\n"));
    CHECK(body.contains("fastcache_node_consensus_member{member=\"n3\"} 1\n"));
    // One HELP/TYPE pair for the series, with the three samples under it: repeating
    // the header per label value is what a scraper rejects, and it is the mistake a
    // per-sample `Append` loop makes silently -- every value is present and the
    // exposition is invalid.
    CHECK(Occurrences(body, "# TYPE fastcache_node_consensus_member gauge") == 1);
    CHECK(Occurrences(body, "# HELP fastcache_node_consensus_member ") == 1);

    CHECK(body.contains("fastcache_node_consensus_leader{leader=\"n2\"} 1\n"));
}

TEST_CASE("The consensus role is a state set driven by RoleTable", "[metrics][prometheus][consensus]")
{
    // A sample per role with exactly one 1, and the label values come from
    // `Consensus::RoleTable` rather than a list written out here -- the rule the
    // tier samples follow against `StorageTierTable` and the counters against
    // `MetricsCatalog`, so a fifth role reaches the scrape by being a row.
    //
    // Driven over EVERY role rather than one, because a renderer that hard-coded
    // `follower` would pass a single-role case and report every leader as a
    // follower, which is the wrong answer in the direction an operator acts on.
    AtomicMetricsSink metrics;

    for (auto const& subject: Consensus::RoleTable)
    {
        auto const body =
            RenderPrometheus(metrics,
                             MetricsSnapshot { .storage = std::nullopt,
                                               .host = std::nullopt,
                                               .consensus = ConsensusStatus { .members = { "n1" }, .role = subject.role } });

        for (auto const& row: Consensus::RoleTable)
            CHECK(body.contains(std::format(
                "fastcache_node_consensus_role{{role=\"{}\"}} {}\n", row.name, row.role == subject.role ? 1 : 0)));
    }
}

TEST_CASE("A node in an election names no leader rather than an empty one", "[metrics][prometheus][consensus]")
{
    // "Nobody leads right now" is a different fact from "somebody else leads", and
    // it is the one a client cannot act on -- so it must not render as a leader
    // whose id is the empty string, which a dashboard would draw as a member.
    //
    // Absence is the spelling, and it is readable because the three unconditional
    // series are still there saying consensus is running.
    AtomicMetricsSink metrics;
    auto const body =
        RenderPrometheus(metrics,
                         MetricsSnapshot { .storage = std::nullopt,
                                           .host = std::nullopt,
                                           .consensus = ConsensusStatus { .members = { "n1", "n2" },
                                                                          .term = Consensus::Term { .value = 9 },
                                                                          .role = Consensus::Role::Candidate } });

    CHECK_FALSE(body.contains("fastcache_node_consensus_leader"));
    CHECK(body.contains("fastcache_node_consensus_members 2\n"));
    CHECK(body.contains("fastcache_node_consensus_role{role=\"candidate\"} 1\n"));
}

namespace
{
/// A sink that answers exactly like `AtomicMetricsSink` except that it reports one
/// named counter as one it has no slot for.
///
/// This is how the skew is CONSTRUCTED, and constructing it is the whole
/// difficulty of #1353. The real defect is cross-TU -- a catalogue compiled
/// against a newer `Counter` than the sink -- and `CounterTable` is
/// `static_assert`ed to cover every enumerator, so no edit to one file can
/// produce it and no single translation unit can contain it. What CAN be
/// reproduced in one TU is the thing the formatter actually observes: a sink that
/// answers `Carries(x) == false` while the catalogue still carries a row for `x`.
/// That is the seam the fix put there, and driving it is what makes the case able
/// to fail.
///
/// Deliberately NOT a sink that merely reads zero: a zero is what the defect
/// produces and what an honest idle counter produces, so a case asserting on the
/// VALUE could not tell them apart. The case asserts on the LINE.
class SkewedSink final: public IMetricsSink
{
  public:
    explicit SkewedSink(IMetricsSink::Counter missing) noexcept:
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
    IMetricsSink::Counter _missing;
};

/// The catalogue row's series name for @p counter, so the case names the series the
/// way the exporter does rather than writing the string out twice.
///
/// Through `DescriptorOf` rather than by indexing `CounterTable`, and the reason is
/// this file's own subject: a raw `CounterTable[ordinal]` assumes the table covers
/// every ordinal, which is exactly what the change under test stops the EXPORTER
/// doing. Written the raw way here it would have been the one lookup in the tree
/// making the assumption the branch exists to deny -- in the test that proves the
/// denial. `DescriptorOf` answers nullptr for `Last` and for anything past the end.
/// @param counter The counter to name.
/// @return Its Prometheus series name.
[[nodiscard]] std::string_view PrometheusNameOf(IMetricsSink::Counter counter)
{
    auto const* const row = DescriptorOf(counter);
    REQUIRE(row != nullptr);
    return row->prometheusName;
}
} // namespace

TEST_CASE("A counter the sink has no slot for is omitted and named, never rendered as zero", "[metrics][prometheus][skew]")
{
    auto const missing = IMetricsSink::Counter::ConnectionsAdmissionRejected;
    auto const series = PrometheusNameOf(missing);

    StorageStats const stats;
    auto const snapshot = MetricsSnapshot { .storage = stats, .host = std::nullopt, .uptime = Uptime { 42s } };

    // The CONTROL first, and it is not decoration: a guard nobody has watched
    // ACCEPT is not known to work, and an assertion that the line is absent would
    // pass just as well against an exporter that had stopped emitting it at all.
    AtomicMetricsSink healthy;
    auto const healthyBody = RenderPrometheus(healthy, snapshot);
    INFO("the control must show this series IS exported on a healthy build");
    REQUIRE(healthyBody.contains(std::format("{} 0\n", series)));
    CHECK_FALSE(healthyBody.contains("# SKEW"));

    SkewedSink skewed { missing };
    auto const body = RenderPrometheus(skewed, snapshot);

    // What DISTINGUISHES the fix from the defect. Under the defect this is the
    // one line that appears, well-formed, with a plausible value.
    INFO("body:\n" << body);
    CHECK_FALSE(body.contains(std::format("{} 0\n", series)));

    // Omission alone is the same failure one step along -- a scrape that is
    // quietly short. The reason has to be IN the scrape.
    // The marker AND the series in one needle. Asserting them separately is weaker
    // than it looks: the marker line embeds the name, so `contains(series)` is
    // satisfied by construction once `contains("# SKEW")` holds, and neither
    // assertion can tell a marker naming THIS row from one naming another.
    CHECK(body.contains(std::format("# SKEW {} is", series)));

    // And nothing else is lost: the skew costs one series, not the endpoint.
    CHECK(body.contains("fastcached_uptime_seconds"));
    CHECK(body.contains(PrometheusNameOf(IMetricsSink::Counter::ConnectionsTotal)));

    // The machine-readable half, and the VALUE rather than the presence. A scraper
    // discards every `#` comment, so on its own the marker above reaches monitoring
    // as a series that silently vanished -- indistinguishable from a rename or a
    // down target. This is the part an alert can fire on, and asserting `1` rather
    // than "the line is there" is what separates a real count from a line that
    // always reads zero.
    CHECK(body.contains("fastcached_metrics_catalogue_skew 1\n"));

    // The control for it: the healthy render carries the same series reading zero,
    // so `> 0` is a usable alert rather than an absence somebody has to notice.
    // A zero here is a reading, not a missing line.
    CHECK(healthyBody.contains("fastcached_metrics_catalogue_skew 0\n"));
}

TEST_CASE("A skewed sink still reports every counter it does carry", "[metrics][prometheus][skew]")
{
    auto const missing = IMetricsSink::Counter::ConnectionsAdmissionRejected;
    SkewedSink skewed { missing };
    skewed.Increment(IMetricsSink::Counter::ConnectionsTotal, 5);

    StorageStats const stats;
    auto const body =
        RenderPrometheus(skewed, MetricsSnapshot { .storage = stats, .host = std::nullopt, .uptime = Uptime { 1s } });

    // The value, not merely the name: a fix that dropped every counter row would
    // satisfy an assertion about the omitted one and is caught only here.
    CHECK(body.contains(std::format("{} 5\n", PrometheusNameOf(IMetricsSink::Counter::ConnectionsTotal))));
}
