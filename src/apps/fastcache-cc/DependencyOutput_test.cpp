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

TEST_CASE("Rendered showIncludes notes are recognised by the launcher's own reader", "[dependency-output]")
{
    // Same round-trip argument as the depfile: the marker and the recognition rule
    // both come from DirectManifest, which is where the reading side gets them.
    std::vector<std::string> const deps { R"(C:\src\inc\a.hpp)", R"(C:\src\inc\b.hpp)" };
    auto const text = RenderShowIncludes(deps);

    auto const parsed = ParseIncludePaths(text);
    for (auto const& dep: deps)
    {
        INFO("dependency " << dep);
        CHECK(std::ranges::find(parsed, dep) != parsed.end());
    }
}

TEST_CASE("showIncludes notes use the marker the reader looks for", "[dependency-output]")
{
    std::vector<std::string> const deps { "a.hpp" };
    CHECK(RenderShowIncludes(deps).starts_with(IncludeNoteMarker));
}

TEST_CASE("An empty dependency set renders no notes at all", "[dependency-output]")
{
    // Not a blank line, not a header -- nothing. A stray line would be replayed
    // onto the compiler's real stdout and could be parsed as a note with an empty
    // path.
    CHECK(RenderShowIncludes({}).empty());
}
