// SPDX-License-Identifier: Apache-2.0
#include "DependencyOutput.hpp"
#include "DirectManifest.hpp"
#include "NotePathCollapse.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;
using PathCanon::Grammar;

namespace
{

// `cl` is not the only spelling of the prefix; a localized toolchain has its own.
constexpr std::string_view GermanMarker = "Hinweis: Einlesen der Datei:";

} // namespace

// ---------------------------------------------------------------------------
// CollapseNotePaths -- the region rule

TEST_CASE("A note collapses while its depth padding and its neighbours survive", "[note-path-collapse]")
{
    auto const text = std::string { "Note: including file:   " } + R"(D:\ci\src\a\..\b\x.hpp)" + "\r\n"
                      + "a.cpp(3): warning C4100: unreferenced formal parameter\r\n";
    auto const expected = std::string { "Note: including file:   " } + R"(D:\ci\src\b\x.hpp)" + "\r\n"
                          + "a.cpp(3): warning C4100: unreferenced formal parameter\r\n";

    CHECK(CollapseNotePaths(text, Grammar::ShowIncludes, IncludeNoteMarker) == expected);
}

TEST_CASE("A marker quoted inside a diagnostic is not a note", "[note-path-collapse]")
{
    // One half of the #1270 discriminating pair. The fixture's path carries a collapsible `..` on
    // purpose: with a `..`-free path a loosened anchor would produce identical bytes and the case
    // could not fail for the reason it exists.
    auto const text =
        std::string { R"(a.cpp(3): warning: "Note: including file: D:\ci\src\a\..\b\x.hpp" is unused)" } + "\r\n";
    CHECK(CollapseNotePaths(text, Grammar::ShowIncludes, IncludeNoteMarker) == text);
}

TEST_CASE("Blanks in front of the marker mean the line is not a note", "[note-path-collapse]")
{
    // The other half of the #1270 pair, and the reason recognition stays anchored at column zero:
    // `cl` pads AFTER the marker, never before it, so an indented line beginning with the marker is
    // far likelier to be preprocessed source than a note. Collapsible `..` again, for the same
    // reason as above.
    auto const text = std::string { "  Note: including file: " } + R"(D:\ci\src\a\..\b\x.hpp)" + "\r\n";
    CHECK(CollapseNotePaths(text, Grammar::ShowIncludes, IncludeNoteMarker) == text);
}

TEST_CASE("A localized build's notes collapse and keep their own prefix", "[note-path-collapse]")
{
    auto const german = std::string { GermanMarker } + " " + R"(D:\ci\src\a\..\b\x.hpp)" + "\r\n";
    auto const collapsed = CollapseNotePaths(german, Grammar::ShowIncludes, GermanMarker);

    CHECK(collapsed == std::string { GermanMarker } + " " + R"(D:\ci\src\b\x.hpp)" + "\r\n");
    // The canonical marker must not leak out: restoring it is the other half of the sandwich, and
    // dropping that half would hand Ninja a prefix its msvc_deps_prefix does not match.
    CHECK_FALSE(collapsed.contains(IncludeNoteMarker));

    // The control that makes the case above mean something: the same bytes under the CANONICAL
    // marker match no note at all, so a passing assertion is not just pass-through.
    CHECK(CollapseNotePaths(german, Grammar::ShowIncludes, IncludeNoteMarker) == german);
}

TEST_CASE("A note with nothing to collapse is byte-identical through the whole sandwich", "[note-path-collapse]")
{
    auto const english = std::string { IncludeNoteMarker } + " " + R"(D:\ci\src\b\x.hpp)" + "\r\n";
    CHECK(CollapseNotePaths(english, Grammar::ShowIncludes, IncludeNoteMarker) == english);

    auto const german = std::string { GermanMarker } + " " + R"(D:\ci\src\b\x.hpp)" + "\r\n";
    CHECK(CollapseNotePaths(german, Grammar::ShowIncludes, GermanMarker) == german);
}

TEST_CASE("Only the showIncludes grammar is length-limited", "[note-path-collapse]")
{
    // Ninja's depfile reader carries no `_MAX_PATH` check, so a GNU diagnostic is left alone.
    constexpr std::string_view diagnostic = "/home/dev/proj/src/a/../b/x.cpp:3:5: warning: unused\n";
    CHECK(CollapseNotePaths(diagnostic, Grammar::GccDiagnostics, IncludeNoteMarker) == diagnostic);

    // The control: under ShowIncludes the same bytes are ALSO unchanged, because the line is not a
    // note either way. Without it the case above would pass on the line's shape rather than on the
    // grammar gate. Note this pair cannot see an implementation that hardcodes ShowIncludes.
    CHECK(CollapseNotePaths(diagnostic, Grammar::ShowIncludes, IncludeNoteMarker) == diagnostic);
}

TEST_CASE("On an MSVC stream the notes collapse and the diagnostics do not", "[note-path-collapse]")
{
    // The stream grammar finds diagnostic paths too, and only a note is length-limited: a warning is
    // printed rather than parsed, so its path stays exactly as the compiler spelled it.
    std::string const stream = std::string { IncludeNoteMarker } + " " + R"(D:\ci\src\a\..\b\x.hpp)" + "\r\n"
                               + R"(D:\ci\src\a\..\b\x.hpp(3): warning C4100: 'x': unreferenced parameter)" + "\r\n";
    CHECK(CollapseNotePaths(stream, Grammar::MsvcStream, IncludeNoteMarker)
          == std::string { IncludeNoteMarker } + " " + R"(D:\ci\src\b\x.hpp)" + "\r\n"
                 + R"(D:\ci\src\a\..\b\x.hpp(3): warning C4100: 'x': unreferenced parameter)" + "\r\n");
}

TEST_CASE("The collapse never adds or removes a note", "[note-path-collapse]")
{
    // The property the MaterializeHit ordering rule protects: the stale-hit guard must never start
    // finding fewer dependencies than were emitted. main.cpp is in no test target, so this pins the
    // invariant at the seam that is reachable.
    auto const text = std::string { IncludeNoteMarker } + "  " + R"(D:\ci\src\a\..\b\x.hpp)" + "\r\n"
                      + std::string { IncludeNoteMarker } + "   " + R"(D:\ci\src\c\..\d\y.hpp)" + "\r\n";
    auto const collapsed = CollapseNotePaths(text, Grammar::ShowIncludes, IncludeNoteMarker);

    CHECK(ParseIncludePaths(collapsed).size() == ParseIncludePaths(text).size());
    CHECK(ParseIncludePaths(collapsed).size() == 2);
}

TEST_CASE("A final note with no line terminator survives the rewrite", "[note-path-collapse]")
{
    auto const text = std::string { IncludeNoteMarker } + " " + R"(D:\ci\src\a\..\b\x.hpp)";
    CHECK(CollapseNotePaths(text, Grammar::ShowIncludes, IncludeNoteMarker)
          == std::string { IncludeNoteMarker } + " " + R"(D:\ci\src\b\x.hpp)");
}

// "Two spellings of one header become two identical notes" was pinned here as accepted until
// #1593 collapsed the dependency list where it enters. Its replacement asserts the opposite, at
// that boundary: RootReconciler_test, "One header reached by two include chains is one note and
// one depfile entry".
