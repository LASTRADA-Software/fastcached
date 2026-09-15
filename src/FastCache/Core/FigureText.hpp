// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace FastCache
{

/// @file FigureText.hpp
/// How a figure is written, for a PERSON and for a PROGRAM, in the one place every surface asks.
///
/// **Two writers, because the two readers want opposite things.** A person reads `93.65 GiB` and
/// `85.3 %`; a program -- `/fleet.txt`, `/fleet.json`, a piped `live-stats` record -- reads
/// `100000000000` and `0.8530`, because a humanised figure has to be parsed back and `awk` cannot.
/// Both live here so an operator looking at the page and the panel sees one vocabulary, and a script
/// reading the leader's document and the CLI's pipe reads one scale.

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

/// @p value, present and finite, written as @p format says for a PROGRAM: no unit, no grouping.
///
/// A whole-number format (`Count`, `Bytes`) is written whole, `Rate` and `Seconds` to three decimals, and a
/// `Percent` -- a share in [0, 1] -- to FOUR, which keeps the digits the page's `85.3 %` is drawn from and
/// one more. Each format's count of decimals is one column of one row, never a caller's argument, so no
/// two surfaces can write one share to different precision.
///
/// **Rounding** is `std::format`'s: the nearest number with that many decimals, and a value exactly halfway
/// between two -- which only a binary fraction can be, such as 1/32 -- goes to the one whose last digit is
/// even: 1/32 is `0.0312`, 3/32 is `0.0938`. The ends are written in full, `1.0000` and `0.0000`.
///
/// Absence is the caller's to spell, as for `WriteFigure`: a share nobody can compute is the surface's
/// absent marker, never `0.0000`.
/// @param value The value; finite.
/// @param format How to write it.
/// @return The lexical form.
[[nodiscard]] std::string WriteMachineFigure(double value, FigureFormat format);

} // namespace FastCache
