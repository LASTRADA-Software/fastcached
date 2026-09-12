// SPDX-License-Identifier: Apache-2.0
#include "CliFormat.hpp"

#include <FastCache/Distributed/FleetText.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The separator a tabular format puts between fields.
    struct Separator
    {
        std::string_view text;
    };

    /// What an absent cell renders as in this format, honouring `--absent`.
    ///
    /// JSON is excluded by the caller rather than here: it has a real null and the
    /// override must not be able to turn that into a string that parses and lies.
    /// @param options The render options.
    /// @param spec The format's row.
    /// @return The text to emit for an absent cell.
    [[nodiscard]] std::string_view AbsentTextFor(RenderOptions const& options, FormatSpec const& spec) noexcept
    {
        if (options.absentOverride.has_value())
            return *options.absentOverride;
        return spec.absentText;
    }

    /// One cell's text in a non-JSON format.
    /// @param cell The cell.
    /// @param absent What an absent cell renders as.
    /// @return The text.
    [[nodiscard]] std::string_view PlainText(Cell const& cell, std::string_view absent) noexcept
    {
        return cell.kind == CellKind::Absent ? absent : std::string_view { cell.lexical };
    }

    /// Escape a string for a JSON document.
    ///
    /// Control characters below 0x20 are escaped because a raw one makes the document
    /// invalid rather than merely ugly, and a cache key may contain anything.
    /// @param text The raw text.
    /// @param out Destination; the quotes are written too.
    void AppendJsonString(std::string_view text, std::string& out)
    {
        out += '"';
        for (auto const ch: text)
        {
            switch (ch)
            {
                case '"':
                    out += R"(\")";
                    continue;
                case '\\':
                    out += R"(\\)";
                    continue;
                case '\n':
                    out += R"(\n)";
                    continue;
                case '\r':
                    out += R"(\r)";
                    continue;
                case '\t':
                    out += R"(\t)";
                    continue;
                case '\b':
                    out += R"(\b)";
                    continue;
                case '\f':
                    out += R"(\f)";
                    continue;
                default:
                    break;
            }
            if (static_cast<unsigned char>(ch) < 0x20U)
                out += std::format(R"(\u{:04x})", static_cast<unsigned>(static_cast<unsigned char>(ch)));
            else
                out += ch;
        }
        out += '"';
    }

    /// One cell as a JSON value.
    ///
    /// A `Number` or `Boolean` cell is emitted bare, which is what makes `jq` able to
    /// compare it; everything else is a string. An absent cell is `null` -- the
    /// enumerator this whole model exists for, and the reason `--absent` cannot reach
    /// this format.
    /// @param cell The cell.
    /// @param out Destination.
    void AppendJsonCell(Cell const& cell, std::string& out)
    {
        switch (cell.kind)
        {
            case CellKind::Absent:
                out += "null";
                return;
            case CellKind::Number:
            case CellKind::Boolean:
                out += cell.lexical;
                return;
            case CellKind::Text:
            case CellKind::Binary:
                AppendJsonString(cell.lexical, out);
                return;
            case CellKind::Last:
                break;
        }
        out += "null";
    }

    /// Whether every value present in a column is a number, so it should right-align.
    /// @param rows The table's rows.
    /// @param column The column index.
    /// @return True when the column holds only numbers and absences.
    [[nodiscard]] bool ColumnIsNumeric(std::vector<std::vector<Cell>> const& rows, std::size_t column) noexcept
    {
        auto sawNumber = false;
        for (auto const& row: rows)
        {
            auto const& cell = row[column];
            if (cell.kind == CellKind::Absent)
                continue;
            if (cell.kind != CellKind::Number)
                return false;
            sawNumber = true;
        }
        return sawNumber;
    }

    /// Append `count` spaces.
    /// @param count How many.
    /// @param out Destination.
    void AppendPadding(std::size_t count, std::string& out)
    {
        out.append(count, ' ');
    }

    /// Render a record as aligned `name  value` lines.
    ///
    /// The colour escapes are emitted OUTSIDE the padding arithmetic, exactly as
    /// `Cli/UsageDoc` does it, so the plain and coloured forms are byte-identical once
    /// the escapes are stripped -- otherwise a table looks aligned in one and ragged in
    /// the other.
    /// @param value The record.
    /// @param options The render options.
    /// @return The rendered text.
    [[nodiscard]] std::string RenderHumanRecord(Value const& value, RenderOptions const& options)
    {
        auto const& palette = PaletteFor(options.color);
        auto const absent = AbsentTextFor(options, *DescriptorOf(options.format));

        std::size_t nameColumn = 0;
        for (auto const& field: value.fields)
            nameColumn = std::max(nameColumn, field.name.size());

        std::string out;
        for (auto const& field: value.fields)
        {
            out += palette.term;
            out += field.name;
            out += palette.reset;
            AppendPadding(nameColumn - field.name.size() + 2, out);
            out += PlainText(field.value, absent);
            out += '\n';
        }
        return out;
    }

    /// Render a table as an aligned header plus rows.
    /// @param value The table.
    /// @param options The render options.
    /// @return The rendered text.
    [[nodiscard]] std::string RenderHumanTable(Value const& value, RenderOptions const& options)
    {
        auto const& palette = PaletteFor(options.color);
        auto const absent = AbsentTextFor(options, *DescriptorOf(options.format));
        auto const columnCount = value.columns.size();

        std::vector<std::size_t> widths;
        widths.reserve(columnCount);
        for (auto const index: std::views::iota(std::size_t { 0 }, columnCount))
        {
            auto width = value.columns[index].size();
            for (auto const& row: value.rows)
                width = std::max(width, PlainText(row[index], absent).size());
            widths.push_back(width);
        }

        std::vector<bool> numeric;
        numeric.reserve(columnCount);
        for (auto const index: std::views::iota(std::size_t { 0 }, columnCount))
            numeric.push_back(ColumnIsNumeric(value.rows, index));

        auto const appendPadded = [&](std::string_view text, std::size_t index, std::string& out) {
            auto const pad = widths[index] - text.size();
            if (numeric[index])
                AppendPadding(pad, out);
            out += text;
            // No trailing whitespace on the last column: a line that ends in spaces
            // is one a diff and a copy-paste both mangle.
            if (index + 1 < columnCount && !numeric[index])
                AppendPadding(pad, out);
            if (index + 1 < columnCount)
                AppendPadding(2, out);
        };

        std::string out;
        for (auto const index: std::views::iota(std::size_t { 0 }, columnCount))
        {
            // The header is coloured, so its padding is computed from the plain name
            // and the escapes bracket only the text.
            auto const& name = value.columns[index];
            auto const pad = widths[index] - name.size();
            out += palette.heading;
            out += name;
            out += palette.reset;
            if (index + 1 < columnCount)
            {
                AppendPadding(pad + 2, out);
            }
        }
        out += '\n';

        for (auto const& row: value.rows)
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, columnCount))
                appendPadded(PlainText(row[index], absent), index, out);
            out += '\n';
        }
        return out;
    }

    /// Render any shape in the human format.
    /// @param value The answer.
    /// @param options The render options.
    /// @return The rendered text.
    [[nodiscard]] std::string RenderHuman(Value const& value, RenderOptions const& options)
    {
        auto const absent = AbsentTextFor(options, *DescriptorOf(options.format));
        switch (value.shape)
        {
            case Shape::Empty:
                return {};
            case Shape::Scalar:
                return std::format("{}\n", PlainText(value.scalar, absent));
            case Shape::Record:
                return RenderHumanRecord(value, options);
            case Shape::Table:
                return RenderHumanTable(value, options);
            case Shape::Last:
                break;
        }
        return {};
    }

    /// Render any shape as JSON.
    ///
    /// A table becomes an array of objects keyed by column name, which is the shape
    /// `/fleet.json` already emits for its column tables -- so a consumer that learned
    /// one learned the other, and `jq` needs no positional indexing.
    /// @param value The answer.
    /// @return The rendered document, ending in a newline.
    [[nodiscard]] std::string RenderJson(Value const& value)
    {
        std::string out;
        switch (value.shape)
        {
            case Shape::Empty:
                out += "null";
                break;
            case Shape::Scalar:
                AppendJsonCell(value.scalar, out);
                break;
            case Shape::Record: {
                out += '{';
                auto first = true;
                for (auto const& field: value.fields)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    AppendJsonString(field.name, out);
                    out += ':';
                    AppendJsonCell(field.value, out);
                }
                out += '}';
                break;
            }
            case Shape::Table: {
                out += '[';
                auto firstRow = true;
                for (auto const& row: value.rows)
                {
                    if (!firstRow)
                        out += ',';
                    firstRow = false;
                    out += '{';
                    for (auto const index: std::views::iota(std::size_t { 0 }, value.columns.size()))
                    {
                        if (index != 0)
                            out += ',';
                        AppendJsonString(value.columns[index], out);
                        out += ':';
                        AppendJsonCell(row[index], out);
                    }
                    out += '}';
                }
                out += ']';
                break;
            }
            case Shape::Last:
                out += "null";
                break;
        }
        out += '\n';
        return out;
    }

    /// Render any shape as `name=value` lines.
    ///
    /// A table becomes one `column=value` line per cell with a blank line between
    /// rows, so a shell loop can read it a record at a time.
    /// @param value The answer.
    /// @param options The render options.
    /// @return The rendered text.
    [[nodiscard]] std::string RenderKv(Value const& value, RenderOptions const& options)
    {
        auto const absent = AbsentTextFor(options, *DescriptorOf(options.format));
        std::string out;
        switch (value.shape)
        {
            case Shape::Empty:
                break;
            case Shape::Scalar:
                out += std::format("{}\n", PlainText(value.scalar, absent));
                break;
            case Shape::Record:
                for (auto const& field: value.fields)
                    out += std::format("{}={}\n", field.name, PlainText(field.value, absent));
                break;
            case Shape::Table: {
                auto firstRow = true;
                for (auto const& row: value.rows)
                {
                    if (!firstRow)
                        out += '\n';
                    firstRow = false;
                    for (auto const index: std::views::iota(std::size_t { 0 }, value.columns.size()))
                        out += std::format("{}={}\n", value.columns[index], PlainText(row[index], absent));
                }
                break;
            }
            case Shape::Last:
                break;
        }
        return out;
    }

    /// Render any shape as separated fields with a header line.
    /// @param value The answer.
    /// @param options The render options.
    /// @param separator What goes between fields.
    /// @param quote Applied to each field; identity for TSV, RFC 4180 for CSV.
    /// @return The rendered text.
    [[nodiscard]] std::string RenderSeparated(Value const& value,
                                              RenderOptions const& options,
                                              Separator separator,
                                              std::string (*quote)(std::string_view))
    {
        auto const absent = AbsentTextFor(options, *DescriptorOf(options.format));

        auto const line = [&](std::span<std::string const> cells) {
            std::string out;
            for (auto const index: std::views::iota(std::size_t { 0 }, cells.size()))
            {
                if (index != 0)
                    out += separator.text;
                out += quote(cells[index]);
            }
            out += '\n';
            return out;
        };

        std::string out;
        switch (value.shape)
        {
            case Shape::Empty:
                break;
            case Shape::Scalar: {
                auto const header = std::vector<std::string> { "value" };
                auto const row = std::vector<std::string> { std::string { PlainText(value.scalar, absent) } };
                out += line(header);
                out += line(row);
                break;
            }
            case Shape::Record: {
                auto const header = std::vector<std::string> { "name", "value" };
                out += line(header);
                for (auto const& field: value.fields)
                {
                    auto const row = std::vector<std::string> { field.name, std::string { PlainText(field.value, absent) } };
                    out += line(row);
                }
                break;
            }
            case Shape::Table: {
                out += line(value.columns);
                for (auto const& row: value.rows)
                {
                    std::vector<std::string> cells;
                    cells.reserve(row.size());
                    for (auto const& cell: row)
                        cells.emplace_back(PlainText(cell, absent));
                    out += line(cells);
                }
                break;
            }
            case Shape::Last:
                break;
        }
        return out;
    }

    /// What TSV writes for @p byte, or nothing when it may carry it as it is.
    ///
    /// The TABLE and the scan are the node's: `Distributed::DelimitedEscapes` and
    /// `Distributed::EscapeFor`, which `/fleet.txt` writes with. This file used to
    /// carry its own `TextEscape` and its own four rows, and the row type's
    /// replacement field was even named differently -- two names for one concept,
    /// which is the worse half of a duplicate because a reader who knows one file
    /// does not recognise the other (#1334).
    ///
    /// What is NOT shared is the walk, and that is deliberate rather than
    /// unfinished. `Distributed::EscapeDelimited` also spells control bytes and
    /// replaces invalid UTF-8, because `/fleet.txt` is read by a terminal; this
    /// client's TSV carries a cached value a terminal never sees, so it touches
    /// only the four. `CliFormat_test.cpp` asserts that divergence in BOTH
    /// directions, so calling the node's function here would silently change a
    /// format that was already correct -- and would redden that case, which is the
    /// test doing its job.
    ///
    /// An empty answer means "carry it literally", which is unambiguous only because
    /// no row spells a byte as nothing -- a row that wrote `""` would mean "delete
    /// this byte", which no format here wants.
    /// @param byte The byte to spell.
    /// @return Its spelling, or an empty view when it needs none.
    [[nodiscard]] constexpr std::string_view EscapeForTsv(char byte) noexcept
    {
        return Distributed::EscapeFor(Distributed::DelimitedEscapes, byte);
    }
} // namespace

