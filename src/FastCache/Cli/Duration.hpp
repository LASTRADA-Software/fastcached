// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache
{

/// A unit a duration may be written in.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted. What an operator types and a configuration file
/// keeps is the row's SUFFIX, never the ordinal.
enum class DurationUnit : std::uint8_t
{
    Millisecond,
    Second,
    Minute,
    Hour,
    Day,
    Last,
};

/// One unit: how it is written, and how long it is.
struct DurationUnitSpec
{
    DurationUnit unit;                ///< The enumerator this row describes.
    std::string_view suffix;          ///< What follows the digits: `ms`, `s`, `min`, `h`, `d`.
    std::chrono::milliseconds length; ///< One of it.
};

/// Every unit a duration may be written in, shortest first (#1402).
///
/// **The one grammar for every time-valued setting in every binary**: a flag, a configuration key, a replicated
/// setting and the launcher's environment. Parsed by `ParseDuration` and written back by `FormatDuration` from these
/// rows alone, so what any tool prints can be pasted back as a value.
///
/// `min`, not `m`: `m` reads as metres or months as readily as minutes, and a unit an operator has to look up is a
/// unit they will guess. `d` is here for a cache TTL, which runs to days. Nothing finer than `ms`: no setting this
/// tree has means less than a millisecond, and a unit nothing takes is a spelling nothing tests.
inline constexpr auto DurationUnitTable = EnumTable<DurationUnit, DurationUnitSpec> { {
    { .unit = DurationUnit::Millisecond, .suffix = "ms", .length = std::chrono::milliseconds { 1 } },
    { .unit = DurationUnit::Second, .suffix = "s", .length = std::chrono::seconds { 1 } },
    { .unit = DurationUnit::Minute, .suffix = "min", .length = std::chrono::minutes { 1 } },
    { .unit = DurationUnit::Hour, .suffix = "h", .length = std::chrono::hours { 1 } },
    { .unit = DurationUnit::Day, .suffix = "d", .length = std::chrono::days { 1 } },
} };

static_assert(RowsInEnumeratorOrder(DurationUnitTable, &DurationUnitSpec::unit),
              "DurationUnitTable must hold one row per DurationUnit, in enumerator order");

/// Whether the table is shortest first and every unit is a whole number of the one before it.
///
/// `FormatDuration` walks the table from the END and takes the first unit that divides a value, which is the longest
/// such unit only when the table is in length order. A unit that is not a whole number of the one before (a month is
/// not a whole number of weeks) would break the nesting that makes "the first that divides" well defined.
/// @return True when each row is longer than, and a multiple of, the row before it.
[[nodiscard]] constexpr bool UnitsNestInOrder() noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t { 1 }, DurationUnitTable.size()), [](std::size_t index) {
        auto const shorter = DurationUnitTable.at(index - 1).length;
        auto const longer = DurationUnitTable.at(index).length;
        return longer > shorter && longer % shorter == std::chrono::milliseconds::zero();
    });
}

static_assert(UnitsNestInOrder(), "DurationUnitTable must be shortest first, each unit a whole number of the last");

/// Why a text is not a duration.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class DurationFault : std::uint8_t
{
    Empty,          ///< Nothing was given.
    NotANumber,     ///< The digits are missing, or are not a whole number (`1.5s`, `+2s`, ` 2s`).
    Negative,       ///< A leading `-`: a length of time is zero or more.
    MissingUnit,    ///< A bare number, which means milliseconds in one setting and seconds in another.
    UnknownUnit,    ///< A unit `DurationUnitTable` does not have.
    Overflow,       ///< Longer than the setting's field can hold.
    FinerThanField, ///< Not a whole number of the field's resolution (`500ms` into seconds). Never truncated.
    Last,
};

/// A field type a duration can be assigned to: a `std::chrono::duration` whose tick is a whole number of milliseconds.
template <typename T>
concept WholeMillisecondDuration = std::same_as<T, std::chrono::duration<typename T::rep, typename T::period>>
                                   && std::ratio_divide<typename T::period, std::milli>::den == 1;

