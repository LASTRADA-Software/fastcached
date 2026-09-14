// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanels.hpp"
#include "LivePipedView.hpp"
#include "ScrapeFixture.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using FastCache::Testing::Unwrap;

namespace
{

/// Everything the loop presented, in order, as one stream: what a pipe would have received.
class StreamSink final: public IFrameSink
{
  public:
    // A pipe draws no image, and a piped view places none.
    void PresentPlaced(DashboardFrame const& frame) override
    {
        stream += frame.text;
    }

    std::string stream {};
};

/// A projection that reports the newest reading as it arrived, field for field.
///
/// The view's own mechanics -- the header, the rows by name, the formats -- are the subject of most
/// cases here, and they are easiest to see over a record whose fields the case chose.
/// @param model What is known.
/// @return The newest reading, or an empty record.
[[nodiscard]] Value ReadingAsIs(DashboardModel const& model)
{
    if (!model.latest.has_value() || model.latest->shape != Shape::Record)
        return RecordValue({});
    return *model.latest;
}

/// A stats round `ChooseStats` accepts, with the fields named.
/// @param fields The record's fields, in order.
/// @param origin The source that answered.
/// @return One attempt carrying them.
[[nodiscard]] std::vector<StatsAttempt> ReadingOf(std::vector<Field> fields, StatsOrigin origin = StatsOrigin::Info)
{
    return { StatsAttempt { .origin = origin, .asked = true, .record = RecordValue(std::move(fields)), .note = {} } };
}

/// The ordinary reading these cases stream: two counters.
/// @param connections The first counter.
/// @return The attempts.
[[nodiscard]] std::vector<StatsAttempt> Reading(std::uint64_t connections)
{
    return ReadingOf({
        Field { .name = "curr_connections", .value = NumberCell(connections) },
        Field { .name = "used_memory", .value = NumberCell(std::uint64_t { 4096 }) },
    });
}

/// A sample that read, followed by the tick the cadence owes it.
/// @param script Where to append.
/// @param seconds When it was taken.
/// @param attempts What the sources said.
void AddSample(std::vector<DashboardEvent>& script, int seconds, std::vector<StatsAttempt> attempts)
{
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Sample,
                                      .at = TimePoint { std::chrono::seconds { seconds } },
                                      .attempts = std::move(attempts) });
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Tick });
}

/// A sample nothing answered, followed by its tick.
/// @param script Where to append.
void AddFailure(std::vector<DashboardEvent>& script)
{
    script.push_back(DashboardEvent {
        .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable, .note = "nothing answered" });
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Tick });
}

/// Run the loop once; a named coroutine over pointers, for the reason `DashboardLoop_test` gives.
/// @return The task.
[[nodiscard]] Task<void> DriveOnce(IDashboardEventSource* events,
                                   IDashboardView* view,
                                   IFrameSink* sink,
                                   DashboardLimits limits,
                                   std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(events, &ReadStatsSample, view, sink, limits);
}

/// Stream @p script through a piped view and return what the pipe received.
/// @param script The events.
/// @param format The `--format`.
/// @param limits The bound.
/// @param absent The `--absent` override.
/// @param project What a row reports.
/// @return The stream.
[[nodiscard]] std::string Stream(std::vector<DashboardEvent> script,
                                 OutputFormat format,
                                 DashboardLimits limits = {},
                                 std::optional<std::string> absent = std::nullopt,
                                 FigureProjection project = &ReadingAsIs)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };
    auto view = PipedRecordView { format, std::move(absent), project };
    auto sink = StreamSink {};

    auto result = std::optional<DashboardExit> {};
    auto task = DriveOnce(&events, &view, &sink, limits, &result);
    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(result.has_value());
    return sink.stream;
}

/// The stream's lines, without their newlines.
/// @param stream What the pipe received; every line ends in a newline.
/// @return The lines.
[[nodiscard]] std::vector<std::string> Lines(std::string_view stream)
{
    std::vector<std::string> lines;
    while (!stream.empty())
    {
        auto const end = stream.find('\n');
        REQUIRE(end != std::string_view::npos);
        lines.emplace_back(stream.substr(0, end));
        stream.remove_prefix(end + 1);
    }
    return lines;
}

