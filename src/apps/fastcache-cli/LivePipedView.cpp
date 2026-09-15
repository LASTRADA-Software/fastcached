// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanels.hpp"
#include "LivePipedView.hpp"

#include <FastCache/Core/FigureText.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
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

    /// @p value as a piped cell, written for a program by the one writer the leader's documents use too.
    /// @param value The value.
    /// @param format How the panel writes it.
    /// @return The cell.
    [[nodiscard]] Cell RawFigureCell(double value, FigureFormat format)
    {
        return Cell { .kind = CellKind::Number, .lexical = WriteMachineFigure(value, format) };
    }
    /// The newest cell of @p figure's series: what the panel draws for it now.
    /// @param figure The figure.
    /// @param model What is known.
    /// @param tier A tier's name for a tier column's figure, or empty.
    /// @return The value, or absent.
    [[nodiscard]] Cell NewestCell(FigureSpec const& figure, DashboardModel const& model, std::string_view tier = {})
    {
        if (model.history.empty())
            return AbsentCell();
        auto const series = FigureSeries(model.history, figure, tier);
        return series.empty() || !series.back().has_value() ? AbsentCell() : RawFigureCell(*series.back(), figure.format);
    }

} // namespace

Value PanelFigures(PanelSpec const& panel, DashboardModel const& model)
{
    auto fields = std::vector<Field> {};
    fields.push_back(Field { .name = "source",
                             .value = model.latestStamp.has_value() ? TextCell(model.latestStamp->source) : AbsentCell() });

    for (auto const& row: panel.rates)
    {
        fields.push_back(Field { .name = std::string { row.key }, .value = NewestCell(row.figure, model) });
        for (auto const& beside: row.beside)
            fields.push_back(Field { .name = std::string { beside.key }, .value = NewestCell(beside.figure, model) });
        for (auto const& part: row.split)
            fields.push_back(Field { .name = std::string { part.key }, .value = NewestCell(part.figure, model) });
    }
    for (auto const& row: panel.levels)
    {
        if (!row.value.field.Names())
            continue;
        fields.push_back(Field { .name = std::string { row.key }, .value = NewestCell(row.value, model) });
        if (row.limit.has_value())
            fields.push_back(Field { .name = std::string { row.limitKey }, .value = NewestCell(*row.limit, model) });
    }
    for (auto const& block: panel.facts)
        for (auto const& line: block.lines)
            for (auto const& cell: line.cells)
                for (auto const& figure: cell.figures)
                    fields.push_back(
                        Field { .name = std::string { figure.key }, .value = NewestCell(figure.figure, model) });
    if (model.stats.has_value())
        for (auto const tier: TiersIn(panel, *model.stats))
            for (auto const& column: panel.tierColumns)
                fields.push_back(
                    Field { .name = TierFigureKey(tier, column.key), .value = NewestCell(column.figure, model, tier) });
    return RecordValue(std::move(fields));
}

Value CacheFigures(DashboardModel const& model)
{
    return PanelFigures(CachePanel(), model);
}

Value NodeFigures(DashboardModel const& model)
{
    return PanelFigures(NodePanel(), model);
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

DashboardFrame PipedRecordView::PlacedFrame(DashboardModel const& model)
{
    return DashboardFrame { .text = Lines(model), .placements = {} };
}

std::string PipedRecordView::Lines(DashboardModel const& model)
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
