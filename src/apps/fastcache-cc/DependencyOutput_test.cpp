// SPDX-License-Identifier: Apache-2.0
#include "DependencyOutput.hpp"
#include "DependencyProbe.hpp"
#include "DirectManifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace FastCache::Cc;

TEST_CASE("A rendered depfile names the target the build asked for", "[dependency-output]")
{
    // Ninja compares the rule target against the -o path it passed and fails
    // outright when they differ ("expected depfile ... to mention ..."), while make
    // silently matches no rule and drops every dependency. So the target is spelled
    // exactly as given, never derived.
    std::vector<std::string> const deps { "inc/a.hpp" };
    auto const text = RenderDepFile("build/a.o", deps);
    CHECK(text.starts_with("build/a.o:"));
}

TEST_CASE("A rendered depfile round-trips through the launcher's own parser", "[dependency-output]")
{
    // The strongest available check: what this writes is read back by the same
    // function that reads a real compiler's depfile, so the two cannot drift into
    // agreeing with a format nothing else produces.
    std::vector<std::string> const deps { "inc/a.hpp", "inc/b.hpp", "/opt/sdk/c.hpp" };
    auto const text = RenderDepFile("build/a.o", deps);

    auto const parsed = ParseDepFilePaths(text);
    for (auto const& dep: deps)
    {
        INFO("dependency " << dep);
        CHECK(std::ranges::find(parsed, dep) != parsed.end());
    }
    // The rule target is excluded by the parser, as it excludes a real one's.
    CHECK(std::ranges::find(parsed, "build/a.o") == parsed.end());
}

TEST_CASE("A path containing a space is escaped for make", "[dependency-output]")
{
    // Unescaped it reads as TWO dependencies, and the second names a file that does
    // not exist -- which make and Ninja answer by rebuilding this translation unit
    // on every build, forever, with a zero exit code.
    std::vector<std::string> const deps { "/Program Files/sdk/a.hpp" };
    auto const text = RenderDepFile("a.o", deps);
    CHECK(text.contains("/Program\\ Files/sdk/a.hpp"));

    auto const parsed = ParseDepFilePaths(text);
    REQUIRE(parsed.size() == 1);
    CHECK(parsed.front() == "/Program Files/sdk/a.hpp");
}

TEST_CASE("A depfile with no dependencies is still well-formed", "[dependency-output]")
{
    // A translation unit including nothing is legal, and a truncated or empty file
    // would make the build system report a parse error rather than "no deps".
    auto const text = RenderDepFile("a.o", {});
    CHECK(text == "a.o:\n");
    CHECK(ParseDepFilePaths(text).empty());
}

TEST_CASE("Repeated dependencies are emitted once", "[dependency-output]")
{
    // /showIncludes names a header once per inclusion SITE, so the probe's raw
    // output is not a set.
    std::vector<std::string> const deps { "a.hpp", "b.hpp", "a.hpp", "a.hpp" };
    auto const parsed = ParseDepFilePaths(RenderDepFile("a.o", deps));
    CHECK(parsed.size() == 2);
}

namespace
{
/// A prefix `cl` prints under a German language pack, and the value CMake would
/// then record as `msvc_deps_prefix`.
///
/// Any non-English string would exercise the same code. This one is spelled out
/// because the reader has to be able to see that it shares no prefix with
/// `IncludeNoteMarker` -- that is what makes the negative cases below mean
/// something, and a placeholder like "XX:" would leave it to be taken on trust.
constexpr std::string_view LocalizedMarker = "Hinweis: Einlesen der Datei:";
} // namespace

TEST_CASE("Rendered showIncludes notes are recognised by the launcher's own reader", "[dependency-output]")
{
    // Same round-trip argument as the depfile: the recognition rule comes from
    // DirectManifest, which is where the reading side gets it. This is the DEFAULT
    // marker's case -- the English toolchain, where writer and reader agree.
    std::vector<std::string> const deps { R"(C:\src\inc\a.hpp)", R"(C:\src\inc\b.hpp)" };
    auto const text = RenderShowIncludes(deps, IncludeNoteMarker);

    auto const parsed = ParseIncludePaths(text);
    for (auto const& dep: deps)
    {
        INFO("dependency " << dep);
        CHECK(std::ranges::find(parsed, dep) != parsed.end());
    }
}

TEST_CASE("showIncludes notes carry the marker the CALLER named, not the reader's", "[dependency-output]")
{
    // The heart of #700. The old assertion here was
    //
    //     CHECK(RenderShowIncludes(deps).starts_with(IncludeNoteMarker));
    //
    // which is true of the healthy build AND of the broken one -- the writer hard-
    // coding the reader's English literal is exactly what it certified. So it could
    // not fail for the reason it existed, which is what this repository means by
    // asserting what both sides produce.
    //
    // What distinguishes them is whether a localized prefix reaches the output at
    // all, so that is what is asserted, in both directions.
    std::vector<std::string> const deps { R"(C:\src\inc\a.hpp)" };

    auto const localized = RenderShowIncludes(deps, LocalizedMarker);
    CHECK(localized.starts_with(LocalizedMarker));
    CHECK(localized.contains(R"(C:\src\inc\a.hpp)"));
    // ...and the English literal is nowhere in it. Without this the case would pass
    // against a writer that emitted both, or that appended the caller's marker to
    // its own -- neither of which Ninja could match either.
    CHECK_FALSE(localized.contains(IncludeNoteMarker));

    // The control that makes the above mean something: the same call with the
    // default marker still produces the English form. A writer that ignored its
    // argument entirely would fail the localized case; one that mangled every
    // marker would fail this one.
    CHECK(RenderShowIncludes(deps, IncludeNoteMarker).starts_with(IncludeNoteMarker));
}

TEST_CASE("A localized note is invisible to the English reader, which IS the defect", "[dependency-output]")
{
    // Ninja's position, in the launcher's own vocabulary. `ParseIncludePaths`
    // implements the same rule Ninja does -- match the prefix literally, take what
    // follows -- so a reader configured for one language extracting NOTHING from
    // notes written in another is the under-rebuild, stated as an assertion rather
    // than as a comment.
    //
    // It is also why the fix cannot be "consolidate the two spellings": the writer's
    // question (what will Ninja match?) and the reader's (what did the probe print?)
    // have different answers on the same machine.
    std::vector<std::string> const deps { R"(C:\src\inc\a.hpp)", R"(C:\src\inc\b.hpp)" };

    CHECK(ParseIncludePaths(RenderShowIncludes(deps, LocalizedMarker)).empty());
    // The control. Same paths, same reader, marker the reader knows -- two entries.
    // Without it, a `ParseIncludePaths` that had simply stopped working would pass
    // the assertion above.
    CHECK(ParseIncludePaths(RenderShowIncludes(deps, IncludeNoteMarker)).size() == deps.size());
}

TEST_CASE("An empty dependency set renders no notes at all", "[dependency-output]")
{
    // Not a blank line, not a header -- nothing. A stray line would be replayed
    // onto the compiler's real stdout and could be parsed as a note with an empty
    // path.
    //
    // Asked of the default marker only. The same call with `LocalizedMarker` looks
    // like a second case and is not one: with an empty span the loop body never runs,
    // so the marker is unreachable by construction and the assertion holds for any
    // value whatsoever. That is a property of the signature rather than of a code
    // path, which is the shape this repository's testing rule warns about.
    CHECK(RenderShowIncludes({}, IncludeNoteMarker).empty());
}