/// Two readings, a failure, and a third reading.
/// @return The script.
[[nodiscard]] std::vector<DashboardEvent> ReadReadFailRead()
{
    std::vector<DashboardEvent> script;
    AddSample(script, 1, Reading(10));
    AddSample(script, 2, Reading(11));
    AddFailure(script);
    AddSample(script, 4, Reading(12));
    return script;
}

} // namespace

TEST_CASE("a piped run writes its header once and one line per sample with no escape byte", "[cli][live][piped]")
{
    // §9.13, over each line-shaped format. Asserting the output PARSES passes with escapes in it,
    // so the escape byte is counted; asserting a header is present passes when it is repeated, so
    // the header's line is counted too.
    for (auto const format: { OutputFormat::Human, OutputFormat::Tsv, OutputFormat::Csv })
    {
        INFO("format: " << DescriptorOf(format)->name);
        auto const stream = Stream(ReadReadFailRead(), format);
        CHECK(std::ranges::count(stream, '\x1b') == 0);

        auto const lines = Lines(stream);
        REQUIRE(lines.size() == 5);
        CHECK(lines.front().contains("source"));
        CHECK(lines.front().contains("curr_connections"));
        CHECK(std::ranges::count(lines, lines.front()) == 1);
        CHECK(lines[1].contains("10"));
        CHECK(lines[2].contains("11"));
        CHECK(lines[4].contains("12"));
        // The failed sample is a row, not a missing one: nothing it could have read is on it.
        CHECK_FALSE(lines[3].contains("4096"));
    }
}

TEST_CASE("a piped failure is a row of absent cells in the format's own spelling", "[cli][live][piped]")
{
    auto const tsv = Lines(Stream(ReadReadFailRead(), OutputFormat::Tsv));
    REQUIRE(tsv.size() == 5);
    CHECK(tsv[3] == "\t\t");

    auto const human = Lines(Stream(ReadReadFailRead(), OutputFormat::Human));
    REQUIRE(human.size() == 5);
    CHECK(std::ranges::count(human[3], '-') == 3);

    // `--absent` names the placeholder where the format has none of its own.
    auto const named = Lines(Stream(ReadReadFailRead(), OutputFormat::Csv, {}, "NA"));
    REQUIRE(named.size() == 5);
    CHECK(named[3] == "NA,NA,NA");
}

TEST_CASE("piped json is one document per line, and a failure is a document of nulls", "[cli][live][piped]")
{
    // §9.14. No JSON parser is in this tree, so "independently parseable" is asserted as what makes
    // it so: every line is exactly one object the record renderer wrote, and no document spans a
    // newline. `--absent` does not reach it -- a null is JSON's own absent.
    auto const lines = Lines(Stream(ReadReadFailRead(), OutputFormat::Json, {}, "NA"));
    REQUIRE(lines.size() == 4);
    for (auto const& line: lines)
    {
        INFO("line: " << line);
        CHECK(line.starts_with('{'));
        CHECK(line.ends_with('}'));
        CHECK(std::ranges::count(line, '{') == 1);
    }
    CHECK(lines[0].contains(R"("curr_connections":10)"));
    CHECK(lines[2] == R"({"source":null,"curr_connections":null,"used_memory":null})");
}

TEST_CASE("Piped output under --samples=N writes exactly N rows under its header", "[cli][live][piped]")
{
    // §9.16 through the view: the budget's own frame is the Nth row, and the sample after the
    // budget is never read.
    std::vector<DashboardEvent> script;
    AddSample(script, 1, Reading(10));
    AddSample(script, 2, Reading(11));
    AddSample(script, 3, Reading(12));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv, DashboardLimits { .samples = 2 }));
    REQUIRE(lines.size() == 3);
    CHECK(lines[2].contains("11"));
}

