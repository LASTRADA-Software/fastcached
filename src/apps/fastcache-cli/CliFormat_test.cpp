// SPDX-License-Identifier: Apache-2.0
#include "CliFormat.hpp"

#include <FastCache/Cli/UsageTestUtils.hpp>
#include <FastCache/Distributed/FleetText.hpp>
#include <FastCache/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Testing;

namespace
{
/// A record with one present and one absent cell -- the arrangement every
/// absent-is-not-zero assertion below needs.
/// @return The record.
[[nodiscard]] Value RecordWithAnAbsence()
{
    return RecordValue({
        Field { .name = "present", .value = NumberCell(std::uint64_t { 7 }) },
        Field { .name = "missing", .value = AbsentCell() },
        Field { .name = "counted", .value = NumberCell(std::uint64_t { 0 }) },
    });
}

/// Render with defaults for one format.
/// @param value The answer.
/// @param format The format.
/// @return The rendered text.
[[nodiscard]] std::string Render(Value const& value, OutputFormat format)
{
    return RenderValue(value, RenderOptions { .format = format });
}
} // namespace

TEST_CASE("every format is reachable by name and the name list is derived", "[cli][format]")
{
    for (auto const& row: FormatTable)
    {
        auto const found = FormatFromName(row.name);
        REQUIRE(found.has_value());
        CHECK(Unwrap(found) == row.format);
        // The list a refusal prints must name every accepted spelling, or it sends a
        // reader looking for a format that is not there.
        CHECK(FormatNames().contains(row.name));
    }
    CHECK_FALSE(FormatFromName("yaml").has_value());
}

TEST_CASE("absent renders differently from zero in every format", "[cli][format]")
{
    auto const record = RecordWithAnAbsence();

    // Human: a dash, the spelling --cluster-status already uses.
    auto const human = Render(record, OutputFormat::Human);
    CHECK(human.contains("missing  -"));
    CHECK(human.contains("counted  0"));

    // JSON: a real null, and the zero stays a bare number so `jq` can compare it.
    auto const json = Render(record, OutputFormat::Json);
    CHECK(json.contains("\"missing\":null"));
    CHECK(json.contains("\"counted\":0"));
    // The distinguishing assertion: the two must not render alike. A formatter that
    // flattened absent to 0 would satisfy "contains counted:0" and fail this.
    CHECK_FALSE(json.contains("\"missing\":0"));

    // key=value: an empty value, and a zero that is present.
    auto const kv = Render(record, OutputFormat::Kv);
    CHECK(kv.contains("missing=\n"));
    CHECK(kv.contains("counted=0\n"));

    // CSV: an empty field, with the header still there.
    auto const csv = Render(record, OutputFormat::Csv);
    CHECK(csv.contains("missing,\n"));
    CHECK(csv.contains("counted,0\n"));
}

TEST_CASE("the absent override reaches the line formats and never JSON", "[cli][format]")
{
    auto const record = RecordWithAnAbsence();
    auto const options = [](OutputFormat format) {
        return RenderOptions { .format = format, .absentOverride = std::string { "NA" } };
    };

    CHECK(RenderValue(record, options(OutputFormat::Kv)).contains("missing=NA"));
    CHECK(RenderValue(record, options(OutputFormat::Csv)).contains("missing,NA"));
    CHECK(RenderValue(record, options(OutputFormat::Tsv)).contains("missing\tNA"));
    CHECK(RenderValue(record, options(OutputFormat::Human)).contains("missing  NA"));

    // JSON has a real null available, so an override that turned it into the string
    // "NA" would hand a consumer a value that parses and lies.
    auto const json = RenderValue(record, options(OutputFormat::Json));
    CHECK(json.contains("\"missing\":null"));
    CHECK_FALSE(json.contains("NA"));
}

TEST_CASE("a table becomes an array of objects in JSON", "[cli][format]")
{
    auto const table = TableValue({ "key", "value" },
                                  {
                                      { TextCell("a"), TextCell("1") },
                                      { TextCell("b"), AbsentCell() },
                                  });

    auto const json = Render(table, OutputFormat::Json);
    CHECK(json == "[{\"key\":\"a\",\"value\":\"1\"},{\"key\":\"b\",\"value\":null}]\n");
}

