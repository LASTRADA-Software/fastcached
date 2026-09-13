// SPDX-License-Identifier: Apache-2.0
#include "LivePipedView.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using FastCache::Testing::Unwrap;

namespace
{

/// Everything the loop presented, in order, as one stream: what a pipe would have received.
class StreamSink final: public IFrameSink
{
  public:
    void Present(std::string_view frame) override
    {
        stream += frame;
    }

    std::string stream {};
};

/// A stats round `ChooseStats` accepts, with the fields named.
/// @param fields The record's fields, in order.
/// @return One attempt carrying them.
[[nodiscard]] std::vector<StatsAttempt> ReadingOf(std::vector<Field> fields)
{
    return { StatsAttempt {
        .origin = StatsOrigin::Info, .asked = true, .record = RecordValue(std::move(fields)), .note = {} } };
}

/// The ordinary reading these cases stream: two counters.
/// @param connections The first counter.
/// @return The attempts.
[[nodiscard]] std::vector<StatsAttempt> Reading(std::uint64_t connections)
{
    return ReadingOf({
        Field { .name = "curr_connections", .value = NumberCell(connections) },
        Field { .name = "used_memory", .value = NumberCell(std::uint64_t { 4096 }) },
    });
}

/// A sample that read, followed by the tick the cadence owes it.
/// @param script Where to append.
/// @param seconds When it was taken.
/// @param attempts What the sources said.
void AddSample(std::vector<DashboardEvent>& script, int seconds, std::vector<StatsAttempt> attempts)
{
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Sample,
                                      .at = TimePoint { std::chrono::seconds { seconds } },
                                      .attempts = std::move(attempts) });
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Tick });
}

/// A sample nothing answered, followed by its tick.
/// @param script Where to append.
void AddFailure(std::vector<DashboardEvent>& script)
{
    script.push_back(DashboardEvent {
        .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable, .note = "nothing answered" });
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Tick });
}

/// Run the loop once; a named coroutine over pointers, for the reason `DashboardLoop_test` gives.
/// @return The task.
[[nodiscard]] Task<void> DriveOnce(IDashboardEventSource* events,
                                   IDashboardView* view,
                                   IFrameSink* sink,
                                   DashboardLimits limits,
                                   std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(events, &ReadStatsSample, view, sink, limits);
}

/// Stream @p script through a piped view and return what the pipe received.
/// @param script The events.
/// @param format The `--format`.
/// @param limits The bound.
/// @param absent The `--absent` override.
/// @return The stream.
[[nodiscard]] std::string Stream(std::vector<DashboardEvent> script,
                                 OutputFormat format,
                                 DashboardLimits limits = {},
                                 std::optional<std::string> absent = std::nullopt)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };
    auto view = PipedRecordView { format, std::move(absent), &LatestReading };
    auto sink = StreamSink {};

    auto result = std::optional<DashboardExit> {};
    auto task = DriveOnce(&events, &view, &sink, limits, &result);
    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(result.has_value());
    return sink.stream;
}

/// The stream's lines, without their newlines.
/// @param stream What the pipe received; every line ends in a newline.
/// @return The lines.
[[nodiscard]] std::vector<std::string> Lines(std::string_view stream)
{
    std::vector<std::string> lines;
    while (!stream.empty())
    {
        auto const end = stream.find('\n');
        REQUIRE(end != std::string_view::npos);
        lines.emplace_back(stream.substr(0, end));
        stream.remove_prefix(end + 1);
    }
    return lines;
}

/// Two readings, a failure, and a third reading.
/// @return The script.
[[nodiscard]] std::vector<DashboardEvent> ReadReadFailRead()
{
    std::vector<DashboardEvent> script;
    AddSample(script, 1, Reading(10));
    AddSample(script, 2, Reading(11));
    AddFailure(script);
    AddSample(script, 4, Reading(12));
    return script;
}

} // namespace

TEST_CASE("a piped run writes its header once and one line per sample with no escape byte", "[cli][live][piped]")
{
    // §9.13, over each line-shaped format. Asserting the output PARSES passes with escapes in it,
    // so the escape byte is counted; asserting a header is present passes when it is repeated, so
    // the header's line is counted too.
    for (auto const format: { OutputFormat::Human, OutputFormat::Tsv, OutputFormat::Csv })
    {
        INFO("format: " << DescriptorOf(format)->name);
        auto const stream = Stream(ReadReadFailRead(), format);
        CHECK(std::ranges::count(stream, '\x1b') == 0);

        auto const lines = Lines(stream);
        REQUIRE(lines.size() == 5);
        CHECK(lines.front().contains("source"));
        CHECK(lines.front().contains("curr_connections"));
        CHECK(std::ranges::count(lines, lines.front()) == 1);
        CHECK(lines[1].contains("10"));
        CHECK(lines[2].contains("11"));
        CHECK(lines[4].contains("12"));
        // The failed sample is a row, not a missing one: nothing it could have read is on it.
        CHECK_FALSE(lines[3].contains("4096"));
    }
}

