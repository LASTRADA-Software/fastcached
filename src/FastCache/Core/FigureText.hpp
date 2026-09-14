// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace FastCache
{

/// @file FigureText.hpp
/// How a figure is written for a PERSON, in the one place every human surface asks.
///
/// **Human surfaces only.** `/fleet.txt`, `/fleet.json`, `/metrics` and a piped `live-stats`
/// record carry the raw integer, because a humanised figure has to be parsed back by whoever reads
/// it and `awk` cannot. What reads this is the browser page and the terminal panels, and they read
/// it through here so an operator looking at both sees one vocabulary: `93.65 GiB` on the page is
/// `93.65 GiB` in the panel, never `93.6 GiB` beside it.

/// How a figure is written.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted.
enum class FigureFormat : std::uint8_t
{
    Count,   ///< A whole number, grouped in thousands: `1 284 991`.
    Rate,    ///< Events per unit time: grouped when at least ten, one decimal below.
    Percent, ///< A fraction in [0, 1] as `94.2 %`.
    Bytes,   ///< IEC binary units, as the node panel has always written them: `3.41 GiB`.
    Seconds, ///< A duration in seconds: `1.84 s`, then `12.5 min`, then `3.2 h`.
    Last,
};

/// A figure written for a person, in its two parts: the number, and the unit written after it.
///
/// **Two parts because a surface dresses them differently** -- #134's G1 gives a number weight and dims its
/// unit -- and only the writer knows where one ends and the other begins. Reading `1 284 991` or `3.41 GiB` back
/// apart would be a second grammar of the same figure, and the grouping space would pass for the unit's.
struct WrittenFigure
{
    std::string number {}; ///< The digits, sign, grouping and decimal point: `94.2`, `1 284 991`.
    std::string unit {};   ///< What follows the number, the space before it included: ` %`, ` GiB`; empty for none.

    /// The figure as a person reads it.
    /// @return The number and the unit.
    [[nodiscard]] std::string Text() const
    {
        return number + unit;
    }
};

/// @p value, present and finite, written as @p format says.
///
/// Absence is the caller's to spell, because the marker differs per surface (`-` in a terminal,
/// `&ndash;` on a page); a caller that has no value does not call this.
/// @param value The value; finite.
/// @param format How to write it.
/// @return The number and its unit.
[[nodiscard]] WrittenFigure WriteFigure(double value, FigureFormat format);

} // namespace FastCache