TEST_CASE("JSON quotes text and leaves numbers and booleans bare", "[cli][format]")
{
    auto const record = RecordValue({
        Field { .name = "text", .value = TextCell("7") },
        Field { .name = "number", .value = NumberCell(std::uint64_t { 7 }) },
        Field { .name = "flag", .value = BooleanCell(true) },
    });

    auto const json = Render(record, OutputFormat::Json);
    // The distinction that makes `jq '.number > 3'` work and `.text > 3` not.
    CHECK(json.contains("\"text\":\"7\""));
    CHECK(json.contains("\"number\":7"));
    CHECK(json.contains("\"flag\":true"));
}

TEST_CASE("JSON escapes what would otherwise break the document", "[cli][format]")
{
    auto const value = ScalarValue(TextCell("a\"b\\c\nd\te"));
    auto const json = Render(value, OutputFormat::Json);
    CHECK(json == "\"a\\\"b\\\\c\\nd\\te\"\n");
}

TEST_CASE("JSON escapes a control character rather than emitting it raw", "[cli][format]")
{
    // A cache key may contain anything. A raw 0x01 makes the document invalid rather
    // than merely ugly, so it is not enough to handle the named escapes.
    std::string key = "a";
    key.push_back('\x01');
    auto const json = Render(ScalarValue(TextCell(key)), OutputFormat::Json);
    CHECK(json == "\"a\\u0001\"\n");
}

TEST_CASE("CSV quotes only the fields that need it, and doubles a quote", "[cli][format]")
{
    CHECK(QuoteCsvField("plain") == "plain");
    CHECK(QuoteCsvField("has,comma") == "\"has,comma\"");
    CHECK(QuoteCsvField("has\"quote") == "\"has\"\"quote\"");
    CHECK(QuoteCsvField("has\nnewline") == "\"has\nnewline\"");
    CHECK(QuoteCsvField("").empty());
}

TEST_CASE("a scalar gains a header in the tabular formats and not in the others", "[cli][format]")
{
    auto const value = ScalarValue(TextCell("hello"));

    CHECK(Render(value, OutputFormat::Human) == "hello\n");
    CHECK(Render(value, OutputFormat::Kv) == "hello\n");
    // csv and tsv always carry a header, so a consumer can read them uniformly
    // whatever the verb's shape.
    CHECK(Render(value, OutputFormat::Csv) == "value\nhello\n");
    CHECK(Render(value, OutputFormat::Tsv) == "value\nhello\n");
    CHECK(Render(value, OutputFormat::Json) == "\"hello\"\n");
}

TEST_CASE("an empty answer writes nothing, except in JSON where it is null", "[cli][format]")
{
    auto const value = EmptyValue();
    CHECK(Render(value, OutputFormat::Human).empty());
    CHECK(Render(value, OutputFormat::Kv).empty());
    CHECK(Render(value, OutputFormat::Csv).empty());
    CHECK(Render(value, OutputFormat::Tsv).empty());
    // JSON must always be a document, so "nothing to report" needs a spelling.
    CHECK(Render(value, OutputFormat::Json) == "null\n");
}

TEST_CASE("colour changes no column position", "[cli][format]")
{
    auto const table = TableValue({ "name", "count" },
                                  {
                                      { TextCell("short"), NumberCell(std::uint64_t { 1 }) },
                                      { TextCell("much-longer-name"), NumberCell(std::uint64_t { 1000 }) },
                                  });

    auto const plain = RenderValue(table, RenderOptions { .format = OutputFormat::Human });
    auto const colored = RenderValue(table, RenderOptions { .format = OutputFormat::Human, .color = UsageColor::Colored });

    // The escapes are emitted outside the padding arithmetic, so stripping them must
    // give back the plain rendering byte for byte. Getting this wrong makes a table
    // look aligned in one mode and ragged in the other.
    CHECK(StripAnsi(colored) == plain);
    CHECK(colored != plain);
}

