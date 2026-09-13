// SPDX-License-Identifier: Apache-2.0
#include "LivePipedView.hpp"

#include <algorithm>
#include <cassert>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The cells of @p record in @p columns' order: absent where the record names no such field.
    /// @param record The record to read.
    /// @param columns The header's names.
    /// @return One cell per column.
    [[nodiscard]] std::vector<Cell> CellsFor(Value const& record, std::vector<std::string> const& columns)
    {
        std::vector<Cell> cells;
        cells.reserve(columns.size());
        for (auto const& column: columns)
        {
            auto const* field = FindField(record, column);
            cells.push_back(field == nullptr ? AbsentCell() : field->value);
        }
        return cells;
    }

    /// A row of @p count absent cells: a sample that failed.
    /// @param count How many columns.
    /// @return The cells.
    [[nodiscard]] std::vector<Cell> Gap(std::size_t count)
    {
        return std::vector<Cell>(count, AbsentCell());
    }

    /// Append @p count spaces.
    /// @param count How many.
    /// @param out Where.
    void AppendPadding(std::size_t count, std::string& out)
    {
        out.append(count, ' ');
    }
} // namespace

Value LatestReading(DashboardModel const& model)
{
    if (!model.latest.has_value() || model.latest->shape != Shape::Record)
        return RecordValue({});
    return *model.latest;
}

PipedRecordView::PipedRecordView(OutputFormat format, std::optional<std::string> absentOverride, FigureProjection project):
    _format { format },
    _absentOverride { std::move(absentOverride) },
    _project { project }
{
    assert(_project != nullptr && "a piped view needs a projection");
}

std::string_view PipedRecordView::AbsentText() const noexcept
{
    if (_absentOverride.has_value() && _format != OutputFormat::Json)
        return *_absentOverride;
    return DescriptorOf(_format)->absentText;
}

std::string PipedRecordView::Frame(DashboardModel const& model)
{
    if (!model.latest.has_value())
        return {};

    // A failure is what leaves the run at zero; an accepted reading always leaves it at one or more.
    auto const gap = model.runLength == 0;
    auto const record = _project(model);

    std::string out;
    if (_columns.empty())
    {
        for (auto const& field: record.fields)
            _columns.push_back(field.name);
        if (_columns.empty())
            return {};

        auto const first = gap ? Gap(_columns.size()) : CellsFor(record, _columns);
        for (auto const index: std::views::iota(std::size_t { 0 }, _columns.size()))
        {
            auto const text =
                first[index].kind == CellKind::Absent ? AbsentText() : std::string_view { first[index].lexical };
            _widths.push_back(std::max(_columns[index].size(), text.size()));
        }

        switch (_format)
        {
            case OutputFormat::Human:
                for (auto const index: std::views::iota(std::size_t { 0 }, _columns.size()))
                {
                    out += _columns[index];
                    if (index + 1 < _columns.size())
                        AppendPadding(_widths[index] - _columns[index].size() + 2, out);
                }
                out += '\n';
                break;
            case OutputFormat::Tsv:
            case OutputFormat::Csv:
                // The header is a row of names, written by the same rule as every row under it.
                {
                    std::vector<Cell> names;
                    names.reserve(_columns.size());
                    for (auto const& column: _columns)
                        names.push_back(TextCell(column));
                    out += Row(names);
                }
                break;
            case OutputFormat::Json:
            case OutputFormat::Kv:
            case OutputFormat::Last:
                break;
        }
    }

    out += Row(gap ? Gap(_columns.size()) : CellsFor(record, _columns));
    ++_rows;
    return out;
}

std::string PipedRecordView::Row(std::vector<Cell> const& cells) const
{
    auto const absent = AbsentText();
    auto const text = [&](Cell const& cell) {
        return cell.kind == CellKind::Absent ? absent : std::string_view { cell.lexical };
    };

    std::string out;
    switch (_format)
    {
        case OutputFormat::Human:
            for (auto const index: std::views::iota(std::size_t { 0 }, cells.size()))
            {
                auto const cellText = text(cells[index]);
                auto const pad = _widths[index] > cellText.size() ? _widths[index] - cellText.size() : 0;
                auto const last = index + 1 == cells.size();
                // Numbers right-aligned under their heading, as the human table renderer does. No
                // trailing blanks on the last column: a line ending in spaces is one a diff mangles.
                if (cells[index].kind == CellKind::Number)
                    AppendPadding(pad, out);
                out += cellText;
                if (!last)
                    AppendPadding((cells[index].kind == CellKind::Number ? 0 : pad) + 2, out);
            }
            out += '\n';
            break;
        case OutputFormat::Tsv:
        case OutputFormat::Csv:
            for (auto const index: std::views::iota(std::size_t { 0 }, cells.size()))
            {
                if (index != 0)
                    out += _format == OutputFormat::Tsv ? '\t' : ',';
                out += _format == OutputFormat::Tsv ? EscapeTsvField(text(cells[index])) : QuoteCsvField(text(cells[index]));
            }
            out += '\n';
            break;
        case OutputFormat::Json: {
            // One document per line: the record renderer writes no newline inside a document.
            std::vector<Field> fields;
            fields.reserve(cells.size());
            for (auto const index: std::views::iota(std::size_t { 0 }, cells.size()))
                fields.push_back(Field { .name = _columns[index], .value = cells[index] });
            out += RenderValue(RecordValue(std::move(fields)), RenderOptions { .format = OutputFormat::Json });
            break;
        }
        case OutputFormat::Kv:
            if (_rows != 0)
                out += '\n';
            for (auto const index: std::views::iota(std::size_t { 0 }, cells.size()))
                out += std::format("{}={}\n", _columns[index], text(cells[index]));
            break;
        case OutputFormat::Last:
            break;
    }
    return out;
}

} // namespace FastCache::Cli
