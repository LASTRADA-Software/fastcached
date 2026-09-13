// SPDX-License-Identifier: Apache-2.0
#include "ScriptedSixelEncoder.hpp"
#include "SixelEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;

namespace
{

/// @p width by @p height pixels, every one the same opaque colour.
/// @param width Columns.
/// @param height Rows.
/// @param red The red channel.
/// @param green The green channel.
/// @param blue The blue channel.
/// @return The RGBA bytes.
[[nodiscard]] std::vector<std::uint8_t> Solid(
    std::size_t width, std::size_t height, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    auto pixels = std::vector<std::uint8_t> {};
    pixels.reserve(width * height * 4);
    std::ranges::for_each(std::views::iota(std::size_t { 0 }, width * height),
                          [&](std::size_t) { pixels.insert(pixels.end(), { red, green, blue, std::uint8_t { 255 } }); });
    return pixels;
}

/// How many palette entries a Sixel body defines.
/// @param body The body.
/// @return The count of `#<n>;2;` definitions.
[[nodiscard]] std::size_t PaletteEntries(std::string_view body)
{
    auto count = std::size_t { 0 };
    auto at = body.find(";2;");
    while (at != std::string_view::npos)
    {
        ++count;
        at = body.find(";2;", at + 1);
    }
    return count;
}

/// The production encoder, which this build must have.
/// @return The encoder.
[[nodiscard]] std::unique_ptr<ISixelEncoder> Production()
{
    auto made = MakeSixelEncoder();
    REQUIRE(made.has_value());
    return std::move(*made);
}

} // namespace

TEST_CASE("the Sixel adapter answers with a body whose raster attributes are the image's size", "[cli][dashboard][sixel]")
{
    // The adapter's whole job is the conversion, so what distinguishes a working one is that the
    // SIZE and the COLOUR made it across: a 3x2 image states 3;2, and a pure red one defines a
    // palette entry of 100 % red.
    auto encoder = Production();
    auto const pixels = Solid(3, 2, 255, 0, 0);
    auto const body = encoder->Encode(RgbaImage { .pixels = pixels, .width = 3, .height = 2 }, 256);
    REQUIRE(body.has_value());
    CHECK(body->starts_with("\"1;1;3;2"));
    CHECK(body->contains(";2;100;0;0"));
    // A body, not a sequence: the introducer is the terminal sink's to write.
    CHECK(!body->contains('\x1b'));
}

TEST_CASE("encoding one image twice gives the same bytes", "[cli][dashboard][sixel]")
{
    // The property measured before this seam existed, kept as a test so a change to the vendored
    // encoder that brings in nondeterminism within a build is seen here. It says nothing about
    // other standard libraries, which differ (SixelEncoder.hpp).
    auto encoder = Production();
    auto pixels = std::vector<std::uint8_t> {};
    auto state = std::uint32_t { 12345 };
    std::ranges::for_each(std::views::iota(std::size_t { 0 }, std::size_t { 40 * 30 }), [&](std::size_t) {
        state = (state * 1664525U) + 1013904223U;
        pixels.insert(pixels.end(),
                      { static_cast<std::uint8_t>(state >> 24U),
                        static_cast<std::uint8_t>(state >> 16U),
                        static_cast<std::uint8_t>(state >> 8U),
                        std::uint8_t { 255 } });
    });
    auto const image = RgbaImage { .pixels = pixels, .width = 40, .height = 30 };
    auto const first = encoder->Encode(image, 64);
    auto const second = encoder->Encode(image, 64);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
    CHECK(first->size() > 100);
}

TEST_CASE("a Sixel image with no width or one past what the encoder addresses is refused by name", "[cli][dashboard][sixel]")
{
    // A dimension past `int` would wrap into a size the vendored encoder then trusts. WHAT
    // DISTINGUISHES: the refusal names the dimension -- the pixels here are far too few, and an
    // adapter that narrowed first would be refused for THAT instead.
    auto encoder = Production();
    auto const pixels = Solid(1, 1, 0, 0, 0);
    auto const empty = encoder->Encode(RgbaImage { .pixels = pixels, .width = 0, .height = 1 }, 16);
    auto const vast = encoder->Encode(
        RgbaImage { .pixels = pixels, .width = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1, .height = 1 },
        16);
    REQUIRE(!empty.has_value());
    REQUIRE(!vast.has_value());
    CHECK(empty.error().contains("width and a height"));
    CHECK(vast.error().contains("wider or taller"));
}

TEST_CASE("pixel data shorter than the image is refused with the encoder's own reason", "[cli][dashboard][sixel]")
{
    auto encoder = Production();
    auto const pixels = Solid(2, 1, 0, 0, 0);
    auto const body = encoder->Encode(RgbaImage { .pixels = pixels, .width = 4, .height = 4 }, 16);
    REQUIRE(!body.has_value());
    CHECK(body.error().contains("too small"));
}

TEST_CASE("the palette ceiling is clamped rather than narrowed", "[cli][dashboard][sixel]")
{
    // A ceiling past `int` must mean "as many as allowed", not whatever its low bits say. WHAT
    // DISTINGUISHES: 2^32 + 2 narrowed is 2, so four distinct colours would share two palette
    // entries; clamped, each keeps its own.
    auto encoder = Production();
    auto pixels = Solid(1, 1, 255, 0, 0);
    for (auto const& more: { Solid(1, 1, 0, 255, 0), Solid(1, 1, 0, 0, 255), Solid(1, 1, 255, 255, 255) })
        pixels.insert(pixels.end(), more.begin(), more.end());
    auto const huge = (std::size_t { 1 } << 32U) + 2;
    auto const body = encoder->Encode(RgbaImage { .pixels = pixels, .width = 4, .height = 1 }, huge);
    REQUIRE(body.has_value());
    CHECK(PaletteEntries(*body) >= 4);
}

TEST_CASE("a fully transparent image encodes to an empty body", "[cli][dashboard][sixel]")
{
    auto encoder = Production();
    auto const pixels = std::vector<std::uint8_t>(4 * 4 * 4, 0);
    auto const body = encoder->Encode(RgbaImage { .pixels = pixels, .width = 4, .height = 4 }, 16);
    REQUIRE(body.has_value());
    CHECK(body->empty());
}

TEST_CASE("the scripted Sixel encoder records what it was asked and answers from its script", "[cli][dashboard][sixel]")
{
    // The fake a view test draws through, so its own bugs would surface as a view passing for the
    // wrong reason. It copies the pixels because the image is borrowed only for the call.
    auto fake = ScriptedSixelEncoder {};
    auto pixels = Solid(2, 3, 1, 2, 3);
    auto const body = fake.Encode(RgbaImage { .pixels = pixels, .width = 2, .height = 3 }, 64);
    pixels.assign(pixels.size(), 0);

    REQUIRE(body.has_value());
    CHECK(*body == "sixel:2x3");
    REQUIRE(fake.Requests().size() == 1);
    CHECK(fake.Requests()[0].width == 2);
    CHECK(fake.Requests()[0].height == 3);
    CHECK(fake.Requests()[0].maxColors == 64);
    CHECK(fake.Requests()[0].pixels == Solid(2, 3, 1, 2, 3));

    auto refusing = ScriptedSixelEncoder { "no palette" };
    auto const refused = refusing.Encode(RgbaImage { .pixels = pixels, .width = 2, .height = 3 }, 64);
    REQUIRE(!refused.has_value());
    CHECK(refused.error() == "no palette");
    CHECK(refusing.Requests().size() == 1);
}