TEST_CASE("a numeric column right-aligns and a text column does not", "[cli][format]")
{
    auto const table = TableValue({ "n" },
                                  {
                                      { NumberCell(std::uint64_t { 1 }) },
                                      { NumberCell(std::uint64_t { 1000 }) },
                                  });
    auto const human = RenderValue(table, RenderOptions { .format = OutputFormat::Human });
    // "   1" padded to the width of "1000".
    CHECK(human.contains("   1\n"));
    CHECK(human.contains("1000\n"));
}

TEST_CASE("a fleet column is written in the scale the leader's own tables give it", "[cli][format][fleet]")
{
    // #1488: the human table used to print the leader's raw integers, so `heartbeat-age` read
    // `14223` -- which under that column name says a worker was last heard from almost four
    // hours ago, when the truth was fourteen seconds, while `/fleet` rendered `14 s` from the
    // same cell. Wrong rather than merely terser, and wrong toward a false alarm.
    //
    // 90000 ms, and the value is the point of the case. The defect was noticed on this machine
    // only because the raw figure was IMPOSSIBLE -- 15965156 seconds is 185 days on a node up
    // for four hours. A fixture using an impossible number passes under a build that merely
    // mis-scales, because any reading of it looks wrong. 90000 is plausible BOTH ways: 90000 s
    // is 25 hours and 90 s is a minute and a half, and both are things a heartbeat age could
    // say. So only the scale distinguishes them.
    auto const raw = std::uint64_t { 90000 };
    auto const table = TableValue({ "id", "heartbeat-age", "last-picked-age" },
                                  {
                                      { TextCell("w1"), NumberCell(raw), AbsentCell() },
                                  });

    // The expected text comes from the leader's own writer rather than a literal. A literal
    // here would be a second statement of a format this client does not own, and it would
    // redden on any change to how a duration is written -- which is a change to the PAGE, not
    // to this client.
    auto const expected = Distributed::HumanFleetFigure(raw, Distributed::CellFormat::Millis);
    REQUIRE(expected != "90000");

    auto const human = RenderValue(
        table, RenderOptions { .format = OutputFormat::Human, .columnScales = Distributed::FleetSection::Workers });

    CHECK(human.contains(expected));
    // And the raw figure is GONE, not merely accompanied. Asserting only that the written form
    // is present passes on a build that prints both, which is the shape a careless fix takes.
    CHECK_FALSE(human.contains("90000"));

    // `last-picked-age` stays absent. The scale must not reach a cell carrying no value.
    CHECK(human.contains("-"));
}

TEST_CASE("an absent fleet cell is not scaled, even when the absent marker parses", "[cli][format][fleet]")
{
    // **Written after neutering proved the obvious arrangement could not fail.** With the
    // default marker the absent guard is unreachable: `PlainText` answers `-`,
    // `HumanFigureFromMachineText` cannot read that as a figure, and `value_or` hands back the
    // marker -- so deleting `kind == CellKind::Absent` from the condition changes no output and
    // every case stays green. The assertion looked like it covered the guard and covered
    // nothing.
    //
    // `--absent=0` is the arrangement that discriminates, and it is not contrived: the flag
    // exists, and a zero is exactly what somebody piping a table into a spreadsheet asks for.
    // Without the guard the marker then PARSES, and an absent `last-picked-age` renders as a
    // written zero duration -- absent flattened into the healthiest reading on the row, which
    // is the defect the column's own comment in `FleetView.cpp` exists to prevent.
    auto const table = TableValue({ "id", "last-picked-age" },
                                  {
                                      { TextCell("w1"), AbsentCell() },
                                  });

    auto const human = RenderValue(table,
                                   RenderOptions { .format = OutputFormat::Human,
                                                   .absentOverride = std::string { "0" },
                                                   .columnScales = Distributed::FleetSection::Workers });

    // The marker, exactly as asked for, and not a duration built out of it. Derived from the
    // writer rather than written as a literal, for the same reason as the case above.
    auto const scaledZero = Distributed::HumanFleetFigure(std::uint64_t { 0 }, Distributed::CellFormat::Millis);
    REQUIRE(scaledZero != "0");
    CHECK(human.contains("0"));
    CHECK_FALSE(human.contains(scaledZero));
}

