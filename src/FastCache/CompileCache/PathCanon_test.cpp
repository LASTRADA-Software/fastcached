// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PathCanon.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace FastCache;
using PathCanon::Grammar;
using PathCanon::Layout;

// ---------------------------------------------------------------------------
// Task 1: single-path canonicalize/localize round-trip (canon spec v1 vectors).
// ---------------------------------------------------------------------------

TEST_CASE("Canonicalize rewrites a source-root path to a SRCROOT token")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\runner\src)", .buildTree = R"(C:\ci\deep\runner\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\ci\deep\runner\src\include\foo.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == "<SRCROOT>/include/foo.h");
}

TEST_CASE("Canonicalize prefers the longest (build-tree) root when nested under source root")
{
    Layout const layout { .sourceRoot = R"(C:\src)", .buildTree = R"(C:\src\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\src\build\gen\config.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == "<BUILDTREE>/gen/config.h");
}

TEST_CASE("Localize is the inverse of Canonicalize under a different layout")
{
    Layout const producer { .sourceRoot = R"(C:\ci\deep\runner\src)", .buildTree = R"(C:\ci\deep\runner\build)" };
    Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\ci\deep\runner\src\include\foo.h)", producer);
    REQUIRE(token.has_value());
    auto const local = PathCanon::Localize(*token, consumer);
    REQUIRE(local.has_value());
    CHECK(*local == R"(D:\project\include\foo.h)");
}

TEST_CASE("A path under neither root round-trips verbatim")
{
    Layout const layout { .sourceRoot = R"(C:\src)", .buildTree = R"(C:\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\Windows\Kits\10\um\windows.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == R"(C:\Windows\Kits\10\um\windows.h)");
    auto const local = PathCanon::Localize(*token, layout);
    REQUIRE(local.has_value());
    CHECK(*local == R"(C:\Windows\Kits\10\um\windows.h)");
}

TEST_CASE("Root match is on a segment boundary, not a bare prefix")
{
    // sourceRoot C:\srclib must NOT match a path under C:\src\...
    Layout const layout { .sourceRoot = R"(C:\srclib)", .buildTree = R"(C:\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\src\other\x.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == R"(C:\src\other\x.h)");
}

TEST_CASE("Matching is case-insensitive on Windows drive and path")
{
    Layout const layout { .sourceRoot = R"(C:\Ci\Deep\Src)", .buildTree = R"(C:\Ci\Deep\Build)" };
    auto const token = PathCanon::Canonicalize(R"(c:\ci\deep\src\A.H)", layout);
    REQUIRE(token.has_value());
    // Tail preserves the ORIGINAL bytes' case; only separators normalize.
    CHECK(*token == "<SRCROOT>/A.H");
}

// ---------------------------------------------------------------------------
// Task 2: region grammar (showIncludes / diagnostics / depfile).
// ---------------------------------------------------------------------------

TEST_CASE("CanonicalizeRegion rewrites only the path in a /showIncludes line")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    std::string const in = "Note: including file: "
                           R"(C:\ci\deep\src\include\foo.h)"
                           "\r\n";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, layout);
    REQUIRE(out.has_value());
    CHECK(*out == "Note: including file: <SRCROOT>/include/foo.h\r\n");
}

TEST_CASE("LocalizeRegion round-trips a showIncludes block across layouts")
{
    Layout const producer { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    std::string const in = "Note: including file:  "
                           R"(C:\ci\deep\src\a.h)"
                           "\r\n"
                           "Note: including file: "
                           R"(C:\ci\deep\src\b.h)"
                           "\r\n";
    auto const canon = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, producer);
    REQUIRE(canon.has_value());
    auto const local = PathCanon::LocalizeRegion(*canon, Grammar::ShowIncludes, consumer);
    REQUIRE(local.has_value());
    CHECK(local->contains(R"(D:\project\a.h)"));
    CHECK(local->contains(R"(D:\project\b.h)"));
    CHECK_FALSE(local->contains(R"(ci\deep)"));
}

TEST_CASE("A non-path showIncludes line is preserved verbatim")
{
    Layout const layout { .sourceRoot = R"(C:\src)", .buildTree = R"(C:\build)" };
    std::string const in = "some unrelated compiler note\r\n";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, layout);
    REQUIRE(out.has_value());
    CHECK(*out == in);
}

TEST_CASE("CanonicalizeRegion leaves a final line without newline intact")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    // no trailing CRLF
    std::string const in = "Note: including file: "
                           R"(C:\ci\deep\src\z.h)";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, layout);
    REQUIRE(out.has_value());
    CHECK(*out == "Note: including file: <SRCROOT>/z.h");
}

