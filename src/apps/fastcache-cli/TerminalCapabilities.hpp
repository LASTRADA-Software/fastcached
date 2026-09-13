// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardFrame.hpp"
#include "DashboardRung.hpp"

#include <FastCache/Platform/Terminal.hpp>

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
/// It names nothing from `vendor/`: endo's DA1 and DECRQM answers are translated into
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

    /// How many pixels a cell measures (`CSI 16 t`), or nullopt when the terminal did not say.
    ///
    /// Nullopt is not a default size to assume: an image sized from a guess is the wrong size while
    /// every figure beside it is right. So without it the Sixel rung is not chosen at all.
    std::optional<CellPixelSize> cellPixels {};
};

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
