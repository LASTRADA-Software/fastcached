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

// The shape and the magnitude of the report, rebuilt from generic names: a chain of nested
// relative includes where each hop appends `../..` to the TEXTUAL path of its includer, ending in
// a run that climbs out of the module. Mixed separators are deliberate -- the `-I` roots arrive
// spelled with backslash and the `#include` bodies with forward slash, and a driver echoes both
// halves as it found them. The prefix is 60 bytes, so the whole is 299 and the collapse is 88:
// the figures measured on the build that prompted this.
constexpr std::string_view MeasuredPrefix = R"(D:\build-agent\workspace-00007\a1b2c3d4e5\3\example\project\)";

constexpr std::string_view MeasuredTail =
    R"(src\Graphics\Shared\Common\../../Sampling/View\../../Rendering/Raster/Core\../..)"
    R"(/Core\../../Geometry/Core\../../Shared/Core\../../Blending/Core\../../Shading/Pa)"
    R"(lette/Core\../../Sample/Common\../../../../Platform\../Graphics/GlyphRaster.hpp)";

// `cl` is not the only spelling of the prefix; a localized toolchain has its own.
constexpr std::string_view GermanMarker = "Hinweis: Einlesen der Datei:";

} // namespace

// ---------------------------------------------------------------------------
// CollapseRelativeSegments -- the path rule

TEST_CASE("The measured 299-byte note path collapses under Ninja's limit", "[note-path-collapse]")
{
    auto const input = std::string { MeasuredPrefix } + std::string { MeasuredTail };
    auto const collapsed = CollapseRelativeSegments(input);

    // Pinned as their own checks: a fixture that drifts under 260 stops testing the thing it
    // exists for, and would keep passing while doing it.
    CHECK(input.size() == 299);
    CHECK(collapsed.size() == 88);
    CHECK(collapsed == std::string { MeasuredPrefix } + R"(src\Graphics\GlyphRaster.hpp)");
}

TEST_CASE("A path with nothing to collapse comes back byte-identical", "[note-path-collapse]")
{
    // Mixed separators are the shape a driver really emits, and they must not be re-spelled just
    // because the path passed through here.
    constexpr std::string_view mixed = R"(D:\ci\src\inc/h1.h)";
    CHECK(CollapseRelativeSegments(mixed) == mixed);

    // `..` that is not a SEGMENT. A `contains("..")` test would take the slow path and hand back a
    // uniformly re-separated spelling for no reason at all.
    constexpr std::string_view dotsInName = R"(D:\ci\src\a..b\inc/h1.h)";
    CHECK(CollapseRelativeSegments(dotsInName) == dotsInName);
}

TEST_CASE("Dots inside a name do not block a real collapse elsewhere in the path", "[note-path-collapse]")
{
    // This is what pins `..` being tested as a SEGMENT rather than as a substring, and it is the
    // only shape that can: a path with nothing to collapse survives a substring test unharmed,
    // because the step-6 bail then returns the input and the output is right for the wrong reason.
    // Here the two shapes sit in one path, so a substring test bails on `a..b` and silently leaves
    // the genuine `tmp\..` uncollapsed.
    CHECK(CollapseRelativeSegments(R"(D:\ci\src\a..b\tmp\..\x.hpp)") == R"(D:\ci\src\a..b\x.hpp)");
}

TEST_CASE("A drive root cannot be ascended past on either host", "[note-path-collapse]")
{
    // This is the POSIX-host case. On Windows `lexically_normal` already refuses to walk above a
    // drive root, so a Windows-only run cannot see the anchor split being removed; on POSIX `D:` is
    // an ordinary filename and `D:/../x.hpp` would otherwise normalize to a bare `x.hpp`.
    CHECK(CollapseRelativeSegments(R"(D:\..\x.hpp)") == R"(D:\x.hpp)");
    CHECK(CollapseRelativeSegments(R"(D:\ci\..\x.hpp)") == R"(D:\x.hpp)");
}

TEST_CASE("A UNC root keeps both of its leading separators", "[note-path-collapse]")
{
    // `lexically_normal` collapses a leading `//` on POSIX and keeps it on Windows, so the prefix is
    // held aside rather than trusted to the pass.
    CHECK(CollapseRelativeSegments(R"(\\build\share\a\..\b\x.hpp)") == R"(\\build\share\b\x.hpp)");
}

TEST_CASE("A leading dot-dot that nothing lexical can resolve is left alone", "[note-path-collapse]")
{
    // Returning the rewrite here would re-spell the separators of a path whose `..` is still there,
    // which is a change with no benefit attached.
    //
    // The separators are deliberately MIXED. An all-backslash fixture cannot see this rule at all:
    // the rewrite would fold to `/`, fail to collapse anything, and then restore every separator to
    // `\`, arriving back at bytes identical to the input. The case would pass with the bail removed
    // and would be testing nothing.
    constexpr std::string_view relative = R"(..\../inc/a.hpp)";
    CHECK(CollapseRelativeSegments(relative) == relative);
}

TEST_CASE("A forward-slash path stays forward-slash", "[note-path-collapse]")
{
    CHECK(CollapseRelativeSegments("/home/dev/proj/src/a/../b/x.hpp") == "/home/dev/proj/src/b/x.hpp");
}

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

TEST_CASE("On this base an indented marker still reads as a note", "[note-path-collapse]")
{
    // Pins what this release line actually does rather than what it should do. Recognition here
    // skips leading blanks, so an indented line beginning with the marker is treated as a note and
    // its path is collapsed. #1270 narrows that back to a column-zero anchor -- `cl` pads AFTER the
    // marker, never before it -- but that lands in 0.3.0, and pulling it back here would change the
    // canonicalization spec and cold-start every cache on this line.
    //
    // The exposure is bounded on this seam: the collapse rewrites only what is EMITTED, never what
    // is keyed or stored, so a false positive here misprints a line and cannot mis-key a compile.
    auto const text = std::string { "  Note: including file: " } + R"(D:\ci\src\a\..\b\x.hpp)" + "\r\n";
    auto const expected = std::string { "  Note: including file: " } + R"(D:\ci\src\b\x.hpp)" + "\r\n";
    CHECK(CollapseNotePaths(text, Grammar::ShowIncludes, IncludeNoteMarker) == expected);
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

TEST_CASE("Two spellings of one header become two identical notes", "[note-path-collapse]")
{
    // Accepted rather than missed. RenderShowIncludes dedups byte-exactly and runs BEFORE this, so
    // two spellings survive as one entry each and collapse onto the same text. The record is
    // redundant, never untruthful, and the local path has always emitted un-deduped per-site
    // repeats anyway. Deduping after the collapse belongs with the ingest-side work.
    std::vector<std::string> const deps { R"(D:\s\a\..\b\x.h)", R"(D:\s\b\x.h)" };
    auto const rendered = RenderShowIncludes(deps, IncludeNoteMarker);
    auto const collapsed = CollapseNotePaths(rendered, Grammar::ShowIncludes, IncludeNoteMarker);

    auto const paths = ParseIncludePaths(collapsed);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == R"(D:\s\b\x.h)");
    CHECK(paths[1] == R"(D:\s\b\x.h)");
}
