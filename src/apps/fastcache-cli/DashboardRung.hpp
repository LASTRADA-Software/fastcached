// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace FastCache::Cli
{

/// @file DashboardRung.hpp
/// Which rung of the `live-stats` capability ladder a frame is drawn on.
///
/// **Its own header, and nothing else in it,** because it is the one contract between the
/// code that DETECTS what a terminal can do and the code that DRAWS for it. Detection
/// decides a rung once; every panel receives that decision as an injected value and never
/// asks anything ambient itself. A header carrying either side's machinery would couple
/// the two to more than this enum.

/// A rung of the capability ladder (#134 §10), in degradation order: richest first.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted. The declaration order
/// is the ladder, so a new rung goes in at its place on it rather than at the end.
enum class RenderRung : std::uint8_t
{
    /// Pixel-resolution charts, for a terminal that reported Sixel support.
    Sixel,

    /// Block-element sparklines, box-drawing frames and block gauges.
    Unicode,

    /// `+-|` frames and `[####....]` gauges, keeping every figure and every absent marker.
    ///
    /// The sparkline column is dropped rather than replaced by `#` bars: one level per cell
    /// carries no information and looks like data.
    Ascii,

    /// Not a terminal: one line per sample and no frame.
    Piped,

    /// The number of rungs; not a rung.
    Last,
};

} // namespace FastCache::Cli
