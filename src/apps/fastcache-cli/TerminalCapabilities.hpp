// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardFrame.hpp"
#include "DashboardRung.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/Terminal.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace FastCache::Cli
{

/// @file TerminalCapabilities.hpp
/// What a terminal can draw, as one value, and the one function that turns it into a rung.
///
/// **A value, not a query.** The record is filled once, before the dashboard loop, and handed to
/// `ChooseRenderRung`; nothing downstream asks a terminal or the environment anything (#134 §6.5,
/// §9.11, §9.12). A fixture substitutes the record, which is how the ASCII rung is driven without
/// a terminal that lacks Unicode.
///
/// It names nothing from core-cpp: its DA1 and DECRQM answers are translated into
/// `SixelAnswer` and `SynchronizedOutputAnswer` where they are read, in `TerminalEventStream.cpp`.

/// What the terminal said about Sixel graphics, or why it said nothing.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// Four values because the two non-answers are different facts, and both are the reason a
/// dashboard draws no Sixel: a query nobody sent is not a terminal that declined, and a terminal
/// that stayed silent until the deadline is neither.
enum class SixelAnswer : std::uint8_t
{
    Advertised,    ///< DA1 answered and listed attribute 4.
    NotAdvertised, ///< DA1 answered without attribute 4.
    NoReply,       ///< DA1 was sent and nothing answered before the query deadline.
    NotAsked,      ///< No DA1 was sent: there was no terminal input to read a reply from.
};

/// What the terminal said about synchronized output (DEC mode 2026), or why it said nothing.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// Folded from a DECRQM reply rather than carried as one, because a presenter needs one fact --
/// may a frame be bracketed -- and the reply has eight. Kept four-valued for `SixelAnswer`'s
/// reason: only `Supported` brackets a frame, and the other three are different facts about why
/// not, which a report can tell apart.
enum class SynchronizedOutputAnswer : std::uint8_t
{
    Supported,    ///< DECRQM recognised mode 2026 and it can be switched: set or reset.
    NotSupported, ///< DECRQM answered that mode 2026 is not recognised, or permanently reset.
    NoReply,      ///< DECRQM was sent and nothing answered before the query deadline.
    NotAsked,     ///< No DECRQM was sent: there was no terminal input to read a reply from.
};

/// Whether a frame's dressed runs may be written with colour and attributes, and why not.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// Decided once, where the rest of the record is, from the one resolution this program makes of
/// `--color` (`always`, `never`, or `auto`: a terminal, unless `NO_COLOR` says otherwise). Only
/// `Supported` dresses anything; the other two are different facts about why a frame is plain.
enum class ColourAnswer : std::uint8_t
{
    Supported,  ///< Colour and attributes are written.
    Suppressed, ///< `--color` resolved to plain: `never`, `NO_COLOR`, or no terminal to colour.
    NotAsked,   ///< Nothing decided it; a record nobody filled is plain.
};

/// How one `FrameTone` looks where colour is allowed.
///
/// **Colour by palette index, never by value**, so a terminal theme's own green is what an operator
/// sees, and a light theme stays legible: a tone says *fine*, *look at this* or *act*, never a shade.
struct TonePaletteRow
{
    FrameTone tone;                     ///< The enumerator this row describes.
    std::optional<std::uint8_t> colour; ///< The foreground's palette index, or none to keep the terminal's.
    bool bold;                          ///< Whether the run is bold.
    bool dim;                           ///< Whether it is dimmed.
    bool inverse;                       ///< Whether it is drawn in inverse video.
};

/// The one palette: a row per `FrameTone`, in enumerator order (#134 G1).
///
/// Figures carry weight and what names them recedes; the active choice is inverse; state is the
/// terminal's green, yellow and red. Every panel's dressed runs are looked up here and nowhere else.
inline constexpr EnumTable<FrameTone, TonePaletteRow> TonePalette { {
    { .tone = FrameTone::Selected, .colour = std::nullopt, .bold = false, .dim = false, .inverse = true },
    { .tone = FrameTone::Figure, .colour = std::nullopt, .bold = true, .dim = false, .inverse = false },
    { .tone = FrameTone::Label, .colour = std::nullopt, .bold = false, .dim = true, .inverse = false },
    { .tone = FrameTone::Fresh, .colour = std::uint8_t { 2 }, .bold = false, .dim = false, .inverse = false },
    { .tone = FrameTone::Stale, .colour = std::uint8_t { 3 }, .bold = true, .dim = false, .inverse = false },
    { .tone = FrameTone::Alert, .colour = std::uint8_t { 1 }, .bold = true, .dim = false, .inverse = false },
    { .tone = FrameTone::LevelLow, .colour = std::uint8_t { 2 }, .bold = false, .dim = false, .inverse = false },
    { .tone = FrameTone::LevelMid, .colour = std::uint8_t { 3 }, .bold = false, .dim = false, .inverse = false },
    { .tone = FrameTone::LevelHigh, .colour = std::uint8_t { 1 }, .bold = false, .dim = false, .inverse = false },
} };

static_assert(RowsInEnumeratorOrder(TonePalette, &TonePaletteRow::tone),
              "TonePalette must hold one row per FrameTone, in enumerator order");

/// Everything the rung is decided from, and what a frame may be presented with.
struct TerminalCapabilities
{
    SixelAnswer sixel { SixelAnswer::NotAsked };

    /// What the locale (POSIX) or the console output code page (Windows) says the terminal draws.
    /// `Unknown` means there was nothing to read, and it is carried as such rather than folded
    /// into `Other`, so a report can say *unknown* instead of claiming the terminal lacks Unicode.
    TerminalTextEncoding encoding { TerminalTextEncoding::Unknown };

    /// Whether a frame may be bracketed in synchronized output. Read by the presenter, not the rung.
    SynchronizedOutputAnswer synchronizedOutput { SynchronizedOutputAnswer::NotAsked };

    /// Whether a frame's dressed runs are written with the palette. Read by the presenter, not the rung:
    /// a plain frame is the same grid, the active section in `[brackets]` either way.
    ColourAnswer colour { ColourAnswer::NotAsked };

    /// How many pixels a cell measures (`CSI 16 t`), or nullopt when the terminal did not say.
    ///
    /// Nullopt is not a default size to assume: an image sized from a guess is the wrong size while
    /// every figure beside it is right. So without it the Sixel rung is not chosen at all.
    std::optional<CellPixelSize> cellPixels {};
};

/// How @p tone is dressed on a terminal with @p capabilities.
/// @param tone A tone below `Last`.
/// @param capabilities The record.
/// @return The palette row, or null where the record allows no colour and the run is written plain.
[[nodiscard]] constexpr TonePaletteRow const* PaletteFor(FrameTone tone, TerminalCapabilities const& capabilities) noexcept
{
    if (capabilities.colour != ColourAnswer::Supported || tone >= FrameTone::Last)
        return nullptr;
    return &TonePalette[static_cast<std::size_t>(tone)];
}

/// The rung a terminal with @p capabilities is drawn on.
///
/// Sixel when the terminal advertised it AND reported its cell size -- an image cannot be placed in
/// cells whose pixels nobody measured. Otherwise Unicode when the encoding is UTF-8, and ASCII
/// for anything else, UNKNOWN included: ASCII keeps every figure and every absent marker (§10), so
/// it is the floor a guess falls to. `Piped` is not an answer here, because a run without a terminal
/// never has capabilities to ask about.
/// @param capabilities The record.
/// @return The rung.
[[nodiscard]] RenderRung ChooseRenderRung(TerminalCapabilities const& capabilities) noexcept;

} // namespace FastCache::Cli
