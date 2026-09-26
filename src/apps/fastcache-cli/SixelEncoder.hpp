// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace FastCache::Cli
{

/// @file SixelEncoder.hpp
/// Turning pixels into Sixel data: the Sixel rung's one door to core-cpp's terminal UI.
///
/// **One function behind an interface, and core-cpp's encoder behind exactly one file.** The
/// dashboard draws a chart as RGBA pixels and hands them here; `TuiSixelEncoder.cpp` is the only
/// file of this binary that includes `<core/tui/Sixel.hpp>`, so core-cpp's vocabulary meets the
/// first-party one in one place (widen the adapter, never add a crossing). A
/// test substitutes `Testing::ScriptedSixelEncoder` and needs no TUI at all.
///
/// **The same image encodes to the same bytes on every standard library.** Core-cpp's median-cut
/// quantizer orders a bucket's pixels by the whole colour, its widest channel first, so no split
/// follows the order a sort happened to leave equal keys in. It once sorted on the widest channel
/// alone, unstably, and libstdc++, libc++ and MSVC's library each encoded one image to different
/// bytes. So one event list renders one Sixel frame everywhere, and `SixelEncoder_test.cpp` pins an
/// encoding's digest at this seam -- the image and the digest core-cpp's `core/tui/Sixel_test.cpp`
/// pins below it.

/// RGBA pixels, row-major, four bytes per pixel, borrowed for the call.
struct RgbaImage
{
    std::span<std::uint8_t const> pixels; ///< `width * height * 4` bytes, or more.
    std::size_t width { 0 };              ///< Columns of pixels.
    std::size_t height { 0 };             ///< Rows of pixels.
};

/// Encodes pixels as the body of a Sixel sequence.
class ISixelEncoder
{
  public:
    ISixelEncoder() = default;
    ISixelEncoder(ISixelEncoder const&) = delete;
    ISixelEncoder(ISixelEncoder&&) = delete;
    ISixelEncoder& operator=(ISixelEncoder const&) = delete;
    ISixelEncoder& operator=(ISixelEncoder&&) = delete;
    virtual ~ISixelEncoder() = default;

    /// Encode @p image.
    ///
    /// The BODY only -- raster attributes, palette and bands -- without the `DCS q` introducer or
    /// the string terminator: wrapping it is the terminal sink's business, as placing a text frame
    /// is. A fully transparent image encodes to an empty body.
    /// @param image The pixels.
    /// @param maxColors The palette ceiling; the encoder clamps it into [1, 256].
    /// @return The body, or why the image cannot be encoded.
    [[nodiscard]] virtual std::expected<std::string, std::string> Encode(RgbaImage const& image, std::size_t maxColors) = 0;
};

/// The production encoder, or why this build has none.
///
/// A build without core-cpp's terminal UI (`FASTCACHED_BUILD_TUI=OFF`) refuses by saying so, rather than
/// failing to link, so the composition that asks for one can report why the Sixel rung is out of
/// reach.
/// @return The encoder, or the reason there is none.
[[nodiscard]] std::expected<std::unique_ptr<ISixelEncoder>, std::string> MakeSixelEncoder();

} // namespace FastCache::Cli