TEST_CASE("failures before the first reading write nothing, and the header waits for a reading", "[cli][live][piped]")
{
    // They have no columns to be absent in. An empty or column-less first line would break the
    // header-once promise for everything below it.
    std::vector<DashboardEvent> script;
    AddFailure(script);
    AddFailure(script);
    AddSample(script, 3, Reading(10));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].contains("curr_connections"));
    CHECK(lines[1].contains("10"));
}

TEST_CASE("a later piped reading is read against the header by name", "[cli][live][piped]")
{
    // A header printed once is a promise about every row under it: a field it does not name is
    // dropped, and a column the reading lacks is absent rather than shifted into its neighbour.
    std::vector<DashboardEvent> script;
    AddSample(script,
              1,
              ReadingOf({ Field { .name = "a", .value = NumberCell(std::uint64_t { 1 }) },
                          Field { .name = "b", .value = NumberCell(std::uint64_t { 2 }) } }));
    AddSample(script,
              2,
              ReadingOf({ Field { .name = "b", .value = NumberCell(std::uint64_t { 20 }) },
                          Field { .name = "c", .value = NumberCell(std::uint64_t { 30 }) } }));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Csv));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "source,a,b");
    CHECK(lines[2].ends_with(",,20"));
    CHECK_FALSE(lines[2].contains("30"));
}

TEST_CASE("piped kv writes one block of name=value lines per sample", "[cli][live][piped]")
{
    std::vector<DashboardEvent> script;
    AddSample(script, 1, Reading(10));
    AddSample(script, 2, Reading(11));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Kv));
    REQUIRE(lines.size() == 7);
    CHECK(lines[1] == "curr_connections=10");
    CHECK(lines[3].empty());
    CHECK(lines[5] == "curr_connections=11");
    CHECK(std::ranges::count(lines, std::string { "curr_connections=10" }) == 1);
}

