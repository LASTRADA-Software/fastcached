// SPDX-License-Identifier: Apache-2.0
#include "SixelEncoder.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <tui/Sixel.hpp>

namespace FastCache::Cli
{

/// @file EndoSixelEncoder.cpp
/// The adapter from `ISixelEncoder` to the vendored `tui::encodeSixel`.
///
/// **The only file of fastcache-cli that includes `<tui/Sixel.hpp>`.** Everything the vendored
/// signature needs that the first-party one does not say -- `int` dimensions, an `int` colour
/// ceiling, `tui::Result` -- is converted here and nowhere else.

namespace
{
    /// The widest palette a Sixel encoder here is asked for.
    constexpr auto MostColors = std::size_t { 256 };

    /// The vendored encoder behind the first-party seam.
    class EndoSixelEncoder final: public ISixelEncoder
    {
      public:
        [[nodiscard]] std::expected<std::string, std::string> Encode(RgbaImage const& image, std::size_t maxColors) override
        {
            // Refused HERE rather than narrowed: a dimension past `int` would wrap into a size
            // the vendored encoder then trusts, and a zero is a caller's mistake worth naming.
            constexpr auto Widest = static_cast<std::size_t>(std::numeric_limits<int>::max());
            if (image.width == 0 || image.height == 0)
                return std::unexpected(std::string { "a Sixel image needs a width and a height" });
            if (image.width > Widest || image.height > Widest)
                return std::unexpected(std::string { "a Sixel image is wider or taller than the encoder can address" });

            auto const colors = static_cast<int>(std::clamp(maxColors, std::size_t { 1 }, MostColors));
            auto encoded = tui::encodeSixel(tui::ImageData { .pixels = image.pixels,
                                                             .width = static_cast<int>(image.width),
                                                             .height = static_cast<int>(image.height) },
                                            colors);
            if (!encoded.has_value())
                return std::unexpected(std::move(encoded.error()));
            return std::move(*encoded);
        }
    };

} // namespace

std::expected<std::unique_ptr<ISixelEncoder>, std::string> MakeSixelEncoder()
{
    return std::make_unique<EndoSixelEncoder>();
}

} // namespace FastCache::Cli