TEST_CASE("a machine-readable fleet table keeps the leader's raw integer", "[cli][format][fleet]")
{
    // `CellFormat`'s header says a machine-readable surface ignores a column's scale, because a
    // consumer has the column NAME and can scale it itself. So this is a contract rather than a
    // preference, and it is the direction a fix applied to all five formats would break in
    // SILENCE: a person reading the human table would see it corrected while every script
    // parsing `--format=tsv` started receiving `1.5 min` where it had been reading `90000`.
    auto const table = TableValue({ "id", "heartbeat-age" },
                                  {
                                      { TextCell("w1"), NumberCell(std::uint64_t { 90000 }) },
                                  });

    auto const written = Distributed::HumanFleetFigure(std::uint64_t { 90000 }, Distributed::CellFormat::Millis);

    for (auto const format: { OutputFormat::Tsv, OutputFormat::Csv, OutputFormat::Kv, OutputFormat::Json })
    {
        auto const out =
            RenderValue(table, RenderOptions { .format = format, .columnScales = Distributed::FleetSection::Workers });
        CHECK(out.contains("90000"));
        CHECK_FALSE(out.contains(written));
    }
}

TEST_CASE("a column the section does not name is written as the leader sent it", "[cli][format][fleet]")
{
    // A leader may be newer than this client and send a column these tables do not know. An
    // unknown scale is not a reason to refuse a table or to guess at one: the cell is shown as
    // it arrived. This is also what every verb other than `fleet` relies on, since none of them
    // names a section at all.
    auto const table = TableValue({ "id", "not-a-fleet-column" },
                                  {
                                      { TextCell("w1"), NumberCell(std::uint64_t { 90000 }) },
                                  });

    auto const named = RenderValue(
        table, RenderOptions { .format = OutputFormat::Human, .columnScales = Distributed::FleetSection::Workers });
    auto const unnamed = RenderValue(table, RenderOptions { .format = OutputFormat::Human });

    CHECK(named.contains("90000"));
    // And naming a section changes nothing for a table whose columns none of its rows describe,
    // which is what makes the field safe to set on an answer whose shape is not a fleet table.
    CHECK(named == unnamed);
}

TEST_CASE("a kpi row is written in the unit its own row names", "[cli][format][fleet]")
{
    // `kpi` scales per ROW, not per column: its rows are one value per name, so `853` is a count
    // in one row and 853 thousandths in the next and no per-column answer exists. Which columns
    // carry the figure and the unit comes from `FleetRowUnitColumnsFor`, so this client holds no
    // list of which column is which.
    auto const table = TableValue(
        { "kpi", "value", "unit", "of" },
        {
            { TextCell("oldest-heartbeat"), NumberCell(std::uint64_t { 90000 }), TextCell("milliseconds"), AbsentCell() },
            { TextCell("dispatched"), NumberCell(std::uint64_t { 90000 }), TextCell("count"), AbsentCell() },
        });

    auto const human =
        RenderValue(table, RenderOptions { .format = OutputFormat::Human, .columnScales = Distributed::FleetSection::Kpi });

    // The SAME raw figure in both rows, deliberately: a per-column implementation would give the
    // two rows one answer, and any assertion using different numbers would pass under it.
    auto const asDuration = Distributed::HumanFleetFigure(std::uint64_t { 90000 }, Distributed::CellFormat::Millis);
    auto const asCount = Distributed::HumanFleetFigure(std::uint64_t { 90000 }, Distributed::CellFormat::Count);
    REQUIRE(asDuration != asCount);

    CHECK(human.contains(asDuration));
    CHECK(human.contains(asCount));
}

