// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"

#include <format>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    /// The next line of @p text, removed from it; a trailing line without a newline counts.
    /// @param text What is left to read.
    /// @return The line, without its newline.
    [[nodiscard]] std::string_view TakeLine(std::string_view& text) noexcept
    {
        auto const end = text.find('\n');
        auto const line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view {} : text.substr(end + 1);
        return line;
    }

    /// The key a marker line names, or nothing when the line is not a marker.
    ///
    /// A marker is `#`, one space, and a key with no whitespace in it -- the shape
    /// `RenderFleetText` writes. What any other line means depends on where it sits; see
    /// `ParseFleetDocument`.
    /// @param line One line.
    /// @return The key, or nullopt.
    [[nodiscard]] std::optional<std::string_view> MarkerKey(std::string_view line) noexcept
    {
        if (!line.starts_with("# "))
            return std::nullopt;
        auto const key = line.substr(2);
        if (key.empty() || key.find_first_of(" \t") != std::string_view::npos)
            return std::nullopt;
        return key;
    }
} // namespace

std::expected<Value, std::string> FleetTable(std::string_view document)
{
    std::vector<std::string> columns;
    std::vector<std::vector<Cell>> rows;

    auto const split = [](std::string_view line) {
        std::vector<std::string_view> fields;
        while (true)
        {
            auto const at = line.find('\t');
            if (at == std::string_view::npos)
                break;
            fields.push_back(line.substr(0, at));
            line.remove_prefix(at + 1);
        }
        fields.push_back(line);
        return fields;
    };

    bool header = true;
    while (!document.empty())
    {
        auto const line = TakeLine(document);

        // A trailing newline leaves an empty tail, which is not a row of one empty
        // cell. Skipped rather than rendered, or every table gains a blank row.
        if (line.empty())
            continue;

        if (header)
        {
            for (auto const& field: split(line))
                columns.emplace_back(field);
            header = false;
            continue;
        }

        auto const fields = split(line);

        // Every renderer walks `row[index]` for each of the HEADER's columns, so a
        // short row is an out-of-range read rather than a narrow table. Refused
        // rather than padded: padding presents truncated data as complete, and an
        // `Absent` cell claims nobody reported the value when in fact it was
        // reported and lost.
        if (fields.size() != columns.size())
            return std::unexpected(std::format(
                "row {} carries {} field(s) where the header names {}", rows.size() + 1, fields.size(), columns.size()));

        std::vector<Cell> row;
        row.reserve(columns.size());
        for (auto const& field: fields)
            row.push_back(field == "-" ? AbsentCell() : TextCell(std::string { field }));
        rows.push_back(std::move(row));
    }

    // No header at all is not an empty fleet -- an empty SECTION still renders its
    // header line, which is the whole reason the renderer emits one for a table
    // with no rows. A document without one is not a table this client can read.
    if (columns.empty())
        return std::unexpected("the document carries no header line");

    return TableValue(std::move(columns), std::move(rows));
}

std::expected<FleetDocument, std::string> ParseFleetDocument(std::string_view document)
{
    auto parsed = FleetDocument {};
    auto known = std::size_t { 0 };

    // The section being read, and the text it has gathered so far. `skipping` is a section
    // this build has no row for, read past rather than parsed.
    auto current = std::optional<FleetSection> {};
    auto skipping = false;
    auto gathered = std::string {};

    auto const close = [&]() -> std::expected<void, std::string> {
        if (!current.has_value())
            return {};
        auto table = FleetTable(gathered);
        if (!table.has_value())
            return std::unexpected(std::format(
                "section `{}`: {}", Distributed::FleetSectionTable[static_cast<std::size_t>(*current)].key, table.error()));
        parsed.sections[static_cast<std::size_t>(*current)] = *std::move(table);
        current.reset();
        gathered.clear();
        return {};
    };

    auto lineNumber = std::size_t { 0 };
    while (!document.empty())
    {
        auto const line = TakeLine(document);
        ++lineNumber;

        if (auto const key = MarkerKey(line); key.has_value())
        {
            if (auto closed = close(); !closed.has_value())
                return std::unexpected(closed.error());
            auto const section = Distributed::FleetSectionFromKey(*key);
            skipping = !section.has_value();
            if (skipping)
                continue;
            if (parsed.sections[static_cast<std::size_t>(*section)].has_value())
                return std::unexpected(std::format("section `{}` appears twice", *key));
            current = section;
            ++known;
            continue;
        }

        if (line.empty())
            continue;

        // Inside a section every other line is a ROW, a leading `#` included: the first cell
        // can be text a peer chose -- a member's id, a worker's -- and the renderer writes no
        // comment inside a section, so reading one there would drop a real row in silence.
        if (current.has_value())
        {
            gathered += line;
            gathered += '\n';
            continue;
        }
        if (skipping || line.starts_with('#'))
            continue;
        return std::unexpected(std::format("line {} is outside any section", lineNumber));
    }
    if (auto closed = close(); !closed.has_value())
        return std::unexpected(closed.error());

    if (known == 0)
        return std::unexpected("the document names no fleet section this client knows");
    return parsed;
}

} // namespace FastCache::Cli
