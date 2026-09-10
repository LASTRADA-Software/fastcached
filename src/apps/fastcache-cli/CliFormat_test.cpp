// SPDX-License-Identifier: Apache-2.0
#include "CliFormat.hpp"

#include <FastCache/Cli/UsageTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

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
