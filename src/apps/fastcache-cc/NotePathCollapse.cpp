// SPDX-License-Identifier: Apache-2.0
#include "NotePathCollapse.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <string>
#include <string_view>

namespace FastCache::Cc
{

std::string CollapseNotePaths(std::string_view text, PathCanon::Grammar grammar, std::string_view marker)
{
    // Only Ninja's `/showIncludes` reader carries the length check; its depfile reader does not.
    // One gate here beats the same `if` at every call site.
    if (!PathCanon::CarriesIncludeNotes(grammar))
        return std::string { text };

    // `PathCanon`'s span finder matches the canonical marker and nothing else, so a localized
    // build's notes are invisible to it unless the marker is normalized in first. Both rewrites
    // short-circuit to a copy when the two spellings are equal, which is every English build.
    //
    // Walked under `ShowIncludes` whatever the region's own grammar: `MsvcStream` also finds
    // diagnostic paths, and only a NOTE is what Ninja parses and length-limits. So a MISS prints a
    // diagnostic as the compiler spelled it, `..` and all -- while a HIT prints the one the server
    // STORED, which the server collapsed on the way in (generation 6 on). The two differ in spelling and name the
    // same file; left that way on purpose, because re-spelling the miss's own output buys nothing
    // a developer reading it needs (Job 2 review, M5).
    auto const canonical = PathCanon::NormalizeIncludeNoteMarker(text, marker);
    auto const collapsed = PathCanon::RewritePaths(canonical, PathCanon::Grammar::ShowIncludes, [](std::string_view span) {
        return PathCanon::CollapseRelativeSegments(span);
    });
    return PathCanon::RestoreIncludeNoteMarker(collapsed, marker);
}

} // namespace FastCache::Cc
