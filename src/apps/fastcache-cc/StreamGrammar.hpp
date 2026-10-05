// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

namespace FastCache::Cc
{

/// The grammar to tag a stored TEXT REGION with, per compiler flavor.
///
/// Both console streams of one compile carry the same tag, and that tag must cover every line
/// language the stream can carry -- a line its grammar cannot see is stored verbatim, and a hit
/// then replays the path of the checkout that STORED it. On a machine with several checkouts
/// that path resolves to a different tree at a possibly different revision, so the line number
/// lands on unrelated code and the developer edits the wrong one.
///
/// - **The GNU family** writes diagnostics and no `/showIncludes`, so `GccDiagnostics` (#202).
/// - **The MSVC family** writes notes AND diagnostics, and the channel follows the FLAG rather
///   than the driver: `cl` and `clang-cl` put the notes on stdout under `/c` and on stderr under
///   `/EP` (#825). So both streams need both languages, which is `MsvcStream`. It was
///   `ShowIncludes` until generation 6 of the stored value, and every MSVC-family warning, error
///   and `note:` a hit replayed carried the producer's path. `.agent/rules/compile-cache.md`
///   holds the channel table; this must not become a second home for it.
///
/// Here rather than in `main.cpp` so the WIRING is testable: a correct grammar nothing tags a
/// region with is exactly the defect generation 6 closes -- `MsvcDiagnostics` existed, was
/// unit-tested, and reached no production region.
/// @param flavor The compiler family.
/// @return The grammar both stored stream regions carry.
[[nodiscard]] constexpr PathCanon::Grammar StreamGrammar(Flavor flavor) noexcept
{
    switch (flavor)
    {
        case Flavor::Gcc:
        case Flavor::Clang:
            return PathCanon::Grammar::GccDiagnostics;
        case Flavor::Cl:
        case Flavor::ClangCl:
        case Flavor::Unknown:
            return PathCanon::Grammar::MsvcStream;
    }
    return PathCanon::Grammar::MsvcStream;
}

} // namespace FastCache::Cc
