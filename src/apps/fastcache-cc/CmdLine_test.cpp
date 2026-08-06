// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// --- GNU drivers ------------------------------------------------------------

TEST_CASE("ParseCommand classifies every supported driver by basename")
{
    struct Case
    {
        std::string compiler;
        Flavor expected;
    };
    // One row per driver spelling we promise to support, including the
    // version-suffixed and .exe forms real build systems produce.
    auto const cases = std::vector<Case> {
        { .compiler = "cl.exe", .expected = Flavor::Cl },      { .compiler = "clang-cl.exe", .expected = Flavor::ClangCl },
        { .compiler = "gcc", .expected = Flavor::Gcc },        { .compiler = "g++", .expected = Flavor::Gcc },
        { .compiler = "g++-14", .expected = Flavor::Gcc },     { .compiler = "/usr/bin/g++", .expected = Flavor::Gcc },
        { .compiler = "cc", .expected = Flavor::Gcc },         { .compiler = "c++", .expected = Flavor::Gcc },
        { .compiler = "clang", .expected = Flavor::Clang },    { .compiler = "clang++", .expected = Flavor::Clang },
        { .compiler = "clang-18", .expected = Flavor::Clang }, { .compiler = "rustc", .expected = Flavor::Unknown },
    };

    for (auto const& c: cases)
    {
        INFO("compiler: " << c.compiler);
        CHECK(Parse({ c.compiler, "-c", "a.cpp", "-o", "a.o" }).flavor == c.expected);
    }
}

TEST_CASE("ParseCommand extracts source and object from a gcc line")
{
    auto const p = Parse({ "g++", "-std=c++23", "-c", "src/a.cpp", "-o", "build/a.o" });
    CHECK(p.parsedOk);
    CHECK(p.flavor == Flavor::Gcc);
    CHECK(p.source == "src/a.cpp");
    CHECK(p.objPath == "build/a.o");
}

TEST_CASE("ParseCommand captures the depfile path from -MF, joined or separate")
{
    CHECK(Parse({ "g++", "-c", "a.cpp", "-o", "a.o", "-MD", "-MF", "dep/a.d" }).depPath == "dep/a.d");
    CHECK(Parse({ "clang++", "-c", "a.cpp", "-o", "a.o", "-MD", "-MFdep/a.d" }).depPath == "dep/a.d");
}

TEST_CASE("ParseCommand does not treat an absolute path as an option for GNU drivers")
{
    // A GNU driver only introduces options with '-', so /usr/src/a.cpp is a
    // source path — under an MSVC driver the same token would be an option.
    auto const p = Parse({ "gcc", "-c", "/usr/src/a.cpp", "-o", "a.o" });
    CHECK(p.parsedOk);
    CHECK(p.source == "/usr/src/a.cpp");
}

TEST_CASE("ParseCommand rejects lines that do not compile exactly one TU to an object")
{
    // Link step: no -c.
    CHECK_FALSE(Parse({ "g++", "a.o", "b.o", "-o", "app" }).parsedOk);
    // Compile-and-link in one step: a source, but still no -c.
    CHECK_FALSE(Parse({ "g++", "a.cpp", "-o", "app" }).parsedOk);
    // Preprocess-only: produces text, not an object.
    CHECK_FALSE(Parse({ "g++", "-E", "a.cpp" }).parsedOk);
    // Two translation units on one line.
    CHECK_FALSE(Parse({ "g++", "-c", "a.cpp", "b.cpp" }).parsedOk);
    // Unknown driver.
    CHECK_FALSE(Parse({ "rustc", "-c", "a.cpp", "-o", "a.o" }).parsedOk);
}

// --- preprocess command construction ---------------------------------------

TEST_CASE("PreprocessCommand asks a GNU driver for preprocessed text on stdout")
{
    std::vector<std::string> const argv { "g++", "-std=c++23", "-c", "a.cpp", "-o", "build/a.o" };
    auto const pp = PreprocessCommand(Parse(argv), argv);

    CHECK(pp.front() == "g++");
    CHECK(std::ranges::contains(pp, "-E"));
    CHECK(std::ranges::contains(pp, "-std=c++23")); // real flags are preserved
    // The compile action and its object output must be gone, or the probe
    // would write an object instead of text on stdout.
    CHECK_FALSE(std::ranges::contains(pp, "-c"));
    CHECK_FALSE(std::ranges::contains(pp, "-o"));
    CHECK_FALSE(std::ranges::contains(pp, "build/a.o"));
}

TEST_CASE("PreprocessCommand suppresses line markers on every driver")
{
    // Regression guard. `# 1 "/abs/path/t.cpp"` line markers embed the absolute
    // source path, so preprocessed text from two checkouts of identical content
    // at different paths would differ — and the cache keys with it, silently
    // destroying cross-machine sharing, which is this launcher's whole point.
    // MSVC's /EP suppresses them; GNU needs an explicit -P.
    std::vector<std::string> const gnu { "g++", "-c", "a.cpp", "-o", "a.o" };
    CHECK(std::ranges::contains(PreprocessCommand(Parse(gnu), gnu), "-P"));

    std::vector<std::string> const msvc { "cl.exe", "/c", "a.cpp" };
    CHECK(std::ranges::contains(PreprocessCommand(Parse(msvc), msvc), "/EP"));
}

TEST_CASE("PreprocessCommand drops dependency flags together with their values")
{
    // -MF's value must go with it: a stray "dep/a.d" left on the line would be
    // read as a second input file and break the probe.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-o", "a.o", "-MD", "-MF", "dep/a.d" };
    auto const pp = PreprocessCommand(Parse(argv), argv);

    CHECK_FALSE(std::ranges::contains(pp, "-MD"));
    CHECK_FALSE(std::ranges::contains(pp, "-MF"));
    CHECK_FALSE(std::ranges::contains(pp, "dep/a.d"));
    CHECK(std::ranges::contains(pp, "a.cpp"));
}

TEST_CASE("PreprocessCommand uses the MSVC spelling for MSVC drivers")
{
    std::vector<std::string> const argv { "cl.exe", "/c", "/showIncludes", "/Foout.obj", "a.cpp" };
    auto const pp = PreprocessCommand(Parse(argv), argv);

    CHECK(std::ranges::contains(pp, "/EP"));
    CHECK_FALSE(std::ranges::contains(pp, "/c"));
    CHECK_FALSE(std::ranges::contains(pp, "/showIncludes"));
    CHECK_FALSE(std::ranges::contains(pp, "/Foout.obj"));
}

TEST_CASE("DriverOf reports where each driver reports its dependencies")
{
    // cl prints /showIncludes notes on stderr, clang-cl on stdout; the GNU
    // drivers do not print them at all and use a depfile instead.
    CHECK(DriverOf(Flavor::Cl).includeStream == IncludeStream::Stderr);
    CHECK(DriverOf(Flavor::ClangCl).includeStream == IncludeStream::Stdout);
    CHECK(DriverOf(Flavor::Gcc).includeStream == IncludeStream::None);
    CHECK(DriverOf(Flavor::Gcc).usesDepfile);
    CHECK(DriverOf(Flavor::Clang).usesDepfile);
    CHECK_FALSE(DriverOf(Flavor::Cl).usesDepfile);
    CHECK(DriverOf(Flavor::Unknown).flavor == Flavor::Unknown);
}
