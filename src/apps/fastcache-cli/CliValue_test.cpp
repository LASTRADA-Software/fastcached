// SPDX-License-Identifier: Apache-2.0
#include "CliValue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;

TEST_CASE("an absent cell is not an empty text cell", "[cli][value]")
{
    // The distinction the whole type exists for. Both render as "nothing" in a naive
    // formatter, and only one of them is a claim about the world -- so the assertion
    // is on the KIND, which is what every formatter branches on.
    auto const absent = AbsentCell();
    auto const empty = TextCell("");

    CHECK(absent.kind == CellKind::Absent);
    CHECK(empty.kind == CellKind::Text);
    CHECK(absent.lexical.empty());
    CHECK(empty.lexical.empty());
    // Equal lexically, different in kind. A test comparing only the text would pass
    // with the two collapsed, which is the defect.
    CHECK(absent.lexical == empty.lexical);
    CHECK(absent.kind != empty.kind);
}

TEST_CASE("a zero counter is a number, not an absence", "[cli][value]")
{
    // The converse rule, and the one that gets forgotten: a counter's zero is the
    // truth about events that never happened.
    auto const zero = NumberCell(std::uint64_t { 0 });
    CHECK(zero.kind == CellKind::Number);
    CHECK(zero.lexical == "0");
    CHECK(zero.kind != AbsentCell().kind);
}

TEST_CASE("bytes that are not UTF-8 are classified as binary, not repaired", "[cli][value]")
{
    // A lone 0x80 continuation byte is not valid UTF-8. The rule is that text a peer
    // sent is carried or refused, never silently repaired -- so the cell must say
    // `Binary` rather than substituting U+FFFD and looking like text.
    std::string const invalid = { 'a', '\x80', 'b' };
    auto const cell = TextCell(invalid);

    CHECK(cell.kind == CellKind::Binary);
    // Base64 of "a\x80b". The assertion is that the ORIGINAL bytes are recoverable,
    // which a repair would have destroyed.
    CHECK(cell.lexical == "YYBi");
}

TEST_CASE("valid UTF-8 stays text, including multi-byte sequences", "[cli][value]")
{
    // The positive control for the case above. Without it, a classifier that called
    // everything binary would pass that test.
    auto const ascii = TextCell("plain");
    CHECK(ascii.kind == CellKind::Text);
    CHECK(ascii.lexical == "plain");

    auto const multibyte = TextCell("caf\xc3\xa9");
    CHECK(multibyte.kind == CellKind::Text);
    CHECK(multibyte.lexical == "caf\xc3\xa9");
}

TEST_CASE("WellFormed refuses a table row narrower than its header", "[cli][value]")
{
    auto const good = TableValue({ "a", "b" }, { { TextCell("1"), TextCell("2") } });
    CHECK(WellFormed(good));

    auto ragged = good;
    ragged.rows.emplace_back(std::vector<Cell> { TextCell("only one") });
    // The one way this model can be malformed, and the one that makes a formatter read
    // past a row's end.
    CHECK_FALSE(WellFormed(ragged));
}

TEST_CASE("WellFormed refuses members that contradict the shape", "[cli][value]")
{
    auto scalar = ScalarValue(TextCell("x"));
    CHECK(WellFormed(scalar));
    scalar.fields.emplace_back(Field { .name = "stray", .value = TextCell("y") });
    CHECK_FALSE(WellFormed(scalar));

    auto record = RecordValue({ Field { .name = "a", .value = TextCell("1") } });
    CHECK(WellFormed(record));
    record.columns.emplace_back("stray");
    CHECK_FALSE(WellFormed(record));
}

TEST_CASE("FindField finds by name and only in a record", "[cli][value]")
{
    auto const record = RecordValue({
        Field { .name = "first", .value = NumberCell(std::uint64_t { 1 }) },
        Field { .name = "second", .value = AbsentCell() },
    });

    REQUIRE(FindField(record, "first") != nullptr);
    CHECK(FindField(record, "first")->value.lexical == "1");
    // A field that exists and is absent is found, and its cell is absent. Returning
    // null for it would make "no such field" and "the field reported nothing" the
    // same answer.
    REQUIRE(FindField(record, "second") != nullptr);
    CHECK(FindField(record, "second")->value.kind == CellKind::Absent);
    CHECK(FindField(record, "third") == nullptr);

    auto const table = TableValue({ "first" }, { { TextCell("1") } });
    CHECK(FindField(table, "first") == nullptr);
}

TEST_CASE("the builders produce the shape they name", "[cli][value]")
{
    CHECK(EmptyValue().shape == Shape::Empty);
    CHECK(ScalarValue(TextCell("x")).shape == Shape::Scalar);
    CHECK(RecordValue({}).shape == Shape::Record);
    CHECK(TableValue({}, {}).shape == Shape::Table);

    CHECK(WellFormed(EmptyValue()));
    CHECK(WellFormed(RecordValue({})));
    CHECK(WellFormed(TableValue({}, {})));
}

TEST_CASE("a real cell renders with two decimals and a boolean with a word", "[cli][value]")
{
    CHECK(RealCell(1.5).lexical == "1.50");
    CHECK(RealCell(1.005).kind == CellKind::Number);
    CHECK(BooleanCell(true).lexical == "true");
    CHECK(BooleanCell(false).lexical == "false");
    CHECK(BooleanCell(false).kind == CellKind::Boolean);
    // A false boolean is NOT an absence, which is the same trap as the zero counter.
    CHECK(BooleanCell(false).kind != CellKind::Absent);
}

TEST_CASE("a signed number cell keeps its sign", "[cli][value]")
{
    CHECK(NumberCell(std::int64_t { -2 }).lexical == "-2");
    CHECK(NumberCell(std::int64_t { -2 }).kind == CellKind::Number);
}