namespace
{

/// @p line split on tabs.
/// @param line One TSV line.
/// @return Its fields.
[[nodiscard]] std::vector<std::string> Fields(std::string_view line)
{
    auto fields = std::vector<std::string> {};
    while (true)
    {
        auto const end = line.find('\t');
        fields.emplace_back(line.substr(0, end));
        if (end == std::string_view::npos)
            return fields;
        line.remove_prefix(end + 1);
    }
}

/// The names of @p record's fields, in order.
/// @param record The record.
/// @return The names.
[[nodiscard]] std::vector<std::string> NamesOf(Value const& record)
{
    auto names = std::vector<std::string> {};
    for (auto const& field: record.fields)
        names.push_back(field.name);
    return names;
}

/// A cache daemon's `/metrics` at one moment: its cache block, and no catalogue counter at all.
/// @param hits GET hits.
/// @param misses GET misses.
/// @param gets GET commands.
/// @param used Bytes used.
/// @return The attempts.
[[nodiscard]] std::vector<StatsAttempt> CacheAt(std::uint64_t hits,
                                                std::uint64_t misses,
                                                std::uint64_t gets,
                                                std::uint64_t used)
{
    auto reading = StatsReading {};
    reading.snapshot.storage = StorageStats { .itemCount = 12,
                                              .bytesUsed = static_cast<std::size_t>(used),
                                              .bytesLimit = 8192,
                                              .cmdGet = gets,
                                              .getHits = hits,
                                              .getMisses = misses };
    return ReadingOf(ScrapeOf(reading), StatsOrigin::Metrics);
}

/// A compile node's `/metrics` @p step intervals in: 16 cores registered with 16 slots and 6 running, 625 permille
/// busy over every interval, 32 GiB of memory free, and a cache tier holding 2 of its 8 GiB.
/// @param step How far the CPU counters have moved.
/// @return The attempts.
[[nodiscard]] std::vector<StatsAttempt> NodeAt(std::uint64_t step)
{
    auto reading = StatsReading {};
    reading.snapshot.host = HostCapacity { .logicalCores = 16,
                                           .configuredSlots = 16,
                                           .totalMemoryBytes = std::uint64_t { 64 } << 30U,
                                           .diskCapacityBytes = std::uint64_t { 512 } << 30U,
                                           .diskFreeBytes = std::uint64_t { 41 } << 30U,
                                           .busySlots = 6 };
    reading.snapshot.hostLoad = HostLoadReading { .cpu = CpuTicks { .busy = 625 * step, .total = 1000 * step },
                                                  .availableMemoryBytes = std::uint64_t { 32 } << 30U };
    reading.snapshot.storage =
        StorageStats { .bytesUsed = std::size_t { 2 } << 30U, .bytesLimit = std::size_t { 8 } << 30U };
    return ReadingOf(ScrapeOf(reading), StatsOrigin::Metrics);
}

/// A cache daemon's `/metrics` at one moment, carrying @p tiers.
/// @param items Every tier's item count; its bytes used are a hundred times that.
/// @param tiers The `StorageTierTable` names the reading carries.
/// @return The attempts.
[[nodiscard]] std::vector<StatsAttempt> TieredAt(std::uint64_t items, std::vector<std::string_view> const& tiers)
{
    auto reading = StatsReading {};
    reading.snapshot.storage = StorageStats { .itemCount = static_cast<std::size_t>(items) };
    for (auto const tier: tiers)
    {
        auto const* row = FindIfOrNull(StorageTierTable, [tier](auto const& one) { return one.name == tier; });
        REQUIRE(row != nullptr);
        reading.snapshot.storageTiers[static_cast<std::size_t>(row->tier)] =
            StorageStats { .itemCount = static_cast<std::size_t>(items),
                           .bytesUsed = static_cast<std::size_t>(items * 100) };
    }
    return ReadingOf(ScrapeOf(reading), StatsOrigin::Metrics);
}

/// The cell under @p name in @p row, failing the case when the header names no such column.
/// @param header The header's fields.
/// @param row A row's fields.
/// @param name The column.
/// @return The cell's text.
[[nodiscard]] std::string CellUnder(std::vector<std::string> const& header,
                                    std::vector<std::string> const& row,
                                    std::string_view name)
{
    auto const found = std::ranges::find(header, name);
    REQUIRE(found != header.end());
    auto const index = static_cast<std::size_t>(found - header.begin());
    REQUIRE(index < row.size());
    return row[index];
}

constexpr auto WholeRate = FigureSpec { .field = CounterField<IMetricsSink::Counter::ConnectionsTotal>() };
constexpr auto BesideNamed = std::array { BesideFigure { .key = "b", .figure = WholeRate } };
constexpr auto BesideUnnamed = std::array { BesideFigure { .key = "", .figure = WholeRate } };
constexpr auto BesideRepeating = std::array { BesideFigure { .key = "a", .figure = WholeRate } };
constexpr auto RatesNamed =
    std::array { RateRow { .label = "a/s", .key = "a", .figure = WholeRate, .beside = BesideNamed } };
constexpr auto RatesUnnamedBeside =
    std::array { RateRow { .label = "a/s", .key = "a", .figure = WholeRate, .beside = BesideUnnamed } };
constexpr auto RatesRepeating =
    std::array { RateRow { .label = "a/s", .key = "a", .figure = WholeRate, .beside = BesideRepeating } };
constexpr auto LevelsLimitNamed =
    std::array { LevelRow { .label = "c", .key = "c", .value = WholeRate, .limit = WholeRate, .limitKey = "c_limit" } };
constexpr auto LevelsRepeatingARate = std::array { LevelRow { .label = "c", .key = "b", .value = WholeRate } };
// `<tier>_<key>` for `TiersNamed`'s column in each tier: a figure key a program could not tell apart.
constexpr auto LevelsNamingAMemoryTierFigure =
    std::array { LevelRow { .label = "c", .key = "memory_a", .value = WholeRate } };
constexpr auto LevelsNamingADiskTierFigure = std::array { LevelRow { .label = "c", .key = "disk_a", .value = WholeRate } };
constexpr auto LevelsLimitUnnamed =
    std::array { LevelRow { .label = "c", .key = "c", .value = WholeRate, .limit = WholeRate } };
constexpr auto LevelsKeyWithoutLimit =
    std::array { LevelRow { .label = "c", .key = "c", .value = WholeRate, .limitKey = "c_limit" } };
constexpr auto TiersNamed = std::array { TierColumn { .header = "items", .key = "a", .figure = WholeRate } };
constexpr auto TiersUnnamed = std::array { TierColumn { .header = "items", .key = "", .figure = WholeRate } };
constexpr auto TiersRepeating = std::array { TierColumn { .header = "items", .key = "items", .figure = WholeRate },
                                             TierColumn { .header = "count", .key = "items", .figure = WholeRate } };

/// A panel over the blocks a case chose.
/// @param rates The rate rows.
/// @param levels The level rows.
/// @param tiers The tier columns.
/// @return The spec.
[[nodiscard]] constexpr PanelSpec SpecOf(std::span<RateRow const> rates,
                                         std::span<LevelRow const> levels,
                                         std::span<TierColumn const> tiers) noexcept
{
    return PanelSpec { .title = "t", .rates = rates, .levels = levels, .tierColumns = tiers, .tierNote = {} };
}

} // namespace