/// Read a duration: one whole number, then one unit from `DurationUnitTable`, nothing before and nothing after.
///
/// **A bare number is refused.** That is the defect the grammar exists to close: `--interval=5` meaning milliseconds
/// in one flag and seconds in the next. No fraction, no composite (`1h30min`), no whitespace: a value with one
/// spelling is a value two tools can compare as text.
/// @param text What the operator wrote.
/// @return The length, or which rule it broke.
[[nodiscard]] std::expected<std::chrono::milliseconds, DurationFault> ParseDuration(std::string_view text) noexcept;

/// Fit a length into a field's type exactly.
///
/// `FinerThanField` when the length is not a whole number of the field's tick -- `500ms` into seconds -- because a
/// value silently rounded is a setting the operator did not choose. `Overflow` when the tick count does not fit the
/// field's representation.
/// @param value The length.
/// @return The field's value, or why it will not fit.
template <WholeMillisecondDuration Target>
[[nodiscard]] constexpr std::expected<Target, DurationFault> DurationIn(std::chrono::milliseconds value) noexcept
{
    auto const tick = std::chrono::duration_cast<std::chrono::milliseconds>(Target { 1 }).count();
    if (value.count() % tick != 0)
        return std::unexpected(DurationFault::FinerThanField);
    auto const ticks = value.count() / tick;
    if (!std::in_range<typename Target::rep>(ticks))
        return std::unexpected(DurationFault::Overflow);
    return Target { static_cast<Target::rep>(ticks) };
}

/// Write a length in the longest unit that divides it exactly: `90000ms` is `90s`, `1500ms` stays `1500ms`.
///
/// **Everything this writes, `ParseDuration` reads back to the same length** -- the property every place that prints
/// a setting depends on, since what it prints is meant to be pasted back. Zero is `0s`: every unit divides it, and
/// seconds is the unit a disabled interval is most often read in.
/// @param value The length; zero or more.
/// @return Its text.
[[nodiscard]] std::string FormatDuration(std::chrono::milliseconds value);

/// The units, as an operator is told them: `ms, s, min, h, d`.
/// @return The suffixes in table order.
[[nodiscard]] std::string DurationUnitList();

/// Say why @p text is not a duration, naming the grammar where the grammar is the fix.
/// @param fault What `ParseDuration` or `DurationIn` answered.
/// @param text What the operator wrote.
/// @param resolution The field's tick, named by `FinerThanField`.
/// @return The sentence.
[[nodiscard]] std::string DescribeDurationFault(DurationFault fault,
                                                std::string_view text,
                                                std::chrono::milliseconds resolution = std::chrono::milliseconds { 1 });

/// The same refusal as a configuration error.
///
/// It names no field: a value parser cannot know which flag or key it was reached through, and `ApplyOneOption`
/// stamps the row's own spelling.
/// @param fault What was wrong.
/// @param text What the operator wrote.
/// @param resolution The field's tick.
/// @return The error.
[[nodiscard]] ConfigError DurationError(DurationFault fault,
                                        std::string_view text,
                                        std::chrono::milliseconds resolution = std::chrono::milliseconds { 1 });

/// A value parser for an option row: `AssignFrom<&Config::drainTimeout, ParseDurationValue<std::chrono::seconds>>()`.
/// @param text The flag's or key's value.
/// @return The field's value, or the refusal.
template <WholeMillisecondDuration Target>
[[nodiscard]] std::expected<Target, ConfigError> ParseDurationValue(std::string_view text)
{
    auto const resolution = std::chrono::duration_cast<std::chrono::milliseconds>(Target { 1 });
    return ParseDuration(text)
        .and_then([](std::chrono::milliseconds value) { return DurationIn<Target>(value); })
        .transform_error([text, resolution](DurationFault fault) { return DurationError(fault, text, resolution); });
}

} // namespace FastCache