TEST_CASE("a kpi unit this build does not know is written as the leader sent it", "[cli][format][fleet]")
{
    // `permille` is not a made-up spelling: it is what a node from before #1445 writes for
    // `cache-hit-rate`, and it was observed on this machine's own service, twenty commits behind
    // the client reading it. So a client meeting a unit outside its vocabulary is the ORDINARY
    // case during a rollout rather than an edge, and the answer is to show the figure as sent --
    // not to refuse the table, and above all not to scale it by whatever the column happens to
    // suggest.
    auto const table =
        TableValue({ "kpi", "value", "unit", "of" },
                   {
                       { TextCell("cache-hit-rate"), NumberCell(std::uint64_t { 294 }), TextCell("permille"), AbsentCell() },
                   });

    auto const human =
        RenderValue(table, RenderOptions { .format = OutputFormat::Human, .columnScales = Distributed::FleetSection::Kpi });

    CHECK(human.contains("294"));
    CHECK(human.contains("permille"));
    // And it is not silently read as a share, which is the nearest spelling in the vocabulary and
    // the one a guess would land on -- 294 as a fraction is 29400%.
    auto const asShare = Distributed::HumanFleetFigure(std::uint64_t { 294 }, Distributed::CellFormat::Share);
    CHECK_FALSE(human.contains(asShare));
}

TEST_CASE("no human line ends in whitespace", "[cli][format]")
{
    // A trailing run of spaces is something a diff and a copy-paste both mangle, and
    // it is the easy mistake in a padded renderer.
    auto const table = TableValue({ "a", "bbbb" },
                                  {
                                      { TextCell("xxxx"), TextCell("y") },
                                  });
    auto const human = RenderValue(table, RenderOptions { .format = OutputFormat::Human });
    for (auto const& line: UsageLines(human))
        CHECK((line.empty() || line.back() != ' '));
}

TEST_CASE("TSV spells out the four characters it cannot carry raw", "[cli][format][tsv]")
{
    // The same four `/fleet.txt` spells out, in the same spelling: the node serves those
    // tables and this client renders `--format=tsv`, and an operator pipes both into one
    // script.
    CHECK(EscapeTsvField("plain") == "plain");
    CHECK(EscapeTsvField("has\ttab") == "has\\ttab");
    CHECK(EscapeTsvField("has\nnewline") == "has\\nnewline");
    CHECK(EscapeTsvField("has\rreturn") == "has\\rreturn");
    CHECK(EscapeTsvField("has\\backslash") == "has\\\\backslash");
    CHECK(EscapeTsvField("").empty());

    // One pass, so the escape character introduced for the tab is not escaped again by
    // a second: `\\t` here would be a two-pass implementation announcing itself.
    CHECK(EscapeTsvField("\t") == "\\t");
}

TEST_CASE("a tab inside a cell leaves the TSV row's column count unchanged", "[cli][format][tsv]")
{
    // **This is the assertion that distinguishes.** A case over ordinary cells passes
    // under the bug -- the healthy and the broken renderer emit identical bytes for a
    // field with no separator in it -- so what is asserted is the COUNT of separators
    // on the row, which is what a consumer cutting on `\t` reads.
    auto const record = RecordValue({
        Field { .name = "host", .value = TextCell("left\tright") },
    });
    auto const lines = UsageLines(Render(record, OutputFormat::Tsv));
    REQUIRE(lines.size() >= 2);

    auto const& header = lines[0];
    auto const& row = lines[1];
    CHECK(std::ranges::count(header, '\t') == 1);
    CHECK(std::ranges::count(row, '\t') == 1);
    CHECK(row == "host\tleft\\tright");

    // A newline is the other half: raw, it does not shift a column, it invents a whole
    // row -- so the row count is what says it was carried.
    auto const withNewline = RecordValue({
        Field { .name = "host", .value = TextCell("top\nbottom") },
    });
    CHECK(UsageLines(Render(withNewline, OutputFormat::Tsv)).size() == 2);
}