TEST_CASE("a piped failure is a row of absent cells in the format's own spelling", "[cli][live][piped]")
{
    auto const tsv = Lines(Stream(ReadReadFailRead(), OutputFormat::Tsv));
    REQUIRE(tsv.size() == 5);
    CHECK(tsv[3] == "\t\t");

    auto const human = Lines(Stream(ReadReadFailRead(), OutputFormat::Human));
    REQUIRE(human.size() == 5);
    CHECK(std::ranges::count(human[3], '-') == 3);

    // `--absent` names the placeholder where the format has none of its own.
    auto const named = Lines(Stream(ReadReadFailRead(), OutputFormat::Csv, {}, "NA"));
    REQUIRE(named.size() == 5);
    CHECK(named[3] == "NA,NA,NA");
}

TEST_CASE("piped json is one document per line, and a failure is a document of nulls", "[cli][live][piped]")
{
    // §9.14. No JSON parser is in this tree, so "independently parseable" is asserted as what makes
    // it so: every line is exactly one object the record renderer wrote, and no document spans a
    // newline. `--absent` does not reach it -- a null is JSON's own absent.
    auto const lines = Lines(Stream(ReadReadFailRead(), OutputFormat::Json, {}, "NA"));
    REQUIRE(lines.size() == 4);
    for (auto const& line: lines)
    {
        INFO("line: " << line);
        CHECK(line.starts_with('{'));
        CHECK(line.ends_with('}'));
        CHECK(std::ranges::count(line, '{') == 1);
    }
    CHECK(lines[0].contains(R"("curr_connections":10)"));
    CHECK(lines[2] == R"({"source":null,"curr_connections":null,"used_memory":null})");
}

TEST_CASE("--samples=N piped writes exactly N rows under its header", "[cli][live][piped]")
{
    // §9.16 through the view: the budget's own frame is the Nth row, and the sample after the
    // budget is never read.
    std::vector<DashboardEvent> script;
    AddSample(script, 1, Reading(10));
    AddSample(script, 2, Reading(11));
    AddSample(script, 3, Reading(12));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv, DashboardLimits { .samples = 2 }));
    REQUIRE(lines.size() == 3);
    CHECK(lines[2].contains("11"));
}

TEST_CASE("failures before the first reading write nothing, and the header waits for a reading", "[cli][live][piped]")
{
    // They have no columns to be absent in. An empty or column-less first line would break the
    // header-once promise for everything below it.
    std::vector<DashboardEvent> script;
    AddFailure(script);
    AddFailure(script);
    AddSample(script, 3, Reading(10));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Tsv));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].contains("curr_connections"));
    CHECK(lines[1].contains("10"));
}

TEST_CASE("a later piped reading is read against the header by name", "[cli][live][piped]")
{
    // A header printed once is a promise about every row under it: a field it does not name is
    // dropped, and a column the reading lacks is absent rather than shifted into its neighbour.
    std::vector<DashboardEvent> script;
    AddSample(script,
              1,
              ReadingOf({ Field { .name = "a", .value = NumberCell(std::uint64_t { 1 }) },
                          Field { .name = "b", .value = NumberCell(std::uint64_t { 2 }) } }));
    AddSample(script,
              2,
              ReadingOf({ Field { .name = "b", .value = NumberCell(std::uint64_t { 20 }) },
                          Field { .name = "c", .value = NumberCell(std::uint64_t { 30 }) } }));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Csv));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "source,a,b");
    CHECK(lines[2].ends_with(",,20"));
    CHECK_FALSE(lines[2].contains("30"));
}

TEST_CASE("piped kv writes one block of name=value lines per sample", "[cli][live][piped]")
{
    std::vector<DashboardEvent> script;
    AddSample(script, 1, Reading(10));
    AddSample(script, 2, Reading(11));

    auto const lines = Lines(Stream(std::move(script), OutputFormat::Kv));
    REQUIRE(lines.size() == 7);
    CHECK(lines[1] == "curr_connections=10");
    CHECK(lines[3].empty());
    CHECK(lines[5] == "curr_connections=11");
    CHECK(std::ranges::count(lines, std::string { "curr_connections=10" }) == 1);
}
