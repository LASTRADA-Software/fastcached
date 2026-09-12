// SPDX-License-Identifier: Apache-2.0
#include "StatsSource.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The field naming which source answered.
    ///
    /// On stdout rather than only in a remark: *which numbers am I looking at* is a
    /// question a script asks too, and a dashboard that cannot tell a 102-counter
    /// reading from a 7-field one will draw the second as if the missing 95 were zero.
    constexpr std::string_view SourceFieldName = "source";

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
        chosen.fields.insert(chosen.fields.begin(),
                             Field { .name = std::string { SourceFieldName }, .value = TextCell(std::string { row.name }) });

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
