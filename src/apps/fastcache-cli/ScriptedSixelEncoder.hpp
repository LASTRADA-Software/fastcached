// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "SixelEncoder.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cli::Testing
{

/// @file ScriptedSixelEncoder.hpp
/// A Sixel encoder that writes down what it was asked and answers from a script.
///
/// **Its body names the image rather than encoding it**, and that is deliberate: a view test asserts
/// WHAT it asked to have drawn -- the dimensions, the pixels, the palette ceiling -- and this fake is
/// what makes that the only thing it can assert. Real Sixel bytes are the quantizer's answer, so a
/// view test comparing them would move with every change to core-cpp's encoder while saying nothing
/// about the view; the encoder's own bytes are pinned once, at the seam (`SixelEncoder_test.cpp`).

/// One call the fake received.
struct SixelRequest
{
    std::vector<std::uint8_t> pixels; ///< A copy: the image is borrowed only for the call.
    std::size_t width { 0 };          ///< As given.
    std::size_t height { 0 };         ///< As given.
    std::size_t maxColors { 0 };      ///< As given.
};

class ScriptedSixelEncoder final: public ISixelEncoder
{
  public:
    /// Answer every call with a body naming the image's size.
    ScriptedSixelEncoder() = default;

    /// Answer every call with @p refusal.
    /// @param refusal Why the image cannot be encoded.
    explicit ScriptedSixelEncoder(std::string refusal):
        _refusal { std::move(refusal) }
    {
    }

    [[nodiscard]] std::expected<std::string, std::string> Encode(RgbaImage const& image, std::size_t maxColors) override
    {
        _requests.push_back(SixelRequest { .pixels = { image.pixels.begin(), image.pixels.end() },
                                           .width = image.width,
                                           .height = image.height,
                                           .maxColors = maxColors });
        if (!_refusal.empty())
            return std::unexpected(_refusal);
        return std::format("sixel:{}x{}", image.width, image.height);
    }

    /// Every call, in order.
    /// @return The calls.
    [[nodiscard]] std::vector<SixelRequest> const& Requests() const noexcept
    {
        return _requests;
    }

  private:
    std::string _refusal {};
    std::vector<SixelRequest> _requests {};
};

} // namespace FastCache::Cli::Testing
