// SPDX-License-Identifier: Apache-2.0
#include "SixelEncoder.hpp"

namespace FastCache::Cli
{

/// @file SixelEncoderUnavailable.cpp
/// The Sixel seam for a build without the vendored TUI (`FASTCACHED_BUILD_TUI=OFF`).
///
/// It refuses rather than handing back an encoder that produces nothing: an empty body is what a
/// fully transparent image encodes to, so an encoder answering empty would draw a blank chart
/// where the fact is that this binary cannot draw one.

std::expected<std::unique_ptr<ISixelEncoder>, std::string> MakeSixelEncoder()
{
    return std::unexpected(
        std::string { "this fastcache-cli was built without the terminal UI (FASTCACHED_BUILD_TUI=OFF)" });
}

} // namespace FastCache::Cli
