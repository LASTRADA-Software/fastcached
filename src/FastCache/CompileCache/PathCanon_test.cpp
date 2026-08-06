// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PathCanon.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

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
