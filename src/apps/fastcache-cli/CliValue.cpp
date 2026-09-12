// SPDX-License-Identifier: Apache-2.0
#include "CliValue.hpp"

#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <utility>

namespace FastCache::Cli
{

Cell AbsentCell() noexcept
{
    return Cell {};
}

Cell TextCell(std::string text)
{
    if (IsValidUtf8(text))
        return Cell { .kind = CellKind::Text, .lexical = std::move(text) };

    // Encoded and SAID, rather than substituted. A renderer that repaired the bytes
    // would make a wrong key look like a real one -- the operator would see a
    // plausible key that no `get` will ever match -- where a base64 `Binary` cell
    // says what happened and the caller's advisory says why. That is this tree's
    // rule for text a peer sent: refuse it or carry it, never repair it.
    std::span<std::byte const> const raw { reinterpret_cast<std::byte const*>(text.data()), text.size() };
    return Cell { .kind = CellKind::Binary, .lexical = Base64Encode(raw) };
}

Cell NumberCell(std::uint64_t value)
{
    return Cell { .kind = CellKind::Number, .lexical = std::format("{}", value) };
}

Cell NumberCell(std::int64_t value)
{
    return Cell { .kind = CellKind::Number, .lexical = std::format("{}", value) };
}

Cell RealCell(double value)
{
    return Cell { .kind = CellKind::Number, .lexical = std::format("{:.2f}", value) };
}

Cell BooleanCell(bool value)
{
    return Cell { .kind = CellKind::Boolean, .lexical = value ? "true" : "false" };
}

Value EmptyValue() noexcept
{
    return Value {};
}

Value ScalarValue(Cell cell)
{
    return Value { .shape = Shape::Scalar, .scalar = std::move(cell) };
}

Value RecordValue(std::vector<Field> fields)
{
    return Value { .shape = Shape::Record, .fields = std::move(fields) };
}

Value TableValue(std::vector<std::string> columns, std::vector<std::vector<Cell>> rows)
{
    return Value { .shape = Shape::Table, .columns = std::move(columns), .rows = std::move(rows) };
}

bool WellFormed(Value const& value) noexcept
{
    switch (value.shape)
    {
        case Shape::Empty:
            return value.fields.empty() && value.columns.empty() && value.rows.empty()
                   && value.scalar.kind == CellKind::Absent;
        case Shape::Scalar:
            return value.fields.empty() && value.columns.empty() && value.rows.empty();
        case Shape::Record:
            return value.columns.empty() && value.rows.empty();
        case Shape::Table:
            // The whole point of the check: a row narrower than the header is a
            // formatter reading past its end.
            return value.fields.empty() && std::ranges::all_of(value.rows, [&value](auto const& row) {
                       return row.size() == value.columns.size();
                   });
        case Shape::Last:
            break;
    }
    return false;
}

Field const* FindField(Value const& value, std::string_view name) noexcept
{
    if (value.shape != Shape::Record)
        return nullptr;
    auto const hit = std::ranges::find_if(value.fields, [name](Field const& field) { return field.name == name; });
    return hit == std::ranges::end(value.fields) ? nullptr : &*hit;
}

} // namespace FastCache::Cli
