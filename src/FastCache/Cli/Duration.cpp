// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Duration.hpp>

#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include <core/Ranges.hpp>

namespace FastCache
{

namespace
{

    /// One refusal: the configuration error class it is, and its sentence.
    struct DurationFaultSpec
    {
        DurationFault fault;  ///< The enumerator this row describes.
        ConfigErrorCode code; ///< Wrong in kind, or right in kind and out of range.
        /// The sentence, from what was written and the field's tick.
        std::string (*describe)(std::string_view text, std::chrono::milliseconds resolution);
    };

    /// Every refusal's words, one row each. The grammar faults name the grammar, because the grammar is the fix.
    constexpr auto DurationFaultTable = EnumTable<DurationFault, DurationFaultSpec> { {
        { .fault = DurationFault::Empty,
          .code = ConfigErrorCode::TypeMismatch,
          .describe =
              [](std::string_view /*text*/, std::chrono::milliseconds /*resolution*/) {
                  return std::format("expected a duration -- a whole number and a unit ({}), e.g. 500ms or 2s -- and got "
                                     "nothing",
                                     DurationUnitList());
              } },
        { .fault = DurationFault::NotANumber,
          .code = ConfigErrorCode::TypeMismatch,
          .describe =
              [](std::string_view text, std::chrono::milliseconds /*resolution*/) {
                  return std::format("`{}` is not a duration: write a whole number and a unit ({}), e.g. 1500ms rather "
                                     "than 1.5s",
                                     text,
                                     DurationUnitList());
              } },
        { .fault = DurationFault::Negative,
          .code = ConfigErrorCode::OutOfRange,
          .describe =
              [](std::string_view text, std::chrono::milliseconds /*resolution*/) {
                  return std::format("`{}` is negative; a duration is a length of time, zero or more", text);
              } },
        { .fault = DurationFault::MissingUnit,
          .code = ConfigErrorCode::TypeMismatch,
          .describe =
              [](std::string_view text, std::chrono::milliseconds /*resolution*/) {
                  return std::format("`{}` names no unit, and a bare number means nothing here; write it with one of {}, "
                                     "e.g. {}ms or {}s",
                                     text,
                                     DurationUnitList(),
                                     text,
                                     text);
              } },
        { .fault = DurationFault::UnknownUnit,
          .code = ConfigErrorCode::TypeMismatch,
          .describe =
              [](std::string_view text, std::chrono::milliseconds /*resolution*/) {
                  return std::format(
                      "`{}` is not written in a unit this build knows; the units are {}", text, DurationUnitList());
              } },
        { .fault = DurationFault::Overflow,
          .code = ConfigErrorCode::OutOfRange,
          .describe =
              [](std::string_view text, std::chrono::milliseconds /*resolution*/) {
                  return std::format("`{}` is longer than this setting can hold", text);
              } },
        { .fault = DurationFault::FinerThanField,
          .code = ConfigErrorCode::OutOfRange,
          .describe =
              [](std::string_view text, std::chrono::milliseconds resolution) {
                  return std::format("`{}` is not a whole number of {}, the finest this setting keeps; it is refused "
                                     "rather than rounded",
                                     text,
                                     FormatDuration(resolution));
              } },
    } };

    static_assert(RowsInEnumeratorOrder(DurationFaultTable, &DurationFaultSpec::fault),
                  "DurationFaultTable must hold one row per DurationFault, in enumerator order");

    /// @return True for an ASCII digit, and nothing else a locale might call one.
    [[nodiscard]] constexpr bool IsAsciiDigit(char c) noexcept
    {
        return c >= '0' && c <= '9';
    }

} // namespace

std::expected<std::chrono::milliseconds, DurationFault> ParseDuration(std::string_view text) noexcept
{
    if (text.empty())
        return std::unexpected(DurationFault::Empty);
    // Asked before the digits are, so `-5s` is refused as what it is rather than as "not a number".
    if (text.front() == '-')
        return std::unexpected(DurationFault::Negative);

    auto const digitCount = static_cast<std::size_t>(std::ranges::distance(text | std::views::take_while(IsAsciiDigit)));
    auto const digits = text.substr(0, digitCount);
    auto const suffix = text.substr(digitCount);
    if (digits.empty())
        return std::unexpected(DurationFault::NotANumber);
    if (suffix.empty())
        return std::unexpected(DurationFault::MissingUnit);

    auto const* const unit = core::findOrNull(DurationUnitTable, suffix, &DurationUnitSpec::suffix);
    if (unit == nullptr)
        // A fraction's point is not a unit that happens to be unknown: `1.5s` is told to write whole numbers.
        return std::unexpected(suffix.front() == '.' || suffix.front() == ',' ? DurationFault::NotANumber
                                                                              : DurationFault::UnknownUnit);

    auto count = std::uint64_t { 0 };
    auto const read = std::from_chars(digits.data(), digits.data() + digits.size(), count);
    if (read.ec == std::errc::result_out_of_range)
        return std::unexpected(DurationFault::Overflow);
    if (read.ec != std::errc {} || read.ptr != digits.data() + digits.size())
        return std::unexpected(DurationFault::NotANumber);

    // Checked BEFORE the multiplication, which would otherwise wrap into a plausible length.
    auto const ceiling = static_cast<std::uint64_t>(std::chrono::milliseconds::max().count())
                         / static_cast<std::uint64_t>(unit->length.count());
    if (count > ceiling)
        return std::unexpected(DurationFault::Overflow);
    return std::chrono::milliseconds { static_cast<std::chrono::milliseconds::rep>(count) * unit->length.count() };
}

std::string FormatDuration(std::chrono::milliseconds value)
{
    // A negative length is no setting's value -- `ParseDuration` refuses one -- so printing one would print what
    // cannot be pasted back.
    assert(value >= std::chrono::milliseconds::zero());
    if (value == std::chrono::milliseconds::zero())
        return "0s";
    for (auto const& unit: DurationUnitTable | std::views::reverse)
    {
        if (value % unit.length == std::chrono::milliseconds::zero())
            return std::format("{}{}", value / unit.length, unit.suffix);
    }
    // The first row is one millisecond, which divides everything; the table's static_assert keeps it first.
    return std::format("{}ms", value.count());
}

std::string DurationUnitList()
{
    auto text = std::string {};
    for (auto const& unit: DurationUnitTable)
    {
        if (!text.empty())
            text += ", ";
        text += unit.suffix;
    }
    return text;
}

std::string DescribeDurationFault(DurationFault fault, std::string_view text, std::chrono::milliseconds resolution)
{
    return DurationFaultTable.at(static_cast<std::size_t>(fault)).describe(text, resolution);
}

ConfigError DurationError(DurationFault fault, std::string_view text, std::chrono::milliseconds resolution)
{
    return ConfigError { .code = DurationFaultTable.at(static_cast<std::size_t>(fault)).code,
                         .source = {},
                         .line = 0,
                         .field = {},
                         .context = DescribeDurationFault(fault, text, resolution) };
}

} // namespace FastCache