TEST_CASE("a panel names every figure once for a program and never by its label", "[cli][live][piped][figures]")
{
    // The control: whole keys pass, a tier key may repeat a figure key (a tier's is named per tier).
    STATIC_REQUIRE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsLimitNamed, TiersNamed)));
    CHECK(PanelKeysAreWhole(CachePanel()));
    CHECK(PanelKeysAreWhole(NodePanel()));

    // Each way a key can be missing or ambiguous is refused, one at a time.
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesUnnamedBeside, LevelsLimitNamed, TiersNamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesRepeating, LevelsLimitNamed, TiersNamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsRepeatingARate, TiersNamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsLimitUnnamed, TiersNamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsKeyWithoutLimit, TiersNamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsLimitNamed, TiersUnnamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsLimitNamed, TiersRepeating)));

    // A figure key spelling a tier figure's name is refused in every tier, and only because a tier
    // column makes it one: without the column the same key is an ordinary name.
    STATIC_REQUIRE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsNamingAMemoryTierFigure, {})));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsNamingAMemoryTierFigure, TiersNamed)));
    STATIC_REQUIRE_FALSE(PanelKeysAreWhole(SpecOf(RatesNamed, LevelsNamingADiskTierFigure, TiersNamed)));
}

TEST_CASE("a tier figure's machine name is the tier and the column key joined, and nothing else is",
          "[cli][live][piped][figures]")
{
    CHECK(TierFigureKey("disk", "bytes_used") == "disk_bytes_used");
    CHECK(IsTierFigureKey(TierFigureKey("disk", "bytes_used"), "disk", "bytes_used"));
    CHECK_FALSE(IsTierFigureKey("disk_bytes_used", "disk", "bytes"));
    CHECK_FALSE(IsTierFigureKey("disk_bytes_used", "memory", "bytes_used"));
    CHECK_FALSE(IsTierFigureKey("diskXbytes_used", "disk", "bytes_used"));
    CHECK_FALSE(IsTierFigureKey("bytes_used_disk", "disk", "bytes_used"));
    CHECK_FALSE(IsTierFigureKey("disk_x_bytes_used", "disk", "bytes_used"));
}