TEST_CASE("TSV escaping is invertible, so a literal backslash-t and a tab do not collide", "[cli][format][tsv]")
{
    // Drop the backslash from the table and both of these render `a\tb`: the output
    // stays well-formed and stops being a function of the input, which is the same
    // defect this fix exists to remove, one layer down. No reader is invented to prove
    // it -- distinctness is the whole property, and it is asserted directly.
    auto const realTab = EscapeTsvField("a\tb");
    auto const literalEscape = EscapeTsvField("a\\tb");

    CHECK(realTab == "a\\tb");
    CHECK(literalEscape == "a\\\\tb");
    CHECK(realTab != literalEscape);
}

TEST_CASE("the TSV rule agrees with the one /fleet.txt writes", "[cli][format][tsv]")
{
    // **The one-spelling property, made checkable.** The doc comment says this takes the
    // same four in the same spelling as the node's `/fleet.txt`, and a claim in prose is
    // exactly what drifts: two encoders in two binaries, agreeing today because somebody
    // read both. Asserted against the node's own escaper, so a row changed on either
    // side reddens here rather than being discovered by an operator whose script reads
    // both streams.
    for (auto const* text: { "plain", "a\tb", "a\nb", "a\rb", "a\\b", "\\\t\r\n", "" })
    {
        CAPTURE(text);
        CHECK(EscapeTsvField(text) == Distributed::EscapeDelimited(text));
    }

    // And where they legitimately DIVERGE, said out loud rather than left for somebody
    // to trip over: `/fleet.txt` is read by a terminal, so it also spells out control
    // bytes and replaces invalid UTF-8. This client's TSV carries a cached value that a
    // terminal never sees, so it touches only the four. Narrowing the assertion above to
    // inputs free of both is what keeps it exact instead of approximately true.
    auto const control = std::string { "a\x01"
                                       "b" };
    CHECK(EscapeTsvField(control) == control);
    CHECK(Distributed::EscapeDelimited(control) != control);
}

TEST_CASE("every row the node's delimited table carries is a row this client writes", "[cli][format][tsv]")
{
    // The one-spelling property as a DERIVATION rather than as an agreement.
    //
    // The case above compares the two encoders over a corpus written out here, so it
    // answers "do they agree on these seven inputs". This one walks the node's table
    // and asserts the client's output for EVERY row in it -- so a fifth row added to
    // `Distributed::DelimitedEscapes` is covered here without this file being touched,
    // and a client that had gone back to carrying its own copy fails on the row the
    // copy lacks. That is what separates *shared* from *agreeing today*, which is the
    // whole of #1334.
    //
    // Derived, never listed: a list of the four here would be the third copy of the
    // thing this change exists to stop having two of.
    std::size_t rows = 0;
    for (auto const& row: Distributed::DelimitedEscapes)
    {
        CAPTURE(row.byte);
        CHECK(EscapeTsvField(std::string { row.byte }) == row.spelling);
        ++rows;
    }

    // A walk over an empty table asserts nothing while passing, and an empty table is
    // exactly what a botched consolidation leaves behind.
    CHECK(rows == Distributed::DelimitedEscapes.size());
    CHECK(rows > 0);
}

TEST_CASE("the CSV path is untouched by the TSV rule", "[cli][format][tsv]")
{
    // A tab is not a CSV special (RFC 4180 names `,` `"` CR and LF), so CSV carries it
    // raw and unquoted -- and must go on doing so. The control is here rather than
    // implied: the cheap way to write the TSV fix is one shared quoter, and that would
    // change the format that was already correct without any case objecting.
    CHECK(QuoteCsvField("left\tright") == "left\tright");

    auto const record = RecordValue({
        Field { .name = "host", .value = TextCell("left\tright") },
    });
    CHECK(Render(record, OutputFormat::Csv).contains("host,left\tright"));

    // And the reverse direction: the TSV escaper is not a CSV quoter either.
    CHECK(EscapeTsvField("has,comma") == "has,comma");
    CHECK(EscapeTsvField("has\"quote") == "has\"quote");
}
