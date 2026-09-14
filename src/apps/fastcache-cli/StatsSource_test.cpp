// SPDX-License-Identifier: Apache-2.0
#include "StatsSource.hpp"

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::Unwrap;

namespace
{
/// An attempt that produced a record.
/// @param origin Which source.
/// @param field One field to put in the record, so the winner is identifiable.
/// @return The attempt.
[[nodiscard]] StatsAttempt Answered(StatsOrigin origin, std::string field)
{
    return StatsAttempt { .origin = origin,
                          .asked = true,
                          .record = RecordValue(
                              { Field { .name = std::move(field), .value = NumberCell(std::uint64_t { 1 }) } }) };
}

/// An attempt that answered with @p count fields.
///
/// Beside the one-field `Answered` because the advisory names the count it OBSERVED,
/// and one case cannot tell a derivation from a constant: two sizes can.
///
/// @param origin Which source.
/// @param count How many fields the record carries; must be at least one.
/// @return The attempt.
[[nodiscard]] StatsAttempt AnsweredWith(StatsOrigin origin, std::size_t count)
{
    std::vector<Field> fields;
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        fields.push_back(Field { .name = std::format("f{}", index), .value = NumberCell(std::uint64_t { 1 }) });
    return StatsAttempt { .origin = origin, .asked = true, .record = RecordValue(std::move(fields)) };
}

/// An attempt that was made and failed.
/// @param origin Which source.
/// @param note Why.
/// @return The attempt.
[[nodiscard]] StatsAttempt Failed(StatsOrigin origin, std::string note)
{
    return StatsAttempt { .origin = origin, .asked = true, .note = std::move(note) };
}

/// An attempt that was never made.
/// @param origin Which source.
/// @param note Why not.
/// @return The attempt.
[[nodiscard]] StatsAttempt NotAsked(StatsOrigin origin, std::string note)
{
    return StatsAttempt { .origin = origin, .asked = false, .note = std::move(note) };
}

/// Whether any advisory contains @p needle.
/// @param answer The answer.
/// @param needle What to look for.
/// @return True when some advisory contains it.
[[nodiscard]] bool Mentions(Answer const& answer, std::string_view needle)
{
    return std::ranges::any_of(answer.advisories,
                               [needle](std::string const& advisory) { return advisory.contains(needle); });
}

/// The `source` field's text.
/// @param answer The answer.
/// @return The field's lexical value, or empty when there is none.
[[nodiscard]] std::string SourceOf(Answer const& answer)
{
    auto const* const field = FindField(answer.value, "source");
    return field == nullptr ? std::string {} : field->value.lexical;
}
} // namespace

TEST_CASE("every origin has a row and a name", "[cli][stats]")
{
    for (auto const& row: StatsOriginTable)
    {
        REQUIRE(DescriptorOf(row.origin) != nullptr);
        CHECK(DescriptorOf(row.origin)->name == row.name);
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.what.empty());
    }
}