FormatSpec const* DescriptorOf(OutputFormat format) noexcept
{
    auto const index = static_cast<std::size_t>(format);
    if (index >= FormatTable.size())
        return nullptr;
    return &FormatTable[index];
}

std::optional<OutputFormat> FormatFromName(std::string_view name) noexcept
{
    for (auto const& row: FormatTable)
        if (row.name == name)
            return row.format;
    return std::nullopt;
}

std::string FormatNames()
{
    std::string out;
    for (auto const& row: FormatTable)
    {
        if (!out.empty())
            out += ", ";
        out += row.name;
    }
    return out;
}

std::string QuoteCsvField(std::string_view field)
{
    auto const needsQuoting = field.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needsQuoting)
        return std::string { field };

    std::string out;
    out.reserve(field.size() + 2);
    out += '"';
    for (auto const ch: field)
    {
        if (ch == '"')
            out += '"';
        out += ch;
    }
    out += '"';
    return out;
}

std::string EscapeTsvField(std::string_view field)
{
    std::string out;
    out.reserve(field.size());
    for (auto const ch: field)
    {
        // One pass over the table, so the escape character is never escaped twice: a
        // two-pass implementation that spells tabs and then backslashes turns one tab
        // into `\\t`, which reads back as a literal backslash followed by a `t`.
        auto const replacement = EscapeForTsv(ch);
        if (replacement.empty())
            out += ch;
        else
            out += replacement;
    }
    return out;
}

std::string RenderValue(Value const& value, RenderOptions const& options)
{
    switch (options.format)
    {
        case OutputFormat::Human:
            return RenderHuman(value, options);
        case OutputFormat::Json:
            return RenderJson(value);
        case OutputFormat::Kv:
            return RenderKv(value, options);
        case OutputFormat::Tsv:
            return RenderSeparated(value, options, Separator { .text = "\t" }, &EscapeTsvField);
        case OutputFormat::Csv:
            return RenderSeparated(value, options, Separator { .text = "," }, &QuoteCsvField);
        case OutputFormat::Last:
            break;
    }
    return {};
}

} // namespace FastCache::Cli
