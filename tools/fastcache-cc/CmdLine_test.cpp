// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache::Cc;

namespace
{
ParsedCommand Parse(std::vector<std::string> const& argv)
{
    return ParseCommand(std::span<std::string const> { argv });
}
} // namespace

TEST_CASE("ParseCommand extracts source/obj/flavor from a cl line with fused /Fo")
{
    auto const p = Parse({ "cl.exe", "/nologo", "/c", "/showIncludes", R"(/FoD:\b\x.obj)", R"(D:\s\x.cpp)" });
    CHECK(p.parsedOk);
    CHECK(p.flavor == Flavor::Cl);
    CHECK(p.source == R"(D:\s\x.cpp)");
    CHECK(p.objPath == R"(D:\b\x.obj)");
    CHECK(p.wantShowIncludes);
    CHECK(p.compiler == "cl.exe");
}

TEST_CASE("ParseCommand handles clang-cl and detects the flavor by basename")
{
    auto const p = Parse({ R"(C:\llvm\bin\clang-cl.exe)", "/c", R"(/Foout\y.obj)", R"(src\y.cpp)" });
    CHECK(p.parsedOk);
    CHECK(p.flavor == Flavor::ClangCl);
    CHECK(p.source == R"(src\y.cpp)");
    CHECK(p.objPath == R"(out\y.obj)");
    CHECK_FALSE(p.wantShowIncludes);
}

TEST_CASE("ParseCommand accepts a separate /Fo argument")
{
    auto const p = Parse({ "cl", "/c", "/Fo", R"(b\z.obj)", R"(z.cpp)" });
    CHECK(p.parsedOk);
    CHECK(p.objPath == R"(b\z.obj)");
    CHECK(p.source == "z.cpp");
}

TEST_CASE("ParseCommand accepts -o for the object output")
{
    auto const p = Parse({ "clang-cl", "-c", "-o", "obj/a.o", "a.cpp" });
    CHECK(p.parsedOk);
    CHECK(p.objPath == "obj/a.o");
    CHECK(p.source == "a.cpp");
}

TEST_CASE("ParseCommand marks a line with no source file as not cacheable")
{
    auto const p = Parse({ "cl.exe", "/nologo", "link", "/OUT:app.exe", "a.obj", "b.obj" });
    CHECK_FALSE(p.parsedOk);
}

TEST_CASE("ParseCommand recognises .cc and .cxx sources")
{
    CHECK(Parse({ "cl", "/c", "u.cc" }).source == "u.cc");
    CHECK(Parse({ "cl", "/c", "v.cxx" }).source == "v.cxx");
}