TEST_CASE("the richest source wins when several answer", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> {
        Answered(StatsOrigin::Info, "from_info"),
        Answered(StatsOrigin::Metrics, "from_metrics"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Affirmative);
    // Ladder order is the table's order, not the order the attempts arrive in -- so a
    // list given thin-first must still choose the rich one.
    CHECK(SourceOf(answer) == "metrics");
    CHECK(FindField(answer.value, "from_metrics") != nullptr);
    CHECK(FindField(answer.value, "from_info") == nullptr);
}

TEST_CASE("the chosen source is reported on stdout, not only as a remark", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> { Answered(StatsOrigin::Metrics, "x") };
    auto const answer = ChooseStats(attempts);

    // Which numbers am I looking at is a question a script asks too. A dashboard that
    // cannot tell a 102-counter reading from a 7-field one draws the missing 95 as
    // zeroes.
    REQUIRE(answer.value.shape == Shape::Record);
    REQUIRE_FALSE(answer.value.fields.empty());
    CHECK(answer.value.fields.front().name == "source");
}

TEST_CASE("falling back to the thin source carries its caveat", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> {
        Failed(StatsOrigin::Metrics, "connection refused"),
        Answered(StatsOrigin::Info, "used_memory"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(SourceOf(answer) == "info");
    // The caveat says what choosing this rung costs, which is the whole reason for
    // naming the source at all -- and it opens with the count this source RETURNED,
    // read off the record rather than compiled in. `1` is what this fixture's record
    // holds; the case below drives a different size, because one case cannot tell a
    // derivation from a second constant.
    CHECK(Mentions(answer, "RESP INFO on the data port returned 1 field(s)"));
    // The flag as this binary SPELLS it. `--admin-port` stood here and exists in no
    // option table -- a remedy nobody can type is worse than none, because it sends an
    // operator to the help text for a flag that was never there.
    CHECK(Mentions(answer, "--admin-addr"));
    // And the number is NOT the rendered row count: `source` is prepended after the
    // record is read, so a count taken from the rendered value would say 2 here. Those
    // two neighbouring numbers are what put a wrong `seven` in this advisory.
    CHECK_FALSE(Mentions(answer, "returned 2 field(s)"));
    // And the richer source's failure is reported, because an operator who wanted the
    // full set needs to know why they did not get it.
    CHECK(Mentions(answer, "connection refused"));
    CHECK(Mentions(answer, "did not answer"));
}

TEST_CASE("the caveat's field count follows the record, not a constant", "[cli][stats]")
{
    // The other half of the pair above. A constant satisfies exactly one of these two
    // cases, so it is the disagreement that tests anything.
    auto const attempts = std::vector<StatsAttempt> {
        Failed(StatsOrigin::Metrics, "connection refused"),
        AnsweredWith(StatsOrigin::Info, 5),
    };

    auto const answer = ChooseStats(attempts);
    REQUIRE(answer.outcome == Outcome::Affirmative);
    CHECK(Mentions(answer, "returned 5 field(s)"));
    CHECK_FALSE(Mentions(answer, "returned 1 field(s)"));
}

TEST_CASE("a source that was never asked is not reported as having failed", "[cli][stats]")
{
    // The three-state rule, and the state that gets collapsed. Reporting "/metrics did
    // not answer" for an endpoint nothing dialled sends an operator to check a
    // listener that was never contacted.
    auto const attempts = std::vector<StatsAttempt> {
        NotAsked(StatsOrigin::Metrics, "no admin address is known"),
        Answered(StatsOrigin::Info, "used_memory"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(SourceOf(answer) == "info");
    CHECK(Mentions(answer, "was not asked"));
    // The distinguishing assertion: a collapsed implementation would say this.
    CHECK_FALSE(Mentions(answer, "did not answer"));
}

TEST_CASE("nothing answering is unreachable and names every source's state", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> {
        NotAsked(StatsOrigin::Metrics, "no admin address is known"),
        Failed(StatsOrigin::Info, "connection refused"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Unreachable);
    // "stats unavailable" alone does not say whether to start a listener, fix a port,
    // or look at the daemon. Both states are named, and differently.
    CHECK(Mentions(answer, "was not asked"));
    CHECK(Mentions(answer, "did not answer"));
    CHECK(Mentions(answer, "no admin address is known"));
    CHECK(Mentions(answer, "connection refused"));
}

TEST_CASE("a source that was not reported on at all is said to be missing", "[cli][stats]")
{
    // A gatherer that simply omits a source must not read as that source being fine.
    auto const attempts = std::vector<StatsAttempt> { Failed(StatsOrigin::Info, "refused") };
    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(Mentions(answer, "was not reported on"));
}

TEST_CASE("no attempts at all is unreachable rather than an empty success", "[cli][stats]")
{
    auto const answer = ChooseStats({});
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK_FALSE(answer.advisories.empty());
}

TEST_CASE("a Prometheus body parses into numbered fields, keeping labels", "[cli][stats]")
{
    auto const body = std::string { "# HELP fastcached_items Items.\n"
                                    "# TYPE fastcached_items gauge\n"
                                    "fastcached_items 42\n"
                                    "fastcached_tier_items{tier=\"l1\"} 7\n"
                                    "fastcached_tier_items{tier=\"l2\"} 9\n" };

    auto const record = ParsePrometheus(body);
    REQUIRE(record.shape == Shape::Record);
    REQUIRE(FindField(record, "fastcached_items") != nullptr);
    CHECK(FindField(record, "fastcached_items")->value.kind == CellKind::Number);
    CHECK(FindField(record, "fastcached_items")->value.lexical == "42");

    // The labels stay in the field name. Dropping them would merge two tiers into one
    // field and silently report one tier's number for the other.
    REQUIRE(FindField(record, "fastcached_tier_items{tier=\"l1\"}") != nullptr);
    CHECK(FindField(record, "fastcached_tier_items{tier=\"l1\"}")->value.lexical == "7");
    REQUIRE(FindField(record, "fastcached_tier_items{tier=\"l2\"}") != nullptr);
    CHECK(FindField(record, "fastcached_tier_items{tier=\"l2\"}")->value.lexical == "9");

    // The comments contributed nothing.
    CHECK(record.fields.size() == 3);
}

TEST_CASE("a label is read out of a series name, unescaped, whichever position it holds", "[cli][stats]")
{
    CHECK(LabelValue(R"(x_info{version="0.4.1"})", "version") == "0.4.1");
    CHECK(LabelValue(R"(x_info{tier="memory",version="0.4.1"})", "version") == "0.4.1");
    CHECK(LabelValue(R"(x_info{version="0.4.1",tier="memory"})", "tier") == "memory");
    // The three exposition escapes, each standing for its character: a quote does not end the value.
    CHECK(LabelValue(R"(x_info{version="1.0 \"vendor\"\\build\nline"})", "version") == "1.0 \"vendor\"\\build\nline");
    CHECK(LabelValue(R"(x_info{version=""})", "version") == "");

    // Absent, and every way of not parsing, is no value rather than a guess.
    CHECK_FALSE(LabelValue(R"(x_info{tier="memory"})", "version").has_value());
    CHECK_FALSE(LabelValue("x_info", "version").has_value());
    CHECK_FALSE(LabelValue(R"(x_info{version="0.4.1)", "version").has_value());
    CHECK_FALSE(LabelValue(R"(x_info{version="0.4.1})", "version").has_value());
    CHECK_FALSE(LabelValue(R"(x_info{version="a\tb"})", "version").has_value());
    CHECK_FALSE(LabelValue(R"(x_info{version=0.4.1})", "version").has_value());
}

TEST_CASE("the build a /metrics body names round-trips through the parser to its version", "[cli][stats]")
{
    // WHAT DISTINGUISHES: the client's reader against the server's own renderer, with a value carrying every
    // escape. A reader that forgot to unescape, or split the series at a quoted space, reads something else.
    constexpr std::string_view Awkward = "1.0 \"vendor\"\\build\nline";
    auto const reading = ParsePrometheus(RenderInfoMetric(InfoTable.front(), Awkward));
    REQUIRE(reading.fields.size() == 1);
    CHECK(VersionIn(reading, StatsOrigin::Metrics) == std::string { Awkward });

    auto const real = ParsePrometheus(RenderInfoMetric(InfoTable.front(), VersionString));
    CHECK(VersionIn(real, StatsOrigin::Metrics) == std::string { VersionString });
}

TEST_CASE("a version is read where each source states it, and not in another source's spelling", "[cli][stats]")
{
    auto const buildInfo = RecordValue(
        { Field { .name = R"(fastcached_build_info{version="0.4.1"})", .value = NumberCell(std::uint64_t { 1 }) } });
    auto const infoField = RecordValue({ Field { .name = "fastcached_version", .value = TextCell("0.4.1") } });

    CHECK(VersionIn(buildInfo, StatsOrigin::Metrics) == "0.4.1");
    CHECK(VersionIn(infoField, StatsOrigin::Info) == "0.4.1");
    CHECK_FALSE(VersionIn(infoField, StatsOrigin::Metrics).has_value());
    CHECK_FALSE(VersionIn(buildInfo, StatsOrigin::Info).has_value());
    // The counter catalogue states none; a node's version is in its status.
    CHECK_FALSE(VersionIn(buildInfo, StatsOrigin::NodeMetrics).has_value());

    // A series whose name merely begins with the build info's is another series.
    auto const longer = RecordValue(
        { Field { .name = R"(fastcached_build_info_extra{version="9"})", .value = NumberCell(std::uint64_t { 1 }) } });
    CHECK_FALSE(VersionIn(longer, StatsOrigin::Metrics).has_value());

    // An empty version is no version, in both shapes.
    auto const emptyLabel =
        RecordValue({ Field { .name = R"(fastcached_build_info{version=""})", .value = NumberCell(std::uint64_t { 1 }) } });
    CHECK_FALSE(VersionIn(emptyLabel, StatsOrigin::Metrics).has_value());
    CHECK_FALSE(VersionIn(RecordValue({ Field { .name = "fastcached_version", .value = TextCell("") } }), StatsOrigin::Info)
                    .has_value());
}

TEST_CASE("an INFO body parses, classifying numbers apart from text", "[cli][stats]")
{
    auto const body = std::string { "# Server\r\n"
                                    "fastcached_version:0.2.0\r\n"
                                    "redis_version:6.0.0-fastcached\r\n"
                                    "# Memory\r\n"
                                    "used_memory:1234\r\n" };

    auto const record = ParseInfo(body);
    REQUIRE(FindField(record, "used_memory") != nullptr);
    // A number, so JSON emits it bare and `jq` can compare it.
    CHECK(FindField(record, "used_memory")->value.kind == CellKind::Number);

    REQUIRE(FindField(record, "redis_version") != nullptr);
    // Text, and NOT a number truncated at the first dot -- which is what a lax
    // classifier would produce, turning 6.0.0-fastcached into 6.
    CHECK(FindField(record, "redis_version")->value.kind == CellKind::Text);
    CHECK(FindField(record, "redis_version")->value.lexical == "6.0.0-fastcached");

    CHECK(FindField(record, "fastcached_version")->value.lexical == "0.2.0");
    // The section headers contributed nothing.
    CHECK(record.fields.size() == 3);
}

TEST_CASE("an empty or comment-only body parses to no fields", "[cli][stats]")
{
    // The signal the gatherer uses to report "answered with no series" rather than
    // reporting an empty but successful scrape, which a dashboard draws as zeroes.
    CHECK(ParsePrometheus("").fields.empty());
    CHECK(ParsePrometheus("# only a comment\n").fields.empty());
    CHECK(ParseInfo("").fields.empty());
    CHECK(ParseInfo("# Server\r\n").fields.empty());
}

TEST_CASE("an INFO field with an empty value is kept, not dropped", "[cli][stats]")
{
    // A key the server emitted with nothing after the colon is a reported field whose
    // value happens to be empty. Dropping it would make the field look absent.
    auto const record = ParseInfo("some_key:\r\n");
    REQUIRE(FindField(record, "some_key") != nullptr);
    CHECK(FindField(record, "some_key")->value.kind == CellKind::Text);
    CHECK(FindField(record, "some_key")->value.lexical.empty());
}

namespace
{
/// A reading whose every counter, cache field, tier field and host field is a different number, one counter the
/// sink cannot carry, and a version: nothing below passes by reading back a default or a neighbour's field.
/// @return The reading.
[[nodiscard]] StatsReading EveryFieldDistinct()
{
    auto reading = StatsReading {};
    auto next = std::uint64_t { 17 };
    for (auto const& row: CounterTable)
        reading.counters[static_cast<std::size_t>(row.counter)] = (next++ * 1'000'003);
    reading.counters[static_cast<std::size_t>(IMetricsSink::Counter::ConnectionsAdmissionRejected)] = std::nullopt;
    auto const distinct = [&next] {
        auto stats = StorageStats {};
        for (auto const member: StorageStatsSizeFields)
            stats.*member = static_cast<std::size_t>(next++ * 7919);
        for (auto const member: StorageStatsCounterFields)
            stats.*member = next++ * 7919;
        return stats;
    };
    reading.snapshot.storage = distinct();
    reading.snapshot.storageTiers[0] = distinct();
    reading.snapshot.host = HostCapacity { .logicalCores = 32,
                                           .configuredSlots = 30,
                                           .totalMemoryBytes = 68'719'476'736,
                                           .diskCapacityBytes = 2'000'398'934'016,
                                           .diskFreeBytes = 442'381'631'488,
                                           .busySlots = 7 };
    reading.snapshot.hostLoad = HostLoadReading { .cpu = CpuTicks { .busy = 7'700'001, .total = 9'100'003 },
                                                  .availableMemoryBytes = 21'474'836'480 };
    reading.snapshot.upstreamConfigured = true;
    reading.snapshot.uptime = Uptime { std::chrono::seconds { 864'017 } };
    reading.version = "0.4.1-dev \"build\"";
    return reading;
}

/// The model a scrape of @p reading reads back as.
/// @param body The scrape.
/// @return The reading.
[[nodiscard]] StatsReading ReadBack(std::string const& body)
{
    auto const reading = StatsReadingFromRecord(ParsePrometheus(body), StatsOrigin::Metrics);
    REQUIRE(reading.has_value());
    return Unwrap(reading);
}
} // namespace

TEST_CASE("a /metrics scrape reads back as the reading the daemon rendered it from", "[cli][stats]")
{
    // The adapter is the one place a series name becomes a model field, so it must be the formatter's exact
    // inverse over everything a panel can read. WHAT DISTINGUISHES: every field a different number, so a row
    // storing into its neighbour's field, a missing row, a tier read as the whole cache or a counter the sink
    // cannot carry read as zero all break the equality. The consensus block is not read back (see
    // `StatsReadingFromRecord`), so the reading has none.
    auto const original = EveryFieldDistinct();
    auto const back = ReadBack(RenderPrometheus(original));

    // What a scrape does not render reads back as zero, and says so here rather than hiding in the equality: the
    // cache's own index figures, and every field of a tier but the five its series carry. No panel reads those.
    auto expected = original;
    auto storage = Unwrap(expected.snapshot.storage);
    storage.indexBytes = 0;
    storage.indexBytesAtCapacity = 0;
    expected.snapshot.storage = storage;
    for (auto& tier: expected.snapshot.storageTiers)
        if (tier.has_value())
            tier = StorageStats { .itemCount = tier->itemCount,
                                  .bytesUsed = tier->bytesUsed,
                                  .bytesLimit = tier->bytesLimit,
                                  .indexBytes = tier->indexBytes,
                                  .evictions = tier->evictions };
    CHECK(back.counters == original.counters);
    CHECK(back.snapshot.storage == expected.snapshot.storage);
    CHECK(back.snapshot.storageTiers == expected.snapshot.storageTiers);
    CHECK(back.snapshot.host == original.snapshot.host);
    CHECK(back.snapshot.hostLoad == original.snapshot.hostLoad);
    CHECK(back.snapshot.upstreamConfigured == original.snapshot.upstreamConfigured);
    CHECK(back.snapshot.uptime == original.snapshot.uptime);
    CHECK(back.version == original.version);
    CHECK(back == expected);
}

TEST_CASE("a block a record carries only part of reads as absent, never as zeroes", "[cli][stats]")
{
    // A scrape renders the cache, a tier and the host whole or not at all, so a record with part of one is not a
    // reading of that block. WHAT DISTINGUISHES: one series removed takes its block, and only its block -- the
    // other tier and the host stay.
    auto const original = EveryFieldDistinct();
    auto const whole = ReadBack(RenderPrometheus(original));
    auto const without = [&original](std::string_view series) {
        auto record = ParsePrometheus(RenderPrometheus(original));
        auto const before = record.fields.size();
        std::erase_if(record.fields, [series](Field const& field) { return field.name == series; });
        REQUIRE(record.fields.size() == before - 1);
        return Unwrap(StatsReadingFromRecord(record, StatsOrigin::Metrics));
    };

    auto const noItems = without("fastcached_items");
    CHECK_FALSE(noItems.snapshot.storage.has_value());
    CHECK(noItems.snapshot.storageTiers == whole.snapshot.storageTiers);
    CHECK(noItems.snapshot.host == whole.snapshot.host);

    auto const memoryTier = std::string { StorageTierTable[0].name };
    auto const noTierLimit = without(TierSeriesName("fastcached_tier_bytes_limit", memoryTier));
    CHECK_FALSE(noTierLimit.snapshot.storageTiers[0].has_value());
    CHECK(noTierLimit.snapshot.storage == whole.snapshot.storage);

    auto const noCores = without("fastcache_node_logical_cores");
    CHECK_FALSE(noCores.snapshot.host.has_value());
    CHECK(noCores.snapshot.storage == whole.snapshot.storage);
}

TEST_CASE("a machine's load reads back figure by figure, and its CPU as a pair or not at all", "[cli][stats]")
{
    // A scrape renders each load figure only when it was read, so the adapter reads each on its own. WHAT
    // DISTINGUISHES: one tick counter gone takes the CPU and leaves the memory; the memory gone leaves the CPU;
    // both gone is no load block, and the host beside it stays either way.
    auto const original = EveryFieldDistinct();
    auto const whole = ReadBack(RenderPrometheus(original));
    REQUIRE(whole.snapshot.hostLoad.has_value());
    auto const without = [&original](std::initializer_list<std::string_view> series) {
        auto record = ParsePrometheus(RenderPrometheus(original));
        auto const before = record.fields.size();
        std::erase_if(record.fields,
                      [series](Field const& field) { return std::ranges::find(series, field.name) != series.end(); });
        REQUIRE(record.fields.size() == before - series.size());
        return Unwrap(StatsReadingFromRecord(record, StatsOrigin::Metrics));
    };

    auto const noTotal = without({ "fastcache_node_cpu_ticks_total" });
    REQUIRE(noTotal.snapshot.hostLoad.has_value());
    CHECK_FALSE(Unwrap(noTotal.snapshot.hostLoad).cpu.has_value());
    CHECK(Unwrap(noTotal.snapshot.hostLoad).availableMemoryBytes == Unwrap(original.snapshot.hostLoad).availableMemoryBytes);

    auto const noMemory = without({ "fastcache_node_memory_available_bytes" });
    REQUIRE(noMemory.snapshot.hostLoad.has_value());
    CHECK(Unwrap(noMemory.snapshot.hostLoad).cpu == Unwrap(original.snapshot.hostLoad).cpu);
    CHECK_FALSE(Unwrap(noMemory.snapshot.hostLoad).availableMemoryBytes.has_value());

    auto const none = without({ "fastcache_node_cpu_busy_ticks_total",
                                "fastcache_node_cpu_ticks_total",
                                "fastcache_node_memory_available_bytes" });
    CHECK_FALSE(none.snapshot.hostLoad.has_value());
    CHECK(none.snapshot.host == whole.snapshot.host);
}

TEST_CASE("each source states the part of the live model it carries, and INFO states none", "[cli][stats]")
{
    auto const body = RenderPrometheus(EveryFieldDistinct());
    auto const record = ParsePrometheus(body);

    // NodeMetrics carries the catalogue: the counters, and no block of the snapshot.
    auto const node = StatsReadingFromRecord(record, StatsOrigin::NodeMetrics);
    REQUIRE(node.has_value());
    CHECK(Unwrap(node).counters == EveryFieldDistinct().counters);
    CHECK_FALSE(Unwrap(node).snapshot.storage.has_value());
    CHECK_FALSE(Unwrap(node).snapshot.host.has_value());

    // INFO fills no block whole, so it states no reading at all -- not an empty one.
    CHECK_FALSE(StatsReadingFromRecord(record, StatsOrigin::Info).has_value());
    CHECK(StatsReadingFromRecord(record, StatsOrigin::Metrics).has_value());
}
