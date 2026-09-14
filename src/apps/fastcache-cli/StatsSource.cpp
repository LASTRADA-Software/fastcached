// SPDX-License-Identifier: Apache-2.0
#include "StatsSource.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// Split @p text into lines, tolerating both LF and CRLF.
    /// @param text The body.
    /// @return The lines, without terminators.
    [[nodiscard]] std::vector<std::string_view> Lines(std::string_view text)
    {
        std::vector<std::string_view> lines;
        std::size_t at = 0;
        while (at <= text.size())
        {
            auto const eol = text.find('\n', at);
            auto const end = eol == std::string_view::npos ? text.size() : eol;
            auto line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            lines.push_back(line);
            if (eol == std::string_view::npos)
                break;
            at = eol + 1;
        }
        return lines;
    }

    /// Whether @p text is exactly an unsigned integer.
    ///
    /// Used to decide whether a value is a `Number` cell, which decides whether JSON
    /// quotes it. Strict, so `6.0.0-fastcached` stays text rather than becoming a
    /// truncated 6.
    /// @param text The value text.
    /// @return The value, or nullopt.
    [[nodiscard]] std::optional<std::uint64_t> AsUnsigned(std::string_view text) noexcept
    {
        if (text.empty() || !std::ranges::all_of(text, [](char ch) { return ch >= '0' && ch <= '9'; }))
            return std::nullopt;
        std::uint64_t value = 0;
        auto const* const first = text.data();
        auto const* const last = first + text.size();
        auto const [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc {} || ptr != last)
            return std::nullopt;
        return value;
    }

    /// Classify a value read out of a text stats body.
    /// @param text The value text.
    /// @return A `Number` cell when it is exactly an integer, a `Text` cell otherwise.
    [[nodiscard]] Cell ClassifiedCell(std::string_view text)
    {
        if (auto const number = AsUnsigned(text); number.has_value())
            return NumberCell(*number);
        return TextCell(std::string { text });
    }

    /// Trim ASCII spaces and tabs from both ends.
    /// @param text The text.
    /// @return The trimmed view.
    [[nodiscard]] std::string_view Trim(std::string_view text) noexcept
    {
        auto const first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return {};
        auto const last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    /// One `/metrics` series that is a field of a struct in the live model, and where the number goes.
    /// @tparam Struct The struct the field belongs to.
    template <typename Struct>
    struct SeriesField
    {
        std::string_view series;                                   ///< The series name, unlabelled.
        void (*store)(Struct& into, std::uint64_t value) noexcept; ///< Where its number goes.
    };

    /// The cache's statistics as `/metrics` spells them, one row per `StorageStats` field a scrape renders.
    constexpr auto StorageSeries = std::array {
        SeriesField<StorageStats> { .series = "fastcached_cmd_get_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.cmdGet = v; } },
        SeriesField<StorageStats> { .series = "fastcached_cmd_set_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.cmdSet = v; } },
        SeriesField<StorageStats> { .series = "fastcached_cmd_touch_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.cmdTouch = v; } },
        SeriesField<StorageStats> { .series = "fastcached_cmd_flush_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.cmdFlush = v; } },
        SeriesField<StorageStats> { .series = "fastcached_get_hits_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.getHits = v; } },
        SeriesField<StorageStats> { .series = "fastcached_get_misses_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.getMisses = v; } },
        SeriesField<StorageStats> { .series = "fastcached_delete_hits_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.deleteHits = v; } },
        SeriesField<StorageStats> { .series = "fastcached_delete_misses_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.deleteMisses = v; } },
        SeriesField<StorageStats> { .series = "fastcached_incr_hits_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.incrHits = v; } },
        SeriesField<StorageStats> { .series = "fastcached_incr_misses_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.incrMisses = v; } },
        SeriesField<StorageStats> { .series = "fastcached_decr_hits_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.decrHits = v; } },
        SeriesField<StorageStats> { .series = "fastcached_decr_misses_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.decrMisses = v; } },
        SeriesField<StorageStats> { .series = "fastcached_touch_hits_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.touchHits = v; } },
        SeriesField<StorageStats> { .series = "fastcached_touch_misses_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.touchMisses = v; } },
        SeriesField<StorageStats> { .series = "fastcached_cas_hits_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.casHits = v; } },
        SeriesField<StorageStats> { .series = "fastcached_cas_misses_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.casMisses = v; } },
        SeriesField<StorageStats> { .series = "fastcached_cas_badval_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.casBadval = v; } },
        SeriesField<StorageStats> { .series = "fastcached_write_errors_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.writeErrors = v; } },
        SeriesField<StorageStats> { .series = "fastcached_evictions_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.evictions = v; } },
        SeriesField<StorageStats> { .series = "fastcached_evicted_unfetched_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.evictedUnfetched = v; } },
        SeriesField<StorageStats> { .series = "fastcached_expired_unfetched_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.expiredUnfetched = v; } },
        SeriesField<StorageStats> { .series = "fastcached_expirations_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.expirations = v; } },
        SeriesField<StorageStats> {
            .series = "fastcached_items",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.itemCount = static_cast<std::size_t>(v); } },
        SeriesField<StorageStats> {
            .series = "fastcached_bytes_used",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.bytesUsed = static_cast<std::size_t>(v); } },
        SeriesField<StorageStats> {
            .series = "fastcached_bytes_limit",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.bytesLimit = static_cast<std::size_t>(v); } },
    };

    /// One tier's statistics as `/metrics` spells them, each series labelled with the tier.
    constexpr auto TierSeries = std::array {
        SeriesField<StorageStats> {
            .series = "fastcached_tier_items",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.itemCount = static_cast<std::size_t>(v); } },
        SeriesField<StorageStats> {
            .series = "fastcached_tier_bytes_used",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.bytesUsed = static_cast<std::size_t>(v); } },
        SeriesField<StorageStats> {
            .series = "fastcached_tier_bytes_limit",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.bytesLimit = static_cast<std::size_t>(v); } },
        SeriesField<StorageStats> { .series = "fastcached_tier_evictions_total",
                                    .store = [](StorageStats& s, std::uint64_t v) noexcept { s.evictions = v; } },
        SeriesField<StorageStats> {
            .series = "fastcached_tier_index_bytes",
            .store = [](StorageStats& s, std::uint64_t v) noexcept { s.indexBytes = static_cast<std::size_t>(v); } },
    };

    /// The machine's capacity as `/metrics` spells it.
    constexpr auto HostSeries = std::array {
        SeriesField<HostCapacity> {
            .series = "fastcache_node_logical_cores",
            .store = [](HostCapacity& h, std::uint64_t v) noexcept { h.logicalCores = static_cast<std::size_t>(v); } },
        SeriesField<HostCapacity> { .series = "fastcache_node_memory_total_bytes",
                                    .store = [](HostCapacity& h, std::uint64_t v) noexcept { h.totalMemoryBytes = v; } },
        SeriesField<HostCapacity> { .series = "fastcache_node_disk_capacity_bytes",
                                    .store = [](HostCapacity& h, std::uint64_t v) noexcept { h.diskCapacityBytes = v; } },
        SeriesField<HostCapacity> { .series = "fastcache_node_disk_free_bytes",
                                    .store = [](HostCapacity& h, std::uint64_t v) noexcept { h.diskFreeBytes = v; } },
        SeriesField<HostCapacity> {
            .series = "fastcache_node_slots_configured",
            .store = [](HostCapacity& h, std::uint64_t v) noexcept { h.configuredSlots = static_cast<std::size_t>(v); } },
        SeriesField<HostCapacity> {
            .series = "fastcache_node_slots_busy",
            .store = [](HostCapacity& h, std::uint64_t v) noexcept { h.busySlots = static_cast<std::size_t>(v); } },
    };

    /// The machine's CPU counters as `/metrics` spells them: busy ticks, then every tick.
    constexpr std::string_view CpuBusyTicksSeries = "fastcache_node_cpu_busy_ticks_total";
    /// See `CpuBusyTicksSeries`.
    constexpr std::string_view CpuTicksSeries = "fastcache_node_cpu_ticks_total";
    /// The memory a new process could obtain, as `/metrics` spells it.
    constexpr std::string_view MemoryAvailableSeries = "fastcache_node_memory_available_bytes";

    /// The series stating whether an upstream is configured, 0 or 1.
    constexpr std::string_view UpstreamSeries = "fastcache_node_upstream_configured";

    /// The series stating the uptime in seconds.
    constexpr std::string_view UptimeSeries = "fastcached_uptime_seconds";

    /// The unsigned number @p record holds for @p name, or nullopt where it holds none.
    /// @param record The record.
    /// @param name The field.
    /// @return The number.
    [[nodiscard]] std::optional<std::uint64_t> CountIn(Value const& record, std::string_view name)
    {
        auto const* field = FindField(record, name);
        // A `Number` cell and a `Text` cell alike: which kind a figure arrives as is the parser's, and text that is
        // not exactly an unsigned integer is no number either.
        return field == nullptr || (field->value.kind != CellKind::Number && field->value.kind != CellKind::Text)
                   ? std::nullopt
                   : AsUnsigned(field->value.lexical);
    }

    /// What a machine is doing, out of @p record: each figure on its own, as a scrape renders each on its own.
    ///
    /// The CPU is a pair or nothing: one tick counter without the other is no share of anything. **A load block
    /// with neither figure reads back absent**, the one fact a scrape cannot state, since it renders such a block
    /// as no series at all; a panel draws the two alike, and the subscription's binary form states it.
    /// @param record The record.
    /// @return The load, or nullopt when the record states no figure of it.
    [[nodiscard]] std::optional<HostLoadReading> HostLoadIn(Value const& record)
    {
        auto load = HostLoadReading {};
        auto const busy = CountIn(record, CpuBusyTicksSeries);
        auto const total = CountIn(record, CpuTicksSeries);
        if (busy.has_value() && total.has_value())
            load.cpu = CpuTicks { .busy = *busy, .total = *total };
        load.availableMemoryBytes = CountIn(record, MemoryAvailableSeries);
        return load.cpu.has_value() || load.availableMemoryBytes.has_value() ? std::optional { load } : std::nullopt;
    }

    /// A block read whole out of @p record: every row of @p rows present, or no block.
    /// @param record The record.
    /// @param rows The block's series.
    /// @param name How a row's series is spelled in the record, labelled or not.
    /// @return The block, or nullopt when any row is missing.
    template <typename Struct, std::size_t N, typename Name>
    [[nodiscard]] std::optional<Struct> BlockIn(Value const& record,
                                                std::array<SeriesField<Struct>, N> const& rows,
                                                Name name)
    {
        auto block = Struct {};
        for (auto const& row: rows)
        {
            auto const value = CountIn(record, name(row.series));
            if (!value.has_value())
                return std::nullopt;
            row.store(block, *value);
        }
        return block;
    }

    /// A reading holding every catalogue counter @p record carries by its series name, and nothing else.
    /// @param record The record.
    /// @return The reading.
    [[nodiscard]] StatsReading CountersIn(Value const& record)
    {
        auto reading = StatsReading {};
        for (auto const& row: CounterTable)
            reading.counters[static_cast<std::size_t>(row.counter)] = CountIn(record, row.prometheusName);
        return reading;
    }

    /// The whole live model a `/metrics` record states.
    /// @param record The record.
    /// @return The reading.
    [[nodiscard]] std::optional<StatsReading> FromMetrics(Value const& record)
    {
        auto reading = CountersIn(record);
        auto const plain = [](std::string_view series) {
            return std::string { series };
        };
        auto& snapshot = reading.snapshot;
        snapshot.storage = BlockIn(record, StorageSeries, plain);
        for (auto const& tier: StorageTierTable)
            snapshot.storageTiers[static_cast<std::size_t>(tier.tier)] =
                BlockIn(record, TierSeries, [&tier](std::string_view series) { return TierSeriesName(series, tier.name); });
        snapshot.host = BlockIn(record, HostSeries, plain);
        snapshot.hostLoad = HostLoadIn(record);
        if (auto const upstream = CountIn(record, UpstreamSeries); upstream.has_value() && *upstream <= 1)
            snapshot.upstreamConfigured = *upstream == 1;
        if (auto const seconds = CountIn(record, UptimeSeries); seconds.has_value())
            snapshot.uptime = Uptime { std::chrono::seconds { static_cast<std::chrono::seconds::rep>(*seconds) } };
        return reading;
    }

    /// The counters a `NodeMetrics` record states, by their catalogue names.
    /// @param record The record.
    /// @return The reading.
    [[nodiscard]] std::optional<StatsReading> FromNodeMetrics(Value const& record)
    {
        return CountersIn(record);
    }

    /// No reading: `INFO` states no block of the model whole, and no uptime; see `StatsReadingFromRecord`.
    /// @return Nothing.
    [[nodiscard]] std::optional<StatsReading> FromInfo(Value const& /*record*/)
    {
        return std::nullopt;
    }

    /// How one source's record becomes the live model.
    struct RecordReader
    {
        StatsOrigin origin;                                       ///< The enumerator this row describes.
        std::optional<StatsReading> (*read)(Value const& record); ///< What the record states, the version aside.
    };

    /// One row per `StatsOrigin`, in enumerator order.
    constexpr EnumTable<StatsOrigin, RecordReader> RecordReaderTable { {
        { .origin = StatsOrigin::Metrics, .read = &FromMetrics },
        { .origin = StatsOrigin::NodeMetrics, .read = &FromNodeMetrics },
        { .origin = StatsOrigin::Info, .read = &FromInfo },
    } };

    static_assert(RowsInEnumeratorOrder(RecordReaderTable, &RecordReader::origin),
                  "RecordReaderTable must hold one row per StatsOrigin, in enumerator order");

    /// One escape the exposition format writes inside a label value.
    struct LabelEscape
    {
        char written; ///< What follows the backslash.
        char means;   ///< The character it stands for.
    };

    /// Every label-value escape, and nothing else: any other backslash sequence does not parse.
    constexpr auto LabelEscapes = std::array {
        LabelEscape { .written = '\\', .means = '\\' },
        LabelEscape { .written = '"', .means = '"' },
        LabelEscape { .written = 'n', .means = '\n' },
    };
} // namespace

StatsOriginSpec const* DescriptorOf(StatsOrigin origin) noexcept
{
    auto const index = static_cast<std::size_t>(origin);
    if (index >= StatsOriginTable.size())
        return nullptr;
    return &StatsOriginTable[index];
}

Value ParsePrometheus(std::string_view body)
{
    std::vector<Field> fields;
    for (auto const line: Lines(body))
    {
        auto const trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#')
            continue;
        // `name value`, or `name{labels} value`. The last space separates them, so a
        // label value containing a space does not split the series name.
        auto const space = trimmed.find_last_of(' ');
        if (space == std::string_view::npos)
            continue;
        auto const name = Trim(trimmed.substr(0, space));
        auto const value = Trim(trimmed.substr(space + 1));
        if (name.empty() || value.empty())
            continue;
        fields.push_back(Field { .name = std::string { name }, .value = ClassifiedCell(value) });
    }
    return RecordValue(std::move(fields));
}

std::optional<std::string> LabelValue(std::string_view series, std::string_view label)
{
    auto const open = series.find('{');
    if (open == std::string_view::npos || !series.ends_with('}'))
        return std::nullopt;
    auto rest = series.substr(open + 1, series.size() - open - 2);
    while (!rest.empty())
    {
        auto const equals = rest.find("=\"");
        if (equals == std::string_view::npos)
            return std::nullopt;
        auto const name = Trim(rest.substr(0, equals));
        auto value = std::string {};
        auto at = equals + 2;
        while (at < rest.size() && rest[at] != '"')
        {
            if (rest[at] != '\\')
            {
                value += rest[at];
                ++at;
                continue;
            }
            auto const* const escape =
                at + 1 < rest.size() ? FindOrNull(LabelEscapes, rest[at + 1], &LabelEscape::written) : nullptr;
            if (escape == nullptr)
                return std::nullopt;
            value += escape->means;
            at += 2;
        }
        if (at == rest.size())
            return std::nullopt;
        if (name == label)
            return value;
        rest.remove_prefix(at + 1);
        if (rest.starts_with(','))
            rest.remove_prefix(1);
        else if (!rest.empty())
            return std::nullopt;
    }
    return std::nullopt;
}

std::string TierSeriesName(std::string_view base, std::string_view tier)
{
    return std::format("{}{{tier=\"{}\"}}", base, tier);
}

std::optional<StatsReading> StatsReadingFromRecord(Value const& record, StatsOrigin origin)
{
    auto reading = RecordReaderTable[static_cast<std::size_t>(origin)].read(record);
    if (reading.has_value())
        reading->version = VersionIn(record, origin).value_or(std::string {});
    return reading;
}

std::optional<std::string> VersionIn(Value const& reading, StatsOrigin origin)
{
    auto const* const row = DescriptorOf(origin);
    if (row == nullptr || row->version.field.empty())
        return std::nullopt;
    auto const& place = row->version;

    // An empty version is no version: a title reading `fastcached ` would lose the fact silently, where
    // the absent marker says it.
    auto const stated = [](std::optional<std::string> text) {
        return text.has_value() && !text->empty() ? std::move(text) : std::nullopt;
    };
    if (place.label.empty())
    {
        auto const* const field = FindField(reading, place.field);
        return field == nullptr || field->value.kind == CellKind::Absent ? std::nullopt : stated(field->value.lexical);
    }
    for (auto const& field: reading.fields)
    {
        auto const name = std::string_view { field.name };
        if (name.starts_with(place.field) && name.substr(place.field.size()).starts_with('{'))
            if (auto version = stated(LabelValue(name, place.label)); version.has_value())
                return version;
    }
    return std::nullopt;
}

Value ParseInfo(std::string_view body)
{
    std::vector<Field> fields;
    for (auto const line: Lines(body))
    {
        auto const trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#')
            continue;
        auto const colon = trimmed.find(':');
        if (colon == std::string_view::npos)
            continue;
        auto const name = Trim(trimmed.substr(0, colon));
        auto const value = Trim(trimmed.substr(colon + 1));
        if (name.empty())
            continue;
        fields.push_back(Field { .name = std::string { name }, .value = ClassifiedCell(value) });
    }
    return RecordValue(std::move(fields));
}

Answer ChooseStats(std::span<StatsAttempt const> attempts)
{
    /// Find this origin's attempt, if it was reported at all.
    auto const attemptFor = [attempts](StatsOrigin origin) -> StatsAttempt const* {
        auto const hit = std::ranges::find_if(attempts, [origin](StatsAttempt const& a) { return a.origin == origin; });
        return hit == std::ranges::end(attempts) ? nullptr : &*hit;
    };

    // Ladder order is StatsOriginTable's order, richest first.
    for (auto const& row: StatsOriginTable)
    {
        auto const* const attempt = attemptFor(row.origin);
        if (attempt == nullptr || !attempt->record.has_value())
            continue;

        auto chosen = *attempt->record;
        // The source is prepended rather than appended so it is the first thing a
        // human sees and the first key in the JSON object.
        chosen.fields.insert(
            chosen.fields.begin(),
            Field { .name = std::string { StatsSourceFieldName }, .value = TextCell(std::string { row.name }) });

        auto answer = Answered(std::move(chosen));
        if (!row.caveat.empty())
        {
            // The count is what this source RETURNED, read off the record a moment ago,
            // never a constant: the number belongs to the daemon's `INFO` handler and a
            // copy of it here is a claim nothing checks. Taken from the attempt rather
            // than from `chosen`, whose `source` field was just prepended -- that
            // off-by-one is the whole reason two neighbouring numbers (7 and 8) were
            // circulating for one fact.
            answer.advisories.emplace_back(
                std::format("{} returned {} field(s); {}", row.what, attempt->record->fields.size(), row.caveat));
        }

        // Say what the richer sources did, but only the ones that were actually
        // tried: reporting a failure for an endpoint nothing dialled sends an
        // operator to check a listener that was never contacted.
        for (auto const& other: StatsOriginTable)
        {
            if (other.origin == row.origin)
                break;
            auto const* const skipped = attemptFor(other.origin);
            if (skipped == nullptr)
                continue;
            if (skipped->asked && !skipped->note.empty())
                answer.advisories.emplace_back(std::format("{} did not answer: {}", other.what, skipped->note));
            else if (!skipped->asked && !skipped->note.empty())
                answer.advisories.emplace_back(std::format("{} was not asked: {}", other.what, skipped->note));
        }
        return answer;
    }

    // Nothing answered. Report every source by name and in which of the three states
    // it ended, because "stats unavailable" alone does not say whether to start a
    // listener, fix a port, or look at the daemon.
    auto answer = Concluded(Outcome::Unreachable, "no stats source answered");
    for (auto const& row: StatsOriginTable)
    {
        auto const* const attempt = attemptFor(row.origin);
        if (attempt == nullptr)
        {
            answer.advisories.emplace_back(std::format("{} was not reported on", row.what));
            continue;
        }
        auto const detail = attempt->note.empty() ? std::string { "no reason given" } : attempt->note;
        answer.advisories.emplace_back(
            std::format("{} {}: {}", row.what, attempt->asked ? "did not answer" : "was not asked", detail));
    }
    return answer;
}

} // namespace FastCache::Cli