TEST_CASE("a piped cache and node stream name exactly the figures their panels draw", "[cli][live][piped][figures]")
{
    // Pinned, because a script reads these names. They are the panels' `key` columns, so a label
    // reworded for the screen leaves them alone and a key changed on purpose is seen here.
    auto const model = DashboardModel {};
    CHECK(NamesOf(CacheFigures(model))
          == std::vector<std::string> { "source",
                                        "hit_rate",
                                        "hit_rate_since_start",
                                        "ops_per_sec",
                                        "get_per_sec",
                                        "set_per_sec",
                                        "conns_per_sec",
                                        "connections_accepted",
                                        "evictions_per_sec",
                                        "evicted_unfetched_per_sec",
                                        "expired_per_sec",
                                        "expired_unfetched_per_sec",
                                        "items",
                                        "bytes_used",
                                        "bytes_limit" });
    CHECK(NamesOf(NodeFigures(model))
          == std::vector<std::string> { "source",
                                        "compiles_per_min",
                                        "compiles_completed",
                                        "mean_compile_seconds",
                                        "refused_per_min",
                                        "no_slot_per_min",
                                        "lease_expired_per_min",
                                        "unknown_fingerprint_per_min",
                                        "slots_in_flight",
                                        "slots_available",
                                        "slots_registered",
                                        "cache_hit_rate",
                                        "cache_used_bytes",
                                        "cache_limit_bytes",
                                        "cache_fill_ratio",
                                        "cpu_busy_ratio",
                                        "mem_free_bytes",
                                        "scratch_free_bytes" });

    // A header is a promise about every row under it, so no two columns may share a name.
    for (auto const& names: { NamesOf(CacheFigures(model)), NamesOf(NodeFigures(model)) })
    {
        auto sorted = names;
        std::ranges::sort(sorted);
        CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
    }
}

TEST_CASE("a piped node row carries the slots, cache fill and load its panel draws, absent until two readings",
          "[cli][live][piped][figures]")
{
    // The numbers the node panel's fact lines state are its figures, so a script reads the same ones. WHAT
    // DISTINGUISHES: `slots_available` is the scheduler's ceiling (12 of 16, bound by somebody else's CPU), which needs
    // two adjacent readings, as does `cpu_busy_ratio`; both are absent on the first row while the levels are not.
    std::vector<DashboardEvent> script;
    AddSample(script, 1, NodeAt(1));
    AddSample(script, 3, NodeAt(2));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv, {}, std::nullopt, &NodeFigures));
    REQUIRE(lines.size() == 3);
    auto const header = Fields(lines[0]);
    auto const first = Fields(lines[1]);
    auto const second = Fields(lines[2]);
    REQUIRE(first.size() == header.size());
    REQUIRE(second.size() == header.size());
    auto const cell = [&header](std::vector<std::string> const& row, std::string_view name) {
        auto const found = std::ranges::find(header, name);
        REQUIRE(found != header.end());
        return row[static_cast<std::size_t>(found - header.begin())];
    };

    CHECK(cell(first, "slots_in_flight") == "6");
    CHECK(cell(first, "slots_registered") == "16");
    CHECK(cell(first, "slots_available").empty());
    CHECK(cell(first, "cpu_busy_ratio").empty());
    CHECK(cell(first, "mem_free_bytes") == "34359738368");
    CHECK(cell(first, "cache_used_bytes") == "2147483648");
    CHECK(cell(first, "cache_limit_bytes") == "8589934592");
    CHECK(cell(first, "cache_fill_ratio") == "0.2500");

    CHECK(cell(second, "slots_available") == "12");
    CHECK(cell(second, "cpu_busy_ratio") == "0.6250");
}

