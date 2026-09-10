// SPDX-License-Identifier: Apache-2.0
#include "StatsSource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;

namespace
{
/// An attempt that produced a record.
/// @param origin Which source.
/// @param field One field to put in the record, so the winner is identifiable.
/// @return The attempt.
[[nodiscard]] StatsAttempt Answered(StatsOrigin origin, std::string field)
{
    return StatsAttempt { .origin = origin,
                          .asked = true,
                          .record = RecordValue(
                              { Field { .name = std::move(field), .value = NumberCell(std::uint64_t { 1 }) } }) };
}

/// An attempt that was made and failed.
/// @param origin Which source.
/// @param note Why.
/// @return The attempt.
[[nodiscard]] StatsAttempt Failed(StatsOrigin origin, std::string note)
{
    return StatsAttempt { .origin = origin, .asked = true, .note = std::move(note) };
}

/// An attempt that was never made.
/// @param origin Which source.
/// @param note Why not.
/// @return The attempt.
[[nodiscard]] StatsAttempt NotAsked(StatsOrigin origin, std::string note)
{
    return StatsAttempt { .origin = origin, .asked = false, .note = std::move(note) };
}

/// Whether any advisory contains @p needle.
/// @param answer The answer.
/// @param needle What to look for.
/// @return True when some advisory contains it.
[[nodiscard]] bool Mentions(Answer const& answer, std::string_view needle)
{
    return std::ranges::any_of(answer.advisories,
                               [needle](std::string const& advisory) { return advisory.contains(needle); });
}

/// The `source` field's text.
/// @param answer The answer.
/// @return The field's lexical value, or empty when there is none.
[[nodiscard]] std::string SourceOf(Answer const& answer)
{
    auto const* const field = FindField(answer.value, "source");
    return field == nullptr ? std::string {} : field->value.lexical;
}
} // namespace

TEST_CASE("every origin has a row and a name", "[cli][stats]")
{
    for (auto const& row: StatsOriginTable)
    {
        REQUIRE(DescriptorOf(row.origin) != nullptr);
        CHECK(DescriptorOf(row.origin)->name == row.name);
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.what.empty());
    }
}

