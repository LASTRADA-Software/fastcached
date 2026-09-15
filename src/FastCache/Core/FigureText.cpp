// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/FigureText.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <ranges>
#include <string_view>

namespace FastCache
{

namespace
{
    /// @p value rounded to a whole number and grouped in thousands with a space, which reads in
    /// every locale and is never mistaken for a decimal separator.
    ///
    /// Rounded by FORMATTING rather than through an integer: a counter is a `uint64_t` and can
    /// exceed what `llround` may return, which is unspecified rather than an error.
    /// @param value A finite number.
    /// @return The grouped text.
    [[nodiscard]] std::string Grouped(double value)
    {
        auto const whole = std::format("{:.0f}", std::fabs(value));
        auto grouped = std::string {};
        grouped.reserve(whole.size() + (whole.size() / 3) + 1);
        if (std::signbit(value) && whole != "0")
            grouped.push_back('-');
        for (auto const index: std::views::iota(std::size_t { 0 }, whole.size()))
        {
            if (index != 0 && (whole.size() - index) % 3 == 0)
                grouped.push_back(' ');
            grouped.push_back(whole[index]);
        }
        return grouped;
    }

    /// One unit a figure may be written in.
    struct FigureUnit
    {
        double scale;          ///< Base units per this unit.
        std::string_view name; ///< What it is called.
    };

    /// The IEC units a byte figure may be written in, largest first.
    constexpr auto ByteUnits = std::array {
        FigureUnit { .scale = 1024.0 * 1024.0 * 1024.0 * 1024.0, .name = "TiB" },
        FigureUnit { .scale = 1024.0 * 1024.0 * 1024.0, .name = "GiB" },
        FigureUnit { .scale = 1024.0 * 1024.0, .name = "MiB" },
        FigureUnit { .scale = 1024.0, .name = "KiB" },
    };

    /// @p number followed by the unit called @p name, a space between them.
    /// @param number The written number.
    /// @param name The unit.
    /// @return The figure.
    [[nodiscard]] WrittenFigure InUnit(std::string number, std::string_view name)
    {
        return WrittenFigure { .number = std::move(number), .unit = std::format(" {}", name) };
    }

    /// A byte count in the largest unit it fills at least once.
    /// @param value Bytes.
    /// @return The figure.
    [[nodiscard]] WrittenFigure Bytes(double value)
    {
        for (auto const& unit: ByteUnits)
        {
            auto const scaled = value / unit.scale;
            if (scaled >= 1.0)
                return InUnit(scaled < 100.0 ? std::format("{:.2f}", scaled) : std::format("{:.1f}", scaled), unit.name);
        }
        return InUnit(std::format("{:.0f}", value), "B");
    }

    /// The units a duration past a minute is written in, largest first.
    ///
    /// Seconds below a minute keep two decimals, which is what a mean compile time needs; a lease
    /// that has been out for an hour reads as `1.0 h`, never `3600.00 s`.
    constexpr auto LongDurationUnits = std::array {
        FigureUnit { .scale = 3600.0, .name = "h" },
        FigureUnit { .scale = 60.0, .name = "min" },
    };

    /// A duration in seconds.
    /// @param value Seconds.
    /// @return The figure.
    [[nodiscard]] WrittenFigure Seconds(double value)
    {
        for (auto const& unit: LongDurationUnits)
            if (std::fabs(value) >= unit.scale)
                return InUnit(std::format("{:.1f}", value / unit.scale), unit.name);
        return InUnit(std::format("{:.2f}", value), "s");
    }

    /// How one figure format writes a present, finite value.
    struct FigureFormatSpec
    {
        FigureFormat format;                  ///< The enumerator this row describes.
        WrittenFigure (*write)(double value); ///< The writer.
    };

    /// The figure formats, one row per enumerator, in enumerator order.
    constexpr EnumTable<FigureFormat, FigureFormatSpec> FigureFormatTable { {
        { .format = FigureFormat::Count, .write = [](double value) { return WrittenFigure { .number = Grouped(value) }; } },
        { .format = FigureFormat::Rate,
          .write =
              [](double value) {
                  return WrittenFigure { .number =
                                             std::fabs(value) >= 10.0 ? Grouped(value) : std::format("{:.1f}", value) };
              } },
        { .format = FigureFormat::Percent,
          .write = [](double value) { return InUnit(std::format("{:.1f}", value * 100.0), "%"); } },
        { .format = FigureFormat::Bytes, .write = &Bytes },
        { .format = FigureFormat::Seconds, .write = &Seconds },
    } };

    static_assert(RowsInEnumeratorOrder(FigureFormatTable, &FigureFormatSpec::format),
                  "FigureFormatTable must hold one row per FigureFormat, in enumerator order");

    /// How one figure format is written for a program rather than a person.
    struct MachineFigureRow
    {
        FigureFormat format; ///< The enumerator this row describes.
        int decimals;        ///< How many digits follow the decimal point; zero writes no point at all.
    };

    /// One row per `FigureFormat`, in enumerator order: whole numbers stay whole, and a share keeps the digits
    /// its page percentage is drawn from and one more.
    constexpr EnumTable<FigureFormat, MachineFigureRow> MachineFigureTable { {
        { .format = FigureFormat::Count, .decimals = 0 },
        { .format = FigureFormat::Rate, .decimals = 3 },
        { .format = FigureFormat::Percent, .decimals = 4 },
        { .format = FigureFormat::Bytes, .decimals = 0 },
        { .format = FigureFormat::Seconds, .decimals = 3 },
    } };

    static_assert(RowsInEnumeratorOrder(MachineFigureTable, &MachineFigureRow::format),
                  "MachineFigureTable must hold one row per FigureFormat, in enumerator order");
} // namespace

WrittenFigure WriteFigure(double value, FigureFormat format)
{
    return FigureFormatTable[static_cast<std::size_t>(format)].write(value);
}

std::string WriteMachineFigure(double value, FigureFormat format)
{
    return std::format("{:.{}f}", value, MachineFigureTable[static_cast<std::size_t>(format)].decimals);
}

} // namespace FastCache
