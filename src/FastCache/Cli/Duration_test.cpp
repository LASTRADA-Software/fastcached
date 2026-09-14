// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Cli/Options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A text that is not a duration, and the rule it breaks.
struct Refusal
{
    std::string_view text; ///< What an operator wrote.
    DurationFault fault;   ///< What `ParseDuration` must answer.
};

/// One field of each resolution a setting keeps, for the row-level cases.
struct TimedSettings
{
    std::chrono::milliseconds interval { 1s }; ///< Kept in milliseconds.
    std::chrono::seconds drain { 30s };        ///< Kept in seconds.
};

} // namespace

TEST_CASE("Every unit in the table reads as its length, and each length is the one its name promises", "[cli][duration]")
{
    // Walked over the TABLE, so a unit added is a unit parsed; and pinned by NAME below, so a row whose length drifted
    // is not agreed with by a walk that reads the same wrong number twice.
    for (auto const& unit: DurationUnitTable)
    {
        INFO(unit.suffix);
        CHECK(ParseDuration(std::format("7{}", unit.suffix))
              == std::expected<std::chrono::milliseconds, DurationFault> { 7 * unit.length });
    }
    CHECK(ParseDuration("1ms") == std::expected<std::chrono::milliseconds, DurationFault> { 1ms });
    CHECK(ParseDuration("1s") == std::expected<std::chrono::milliseconds, DurationFault> { 1000ms });
    CHECK(ParseDuration("1min") == std::expected<std::chrono::milliseconds, DurationFault> { 60'000ms });
    CHECK(ParseDuration("1h") == std::expected<std::chrono::milliseconds, DurationFault> { 3'600'000ms });
    CHECK(ParseDuration("1d") == std::expected<std::chrono::milliseconds, DurationFault> { 86'400'000ms });
    CHECK(ParseDuration("0ms") == std::expected<std::chrono::milliseconds, DurationFault> { 0ms });
    CHECK(DurationUnitList() == "ms, s, min, h, d");

    // #1402's acceptance, at the grammar: a live-stats interval written either way reads, and a bare one does not.
    CHECK(ParseDuration("2s") == std::expected<std::chrono::milliseconds, DurationFault> { 2000ms });
    CHECK(ParseDuration("500ms") == std::expected<std::chrono::milliseconds, DurationFault> { 500ms });
    CHECK(ParseDuration("2000")
          == std::expected<std::chrono::milliseconds, DurationFault> { std::unexpect, DurationFault::MissingUnit });
}

TEST_CASE("A text that is not a duration is refused by the rule it breaks", "[cli][duration]")
{
    // The longest whole number of days a millisecond count holds, and one more: the multiplication is checked BEFORE it
    // is done, so the second is refused rather than wrapped into a plausible length.
    auto const maxDays = std::chrono::milliseconds::max() / std::chrono::milliseconds { std::chrono::days { 1 } };
    CHECK(ParseDuration(std::format("{}d", maxDays)).has_value());
    auto const pastDays = std::format("{}d", maxDays + 1);

    auto const refusals = std::array {
        Refusal { .text = "", .fault = DurationFault::Empty },
        Refusal { .text = "2000", .fault = DurationFault::MissingUnit },
        Refusal { .text = "0", .fault = DurationFault::MissingUnit },
        Refusal { .text = "5m", .fault = DurationFault::UnknownUnit },
        Refusal { .text = "5sec", .fault = DurationFault::UnknownUnit },
        Refusal { .text = "2S", .fault = DurationFault::UnknownUnit },
        Refusal { .text = "2 s", .fault = DurationFault::UnknownUnit },
        Refusal { .text = "1h30min", .fault = DurationFault::UnknownUnit },
        Refusal { .text = "1.5s", .fault = DurationFault::NotANumber },
        Refusal { .text = "s", .fault = DurationFault::NotANumber },
        Refusal { .text = "+2s", .fault = DurationFault::NotANumber },
        Refusal { .text = " 2s", .fault = DurationFault::NotANumber },
        Refusal { .text = "-5s", .fault = DurationFault::Negative },
        Refusal { .text = "-0ms", .fault = DurationFault::Negative },
        Refusal { .text = "9223372036854775808ms", .fault = DurationFault::Overflow },
        Refusal { .text = "99999999999999999999999s", .fault = DurationFault::Overflow },
        Refusal { .text = pastDays, .fault = DurationFault::Overflow },
    };
    for (auto const& refusal: refusals)
    {
        INFO("`" << refusal.text << "`");
        CHECK(ParseDuration(refusal.text)
              == std::expected<std::chrono::milliseconds, DurationFault> { std::unexpect, refusal.fault });
    }
}

TEST_CASE("Each refusal says what was written and, where the grammar is the fix, the grammar", "[cli][duration]")
{
    CHECK(DescribeDurationFault(DurationFault::MissingUnit, "2000").contains("`2000` names no unit"));
    CHECK(DescribeDurationFault(DurationFault::MissingUnit, "2000").contains("ms, s, min, h, d"));
    CHECK(DescribeDurationFault(DurationFault::UnknownUnit, "5m").contains("ms, s, min, h, d"));
    CHECK(DescribeDurationFault(DurationFault::NotANumber, "1.5s").contains("1500ms rather than 1.5s"));
    CHECK(DescribeDurationFault(DurationFault::Empty, "").contains("got nothing"));
    CHECK(DescribeDurationFault(DurationFault::Negative, "-5s").contains("`-5s` is negative"));
    CHECK(DescribeDurationFault(DurationFault::Overflow, "9d").contains("longer than this setting can hold"));
    CHECK(DescribeDurationFault(DurationFault::FinerThanField, "500ms", 1s).contains("whole number of 1s"));

    // Wrong in kind against right in kind and out of range: the two codes a configuration error already distinguishes.
    CHECK(DurationError(DurationFault::MissingUnit, "2000").code == ConfigErrorCode::TypeMismatch);
    CHECK(DurationError(DurationFault::Negative, "-5s").code == ConfigErrorCode::OutOfRange);
    CHECK(DurationError(DurationFault::FinerThanField, "500ms", 1s).code == ConfigErrorCode::OutOfRange);
    // No field: the row that reached the parser stamps its own spelling.
    CHECK(DurationError(DurationFault::MissingUnit, "2000").field.empty());
}

TEST_CASE("A length fits a field only as a whole number of its tick, and is never rounded into one", "[cli][duration]")
{
    CHECK(DurationIn<std::chrono::seconds>(2000ms) == std::expected<std::chrono::seconds, DurationFault> { 2s });
    CHECK(DurationIn<std::chrono::seconds>(1500ms)
          == std::expected<std::chrono::seconds, DurationFault> { std::unexpect, DurationFault::FinerThanField });
    CHECK(DurationIn<std::chrono::milliseconds>(1500ms)
          == std::expected<std::chrono::milliseconds, DurationFault> { 1500ms });

    // A narrow representation: the tick count must FIT, and the last one that does is accepted.
    using NarrowSeconds = std::chrono::duration<std::int32_t>;
    auto const widest = std::chrono::milliseconds { std::chrono::seconds { std::numeric_limits<std::int32_t>::max() } };
    CHECK(DurationIn<NarrowSeconds>(widest).has_value());
    CHECK(DurationIn<NarrowSeconds>(widest + 1s)
          == std::expected<NarrowSeconds, DurationFault> { std::unexpect, DurationFault::Overflow });
}

TEST_CASE("A duration is written in the longest unit that divides it, and what is written reads back", "[cli][duration]")
{
    // The canonical spellings: each is what FormatDuration writes for the length it names.
    constexpr auto Canonical = std::array<std::string_view, 12> { "0s",   "1ms",   "999ms", "1500ms", "1s", "90s",
                                                                  "1min", "61min", "1h",    "25h",    "1d", "30d" };
    for (auto const canonical: Canonical)
    {
        INFO(canonical);
        auto const read = ParseDuration(canonical);
        REQUIRE(read.has_value());
        CHECK(FormatDuration(read.value()) == canonical);
    }
    CHECK(FormatDuration(90'000ms) == "90s");
    CHECK(FormatDuration(3'600'000ms) == "1h");
    CHECK(FormatDuration(std::chrono::seconds { 86'400 }) == "1d");

    // And the other direction over a spread no hand-written list reaches: every length reads back to itself, the
    // longest one a millisecond count holds included.
    auto const spread =
        std::views::iota(0, 64) | std::views::transform([](int shift) {
            // Unsigned for the shift, so the widest one is `2^63 - 1` rather than signed overflow.
            return std::chrono::milliseconds { static_cast<std::int64_t>((std::uint64_t { 1 } << shift) - 1) };
        });
    for (auto const length: spread)
    {
        INFO(length.count());
        CHECK(ParseDuration(FormatDuration(length)) == std::expected<std::chrono::milliseconds, DurationFault> { length });
    }
    CHECK(ParseDuration(FormatDuration(std::chrono::milliseconds::max()))
          == std::expected<std::chrono::milliseconds, DurationFault> { std::chrono::milliseconds::max() });
}

TEST_CASE("An option row takes a duration through the one value parser, at the field's own resolution", "[cli][duration]")
{
    auto const interval = AssignFrom<&TimedSettings::interval, ParseDurationValue<std::chrono::milliseconds>>();
    auto const drain = AssignFrom<&TimedSettings::drain, ParseDurationValue<std::chrono::seconds>>();

    TimedSettings settings;
    CHECK(interval(settings, "2s").has_value());
    CHECK(settings.interval == 2000ms);
    CHECK(drain(settings, "2min").has_value());
    CHECK(settings.drain == 120s);

    // Refused, and the field left as it was: a half-applied value is the silent substitution the grammar exists to end.
    auto const finer = drain(settings, "500ms");
    REQUIRE_FALSE(finer.has_value());
    CHECK(finer.error().code == ConfigErrorCode::OutOfRange);
    CHECK(finer.error().context.contains("whole number of 1s"));
    CHECK(settings.drain == 120s);

    auto const bare = interval(settings, "2000");
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error().context.contains("names no unit"));
    CHECK(settings.interval == 2000ms);
}
