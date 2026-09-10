// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file CliValue.hpp
/// The one result model every `fastcache-cli` command produces and every output
/// format consumes.
///
/// **Why a shared model rather than a renderer per command per format.** This tool
/// answers in five registers -- a human table, JSON, `key=value`, TSV and CSV. Five
/// formats times a couple of dozen verbs is a hundred-odd render sites, and the
/// interesting property is one that has to hold at every single one of them: *absent
/// is not zero*. Written per site it holds until the first site somebody writes in a
/// hurry, and the failure is silent -- a `0` where nothing was reported reads as a
/// fact. So every command builds a `Value` and the formatters walk it, which makes
/// the property structural rather than remembered.

/// What a cell holds.
///
/// **A private enum**: it is never transmitted, never persisted, and no ordinal of it
/// is read from bytes or from a position, so it carries no explicit values. Stating
/// that is not a formality here -- eighteen enums in this tree do have an ordinal on a
/// wire or in a file, eight of them once said nothing about it, and an explicit `= N`
/// on an enum that needs none asserts a contract that does not exist.
///
/// The kind is carried separately from the text because two formats need it: JSON
/// decides quoting from it, and the human renderer right-aligns numbers. Rendering
/// the value to text at construction and keeping the kind beside it is cheaper than
/// a variant, and it keeps the whole model printable.
enum class CellKind : std::uint8_t
{
    /// Nothing was reported for this cell, and that is different from a zero.
    ///
    /// **This is the enumerator the whole type exists for.** A tier the cache does
    /// not run, a field the chosen stats source cannot supply, a worker that has not
    /// stated its endpoint -- none of those is a reading, and a renderer that flattens
    /// them to `0` publishes a claim about the world: `cores 0` says a machine has no
    /// CPU, and a cache that has served no reads has *no* hit rate rather than one of
    /// 0%.
    ///
    /// The converse matters just as much and points the other way: a **counter** is a
    /// tally of events, so zero is the truth about events that never happened and a
    /// counter must render `0` rather than absent. Which of the two a given number is
    /// is the caller's decision -- this type only has to be able to say either.
    Absent,
    Text,    ///< A string that is valid UTF-8. Quoted by the formats that quote.
    Number,  ///< Already rendered lexically; emitted unquoted by JSON.
    Boolean, ///< `true` or `false` lexically; emitted unquoted by JSON.

    /// Bytes that are not valid UTF-8, held base64-encoded in `lexical`.
    ///
    /// A cache value is an arbitrary byte string -- `set` accepts one and nothing in
    /// the protocol says it is text -- so `get` can hand back bytes no text format can
    /// carry. The three ways to handle that are to emit them raw and corrupt the
    /// document, to substitute `U+FFFD` and lose them silently, or to encode them and
    /// say so. Only the third is honest, and the rule it follows is the tree's own:
    /// text a peer sent is refused or carried, never *repaired* -- a repair is the
    /// failure that is quiet.
    ///
    /// The caller pairs this with an advisory on stderr and, where the shape allows,
    /// an `encoding` field on stdout, so a machine consumer is told rather than left
    /// to guess. `get --raw` bypasses this type entirely and writes the bytes.
    Binary,
    Last,
};

/// One cell of a record or a table.
struct Cell
{
    CellKind kind { CellKind::Absent }; ///< What this cell holds.
    std::string lexical {};             ///< The value as text; empty iff `kind == Absent`.
};

/// A cell nobody reported a value for.
/// @return An absent cell. See CellKind::Absent for why this is not a zero.
[[nodiscard]] Cell AbsentCell() noexcept;

/// A cell holding text.
///
/// The caller is asserting the bytes are valid UTF-8. `TextOrBinaryCell` is what to
/// use when that is not already known.
/// @param text The value.
/// @return The cell.
[[nodiscard]] Cell TextCell(std::string text);

/// A cell holding bytes, classified by whether they are text.
///
/// The one place the UTF-8 question is asked, so no command can forget to ask it.
/// @param bytes The raw value.
/// @return A `Text` cell when @p bytes is valid UTF-8, a base64 `Binary` cell otherwise.
[[nodiscard]] Cell TextOrBinaryCell(std::string_view bytes);

/// A cell holding an unsigned number.
/// @param value The value.
/// @return The cell.
[[nodiscard]] Cell NumberCell(std::uint64_t value);

/// A cell holding a signed number.
/// @param value The value.
/// @return The cell.
[[nodiscard]] Cell NumberCell(std::int64_t value);

/// A cell holding a real number.
/// @param value The value.
/// @return The cell, rendered with a fixed two-decimal precision.
[[nodiscard]] Cell RealCell(double value);

/// A cell holding a boolean.
/// @param value The value.
/// @return The cell.
[[nodiscard]] Cell BooleanCell(bool value);

/// What overall shape an answer takes.
///
/// A private enum, as CellKind above; no ordinal of it travels.
enum class Shape : std::uint8_t
{
    Empty,  ///< The command succeeded and has nothing to report (`flush`, `set`).
    Scalar, ///< One value (`get`, `ttl`, `incr`).
    Record, ///< Named fields in a stated order (`stats`, `info`, `meta`).
    Table,  ///< Columns and rows (`fleet`, `cluster status`, `mget`).
    Last,
};

/// One named field of a record.
struct Field
{
    std::string name {}; ///< The field name, as both a human label and a machine key.
    Cell value {};       ///< Its value.
};

/// A command's answer, in whatever shape that command has.
///
/// Only the members its `shape` names are meaningful; `WellFormed` states the
/// invariant and the tests hold every builder to it.
struct Value
{
    Shape shape { Shape::Empty };           ///< Which members below are meaningful.
    Cell scalar {};                         ///< Meaningful iff `shape == Scalar`.
    std::vector<Field> fields {};           ///< Meaningful iff `shape == Record`.
    std::vector<std::string> columns {};    ///< Meaningful iff `shape == Table`.
    std::vector<std::vector<Cell>> rows {}; ///< Meaningful iff `shape == Table`; each row is `columns.size()` wide.
};

/// An answer with nothing to report.
/// @return The value.
[[nodiscard]] Value EmptyValue() noexcept;

/// An answer that is one value.
/// @param cell The value.
/// @return The value.
[[nodiscard]] Value ScalarValue(Cell cell);

/// An answer that is a list of named fields.
/// @param fields The fields, in the order they should be reported.
/// @return The value.
[[nodiscard]] Value RecordValue(std::vector<Field> fields);

/// An answer that is a table.
/// @param columns The column names, in order.
/// @param rows The rows; every row must be `columns.size()` wide.
/// @return The value.
[[nodiscard]] Value TableValue(std::vector<std::string> columns, std::vector<std::vector<Cell>> rows);

/// Whether a value's members agree with its shape.
///
/// Exists because a table whose rows are narrower than its columns is the one way
/// this model can be malformed, and a formatter reading past a row's end is a
/// crash rather than a wrong answer. Asserted by the builders' tests rather than
/// at every render.
/// @param value The value to check.
/// @return True when the value is internally consistent.
[[nodiscard]] bool WellFormed(Value const& value) noexcept;

/// Look a record field up by name.
///
/// A linear scan, deliberately: a record here is a handful of fields at most, and
/// keeping `fields` an ordered vector is what makes the *reported order* a property
/// of the value rather than of whatever a map happened to hash to.
/// @param value The record to search; any other shape finds nothing.
/// @param name The field name.
/// @return The field, or nullptr. Non-owning; valid while @p value is unmodified.
[[nodiscard]] Field const* FindField(Value const& value, std::string_view name) noexcept;

} // namespace FastCache::Cli