TEST_CASE("the richest source wins when several answer", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> {
        Answered(StatsOrigin::Info, "from_info"),
        Answered(StatsOrigin::Metrics, "from_metrics"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Affirmative);
    // Ladder order is the table's order, not the order the attempts arrive in -- so a
    // list given thin-first must still choose the rich one.
    CHECK(SourceOf(answer) == "metrics");
    CHECK(FindField(answer.value, "from_metrics") != nullptr);
    CHECK(FindField(answer.value, "from_info") == nullptr);
}

TEST_CASE("the chosen source is reported on stdout, not only as a remark", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> { Answered(StatsOrigin::Metrics, "x") };
    auto const answer = ChooseStats(attempts);

    // Which numbers am I looking at is a question a script asks too. A dashboard that
    // cannot tell a 102-counter reading from a 7-field one draws the missing 95 as
    // zeroes.
    REQUIRE(answer.value.shape == Shape::Record);
    REQUIRE_FALSE(answer.value.fields.empty());
    CHECK(answer.value.fields.front().name == "source");
}

TEST_CASE("falling back to the thin source carries its caveat", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> {
        Failed(StatsOrigin::Metrics, "connection refused"),
        Answered(StatsOrigin::Info, "used_memory"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(SourceOf(answer) == "info");
    // The caveat says what choosing this rung costs, which is the whole reason for
    // naming the source at all.
    CHECK(Mentions(answer, "seven fields"));
    // And the richer source's failure is reported, because an operator who wanted the
    // full set needs to know why they did not get it.
    CHECK(Mentions(answer, "connection refused"));
    CHECK(Mentions(answer, "did not answer"));
}

TEST_CASE("a source that was never asked is not reported as having failed", "[cli][stats]")
{
    // The three-state rule, and the state that gets collapsed. Reporting "/metrics did
    // not answer" for an endpoint nothing dialled sends an operator to check a
    // listener that was never contacted.
    auto const attempts = std::vector<StatsAttempt> {
        NotAsked(StatsOrigin::Metrics, "no admin address is known"),
        Answered(StatsOrigin::Info, "used_memory"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(SourceOf(answer) == "info");
    CHECK(Mentions(answer, "was not asked"));
    // The distinguishing assertion: a collapsed implementation would say this.
    CHECK_FALSE(Mentions(answer, "did not answer"));
}

TEST_CASE("nothing answering is unreachable and names every source's state", "[cli][stats]")
{
    auto const attempts = std::vector<StatsAttempt> {
        NotAsked(StatsOrigin::Metrics, "no admin address is known"),
        Failed(StatsOrigin::Info, "connection refused"),
    };

    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Unreachable);
    // "stats unavailable" alone does not say whether to start a listener, fix a port,
    // or look at the daemon. Both states are named, and differently.
    CHECK(Mentions(answer, "was not asked"));
    CHECK(Mentions(answer, "did not answer"));
    CHECK(Mentions(answer, "no admin address is known"));
    CHECK(Mentions(answer, "connection refused"));
}

TEST_CASE("a source that was not reported on at all is said to be missing", "[cli][stats]")
{
    // A gatherer that simply omits a source must not read as that source being fine.
    auto const attempts = std::vector<StatsAttempt> { Failed(StatsOrigin::Info, "refused") };
    auto const answer = ChooseStats(attempts);
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(Mentions(answer, "was not reported on"));
}

TEST_CASE("no attempts at all is unreachable rather than an empty success", "[cli][stats]")
{
    auto const answer = ChooseStats({});
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK_FALSE(answer.advisories.empty());
}

TEST_CASE("a Prometheus body parses into numbered fields, keeping labels", "[cli][stats]")
{
    auto const body = std::string { "# HELP fastcached_items Items.\n"
                                    "# TYPE fastcached_items gauge\n"
                                    "fastcached_items 42\n"
                                    "fastcached_tier_items{tier=\"l1\"} 7\n"
                                    "fastcached_tier_items{tier=\"l2\"} 9\n" };

    auto const record = ParsePrometheus(body);
    REQUIRE(record.shape == Shape::Record);
    REQUIRE(FindField(record, "fastcached_items") != nullptr);
    CHECK(FindField(record, "fastcached_items")->value.kind == CellKind::Number);
    CHECK(FindField(record, "fastcached_items")->value.lexical == "42");

    // The labels stay in the field name. Dropping them would merge two tiers into one
    // field and silently report one tier's number for the other.
    REQUIRE(FindField(record, "fastcached_tier_items{tier=\"l1\"}") != nullptr);
    CHECK(FindField(record, "fastcached_tier_items{tier=\"l1\"}")->value.lexical == "7");
    REQUIRE(FindField(record, "fastcached_tier_items{tier=\"l2\"}") != nullptr);
    CHECK(FindField(record, "fastcached_tier_items{tier=\"l2\"}")->value.lexical == "9");

    // The comments contributed nothing.
    CHECK(record.fields.size() == 3);
}

TEST_CASE("an INFO body parses, classifying numbers apart from text", "[cli][stats]")
{
    auto const body = std::string { "# Server\r\n"
                                    "fastcached_version:0.2.0\r\n"
                                    "redis_version:6.0.0-fastcached\r\n"
                                    "# Memory\r\n"
                                    "used_memory:1234\r\n" };

    auto const record = ParseInfo(body);
    REQUIRE(FindField(record, "used_memory") != nullptr);
    // A number, so JSON emits it bare and `jq` can compare it.
    CHECK(FindField(record, "used_memory")->value.kind == CellKind::Number);

    REQUIRE(FindField(record, "redis_version") != nullptr);
    // Text, and NOT a number truncated at the first dot -- which is what a lax
    // classifier would produce, turning 6.0.0-fastcached into 6.
    CHECK(FindField(record, "redis_version")->value.kind == CellKind::Text);
    CHECK(FindField(record, "redis_version")->value.lexical == "6.0.0-fastcached");

    CHECK(FindField(record, "fastcached_version")->value.lexical == "0.2.0");
    // The section headers contributed nothing.
    CHECK(record.fields.size() == 3);
}

TEST_CASE("an empty or comment-only body parses to no fields", "[cli][stats]")
{
    // The signal the gatherer uses to report "answered with no series" rather than
    // reporting an empty but successful scrape, which a dashboard draws as zeroes.
    CHECK(ParsePrometheus("").fields.empty());
    CHECK(ParsePrometheus("# only a comment\n").fields.empty());
    CHECK(ParseInfo("").fields.empty());
    CHECK(ParseInfo("# Server\r\n").fields.empty());
}

TEST_CASE("an INFO field with an empty value is kept, not dropped", "[cli][stats]")
{
    // A key the server emitted with nothing after the colon is a reported field whose
    // value happens to be empty. Dropping it would make the field look absent.
    auto const record = ParseInfo("some_key:\r\n");
    REQUIRE(FindField(record, "some_key") != nullptr);
    CHECK(FindField(record, "some_key")->value.kind == CellKind::Text);
    CHECK(FindField(record, "some_key")->value.lexical.empty());
}