TEST_CASE("a piped cache row reports the panel's figures unformatted, absent until a rate can be taken",
          "[cli][live][piped][figures]")
{
    std::vector<DashboardEvent> script;
    AddSample(script, 1, CacheAt(90, 10, 1000, 4096));
    AddSample(script, 3, CacheAt(180, 20, 1500, 5120));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv, {}, std::nullopt, &CacheFigures));
    REQUIRE(lines.size() == 3);
    auto const header = Fields(lines[0]);
    auto const first = Fields(lines[1]);
    auto const second = Fields(lines[2]);
    REQUIRE(header.size() == 15);
    REQUIRE(first.size() == header.size());
    REQUIRE(second.size() == header.size());

    // A column the header does not name fails here, rather than reading past the end of a row.
    auto const cell = [&header](std::vector<std::string> const& row, std::string_view name) {
        auto const found = std::ranges::find(header, name);
        REQUIRE(found != header.end());
        return row[static_cast<std::size_t>(found - header.begin())];
    };

    // One reading has no interval: every rate is absent -- never 0, which would claim a server that
    // did nothing -- while what was read as a level is there.
    CHECK(cell(first, "hit_rate").empty());
    CHECK(cell(first, "ops_per_sec").empty());
    CHECK(cell(first, "hit_rate_since_start") == "0.9000");
    CHECK(cell(first, "bytes_used") == "4096");
    CHECK(cell(first, "bytes_limit") == "8192");
    // A level the reading carries is its number, and a counter it does not carry is absent rather than zero.
    CHECK(cell(first, "items") == "12");
    CHECK(cell(first, "conns_per_sec").empty());

    // Over two seconds: 90 hits of 100 lookups, and 500 commands.
    CHECK(cell(second, "hit_rate") == "0.9000");
    CHECK(cell(second, "ops_per_sec") == "250.000");
    CHECK(cell(second, "bytes_used") == "5120");
    CHECK(cell(second, "conns_per_sec").empty());
}

TEST_CASE("a piped cache header waits for a successful reading and names exactly the tiers it carried",
          "[cli][live][piped][figures]")
{
    // The tiers are known only from a reading, so the header is written from the first one that
    // succeeded -- never before it, which would have to guess them.
    std::vector<DashboardEvent> script;
    AddFailure(script);
    AddSample(script, 1, TieredAt(10, { "memory", "disk" }));
    AddSample(script, 2, TieredAt(20, { "memory" }));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv, {}, std::nullopt, &CacheFigures));
    REQUIRE(lines.size() == 3);
    auto const header = Fields(lines[0]);
    auto const first = Fields(lines[1]);
    auto const second = Fields(lines[2]);

    // After the panel's own fifteen, tier by tier in `StorageTierTable` order, column by column.
    REQUIRE(header.size() == 25);
    CHECK(std::vector<std::string>(header.begin() + 15, header.end())
          == std::vector<std::string> { "memory_items",
                                        "memory_bytes_used",
                                        "memory_bytes_limit",
                                        "memory_evictions_per_sec",
                                        "memory_index_bytes",
                                        "disk_items",
                                        "disk_bytes_used",
                                        "disk_bytes_limit",
                                        "disk_evictions_per_sec",
                                        "disk_index_bytes" });
    CHECK(first.size() == header.size());
    CHECK(second.size() == header.size());

    CHECK(CellUnder(header, first, "memory_items") == "10");
    CHECK(CellUnder(header, first, "disk_bytes_used") == "1000");
    // A figure one reading cannot state -- a rate, before a second reading -- is absent, never zero; a tier's
    // stated limit of zero is a reading, and reads as one.
    CHECK(CellUnder(header, first, "disk_evictions_per_sec").empty());
    CHECK(CellUnder(header, first, "disk_bytes_limit") == "0");
    // A tier the header named that a later reading lacks is absent under its heading.
    CHECK(CellUnder(header, second, "memory_items") == "20");
    CHECK(CellUnder(header, second, "disk_items").empty());
}

TEST_CASE("a tier that first appears after the piped header gains no column", "[cli][live][piped][figures]")
{
    std::vector<DashboardEvent> script;
    AddSample(script, 1, TieredAt(10, { "memory" }));
    AddSample(script, 2, TieredAt(20, { "memory", "disk" }));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv, {}, std::nullopt, &CacheFigures));
    REQUIRE(lines.size() == 3);
    auto const header = Fields(lines[0]);
    CHECK(header.size() == 20);
    CHECK(header.back() == "memory_index_bytes");
    CHECK(std::ranges::none_of(header, [](std::string const& name) { return name.starts_with("disk_"); }));
    CHECK(Fields(lines[2]).size() == header.size());
    CHECK(CellUnder(header, Fields(lines[2]), "memory_items") == "20");
}