TEST_CASE("MsvcDiagnostics rewrites the leading path of a diagnostic line")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    std::string const in = R"(C:\ci\deep\src\a.cpp(42): warning C4100: unreferenced)"
                           "\r\n";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::MsvcDiagnostics, layout);
    REQUIRE(out.has_value());
    CHECK(*out == "<SRCROOT>/a.cpp(42): warning C4100: unreferenced\r\n");
}

// ---------------------------------------------------------------------------
// GccDepfile: the multi-token grammar. A depfile line names a target AND a whole
// dependency list, so every token on it is a path that must travel.
// ---------------------------------------------------------------------------

TEST_CASE("GccDepfile canonicalizes both the target and every dependency")
{
    Layout const layout { .sourceRoot = "/home/ci/src", .buildTree = "/home/ci/build" };
    constexpr std::string_view depFile = "/home/ci/build/a.o: /home/ci/src/a.cpp /home/ci/src/a.hpp\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    // Every checkout-rooted path becomes a token; nothing machine-specific left.
    CHECK_FALSE(canonical->contains("/home/ci"));
    CHECK(canonical->contains("<BUILDTREE>"));
    CHECK(canonical->contains("<SRCROOT>"));
    // The rule syntax itself is untouched.
    CHECK(canonical->contains(": "));
    CHECK(canonical->ends_with("\n"));
}

TEST_CASE("GccDepfile round-trips a depfile into a different checkout")
{
    // The property the cache depends on: a depfile stored from one checkout must
    // come back naming the CONSUMER's paths. Replaying it verbatim would point
    // the build system at another tree — or at nothing at all.
    Layout const producer { .sourceRoot = "/build/deep/a/b/c/src", .buildTree = "/build/deep/a/b/c/out" };
    Layout const consumer { .sourceRoot = "/s", .buildTree = "/s/out" };

    constexpr std::string_view depFile = "/build/deep/a/b/c/out/t.o: /build/deep/a/b/c/src/t.cpp \\\n"
                                         "  /usr/include/stdc-predef.h \\\n"
                                         "  /build/deep/a/b/c/src/hdr.hpp\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, producer);
    REQUIRE(canonical.has_value());
    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, consumer);
    REQUIRE(localized.has_value());

    CHECK(localized->contains("/s/out/t.o"));
    CHECK(localized->contains("/s/t.cpp"));
    CHECK(localized->contains("/s/hdr.hpp"));
    // The producer's layout must be entirely gone.
    CHECK_FALSE(localized->contains("/build/deep"));
    // A path under neither root (a system header) is not ours to rewrite.
    CHECK(localized->contains("/usr/include/stdc-predef.h"));
    // Continuations survive: a depfile whose line structure changed could stop
    // parsing as a rule at all.
    CHECK(localized->contains("\\\n"));
}

TEST_CASE("GccDepfile preserves escaped spaces inside a path")
{
    // A checkout under a directory with a space in its name is ordinary; make
    // escapes the space, and losing the escape splits one path into two.
    Layout const layout { .sourceRoot = "/home/My Code", .buildTree = "/home/My Build" };
    constexpr std::string_view depFile = R"(/home/My\ Build/a.o: /home/My\ Code/a.cpp)"
                                         "\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, layout);
    REQUIRE(localized.has_value());
    // Byte-identical round-trip through the same layout.
    CHECK(*localized == depFile);
}

TEST_CASE("GccDepfile keeps a Windows drive letter out of the rule separator")
{
    // "D:" must not read as the target/dependency separator, or every absolute
    // Windows path in the file would be cut down to its drive letter.
    Layout const layout { .sourceRoot = R"(D:\src)", .buildTree = R"(D:\build)" };
    constexpr std::string_view depFile = R"(D:\build\a.obj: D:\src\a.cpp D:\src\a.hpp)"
                                         "\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    CHECK(canonical->contains("<BUILDTREE>"));
    CHECK(canonical->contains("<SRCROOT>"));

    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, layout);
    REQUIRE(localized.has_value());
    CHECK(*localized == depFile);
}

TEST_CASE("GccDepfile leaves a depfile with no in-tree paths untouched")
{
    Layout const layout { .sourceRoot = "/home/ci/src", .buildTree = "/home/ci/build" };
    constexpr std::string_view depFile = "/other/a.o: /other/a.cpp /usr/include/stdio.h\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    CHECK(*canonical == depFile);
}

TEST_CASE("GccDepfile preserves the phony rules -MP emits")
{
    Layout const layout { .sourceRoot = "/s", .buildTree = "/b" };
    constexpr std::string_view depFile = "/b/a.o: /s/a.cpp /s/a.hpp\n"
                                         "\n"
                                         "/s/a.hpp:\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, layout);
    REQUIRE(localized.has_value());
    CHECK(*localized == depFile);
}
