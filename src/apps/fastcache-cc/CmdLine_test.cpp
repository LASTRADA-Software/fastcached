// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"

#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;

namespace
{

/// The same, for the `expected` that carries a refusal's reason.
template <typename T, typename E>
[[nodiscard]] T Unwrap(std::expected<T, E> const& value)
{
    return value.value_or(T {});
}

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

TEST_CASE("PreprocessCommand drops the GNU spelling of compile-only on clang-cl")
{
    // Regression guard. clang-cl accepts BOTH `/c` and `-c` for compile-only --
    // it is a GNU-compatible driver front-ending an MSVC-style one, and CMake's
    // Ninja generator spells it `-c` for CMAKE_CXX_COMPILER=clang-cl. MsvcDrop
    // only named "/c", so a `-c` build's own compile-only marker survived onto
    // the /EP probe line, clang-cl reported it as an unused argument under /EP
    // (-Werror,-Wunused-command-line-argument), and a build with /WX turned that
    // into a hard preprocess failure -- the cache silently unreachable for every
    // translation unit, with the build itself unaffected because the launcher
    // falls back to the real compiler on any cache error.
    std::vector<std::string> const argv { R"(C:\llvm\bin\clang-cl.exe)", "-c", "/showIncludes", "/Foout.obj", "a.cpp" };
    auto const pp = PreprocessCommand(Parse(argv), argv);

    CHECK(std::ranges::contains(pp, "/EP"));
    CHECK_FALSE(std::ranges::contains(pp, "-c"));
    CHECK_FALSE(std::ranges::contains(pp, "/showIncludes"));
    CHECK_FALSE(std::ranges::contains(pp, "/Foout.obj"));
}

TEST_CASE("PreprocessCommand does not drop a flag that merely starts like a dropped one")
{
    // Regression guard. Matching drop flags by bare prefix made `-c` swallow
    // `-coverage` and `-cxx-isystem`, and `/c` swallow `/clr` — removing a real
    // flag from the probe line and stranding its separated value as a second
    // input file. The probe then fails and the TU compiles UNCACHED forever,
    // while every other unit test still passes. Silent hit-rate collapse.
    struct Case
    {
        std::vector<std::string> argv;
        std::string mustKeep; ///< Flag that must survive onto the probe line.
        std::string alsoKeep; ///< Its value, when given separately.
    };
    auto const cases = std::vector<Case> {
        { .argv = { "g++", "-c", "-cxx-isystem", "/inc", "a.cpp", "-o", "a.o" },
          .mustKeep = "-cxx-isystem",
          .alsoKeep = "/inc" },
        { .argv = { "g++", "-c", "-coverage", "a.cpp", "-o", "a.o" }, .mustKeep = "-coverage", .alsoKeep = "" },
        { .argv = { "clang++", "-c", "-current_version", "2", "a.cpp", "-o", "a.o" },
          .mustKeep = "-current_version",
          .alsoKeep = "2" },
        // MSVC: /clr, /constexpr:steps and /cgthreads all begin with "/c".
        { .argv = { "cl.exe", "/c", "/clr", "a.cpp", "/Foa.obj" }, .mustKeep = "/clr", .alsoKeep = "" },
        { .argv = { "cl.exe", "/c", "/constexpr:steps1000", "a.cpp", "/Foa.obj" },
          .mustKeep = "/constexpr:steps1000",
          .alsoKeep = "" },
        { .argv = { "cl.exe", "/c", "/cgthreads4", "a.cpp", "/Foa.obj" }, .mustKeep = "/cgthreads4", .alsoKeep = "" },
        // -MP / -MMD start like -M-family drop flags but are distinct flags.
        { .argv = { "g++", "-c", "-MMD", "a.cpp", "-o", "a.o" }, .mustKeep = "", .alsoKeep = "" },
    };

    for (auto const& c: cases)
    {
        INFO("flag: " << c.mustKeep);
        auto const pp = PreprocessCommand(Parse(c.argv), c.argv);
        if (!c.mustKeep.empty())
            CHECK(std::ranges::contains(pp, c.mustKeep));
        if (!c.alsoKeep.empty())
            CHECK(std::ranges::contains(pp, c.alsoKeep));
        // The source must always survive, and be the only non-flag input.
        CHECK(std::ranges::contains(pp, "a.cpp"));
    }
}

TEST_CASE("PreprocessCommand still drops the joined forms of real dropped flags")
{
    // The fix must not overshoot: a genuinely joined value (`-MFdep.d`,
    // `/Foout.obj`) is still the dropped flag and must go, value and all.
    std::vector<std::string> const gnu { "g++", "-c", "a.cpp", "-oout/a.o", "-MD", "-MFdep/a.d" };
    auto const gnuPp = PreprocessCommand(Parse(gnu), gnu);
    CHECK_FALSE(std::ranges::contains(gnuPp, "-oout/a.o"));
    CHECK_FALSE(std::ranges::contains(gnuPp, "-MFdep/a.d"));
    CHECK(std::ranges::contains(gnuPp, "a.cpp"));

    std::vector<std::string> const msvc { "cl.exe", "/c", "/Foout.obj", "a.cpp" };
    CHECK_FALSE(std::ranges::contains(PreprocessCommand(Parse(msvc), msvc), "/Foout.obj"));
}

TEST_CASE("PreprocessCommand consumes a separated value only for the bare flag")
{
    // A joined form carries its own value, so consuming the NEXT argument too
    // would eat a real flag — here the -I that follows.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-MFdep.d", "-Iinclude", "-o", "a.o" };
    auto const pp = PreprocessCommand(Parse(argv), argv);
    CHECK(std::ranges::contains(pp, "-Iinclude"));
    CHECK_FALSE(std::ranges::contains(pp, "-MFdep.d"));
}

TEST_CASE("ParseCommand accepts a separator-joined value")
{
    CHECK(Parse({ "g++", "-c", "a.cpp", "-MF=dep/a.d", "-o", "a.o" }).depPath == "dep/a.d");
}

TEST_CASE("ParseCommand refuses a compile with no explicit object output")
{
    // Regression guard. `g++ -c a.cpp` defaults its output to ./a.o, a path the
    // launcher does not reconstruct. Treating it as cacheable made every such
    // compile report a MISS and then fail to store — permanently, on every
    // invocation — and would hand an empty path to the object writer on a hit.
    auto const p = Parse({ "g++", "-c", "a.cpp" });
    CHECK(p.source == "a.cpp");
    CHECK(p.objPath.empty());
    CHECK_FALSE(p.parsedOk);

    // The MSVC spelling of the same thing.
    CHECK_FALSE(Parse({ "cl.exe", "/c", "a.cpp" }).parsedOk);
    // ...while the explicit forms stay cacheable.
    CHECK(Parse({ "g++", "-c", "a.cpp", "-o", "a.o" }).parsedOk);
    CHECK(Parse({ "cl.exe", "/c", "a.cpp", "/Foa.obj" }).parsedOk);
}

TEST_CASE("DriverOf reports how each driver reports its dependencies")
{
    // The GNU drivers write a depfile; the MSVC ones print notes inline. WHICH
    // stream an inline reporter uses is deliberately not in the table: it differs
    // between a compile run and a preprocess-only run, so both readers take both
    // streams rather than choosing (see DriverSpec::usesDepfile).
    CHECK(DriverOf(Flavor::Gcc).usesDepfile);
    CHECK(DriverOf(Flavor::Clang).usesDepfile);
    CHECK_FALSE(DriverOf(Flavor::Cl).usesDepfile);
    CHECK(DriverOf(Flavor::Unknown).flavor == Flavor::Unknown);
}

TEST_CASE("DriverOf asks MSVC for its version the only way MSVC answers")
{
    // `cl` has no `--version`. Handed one it prints its banner, warns D9002,
    // errors D8003 and exits 2 -- and CompilerBanner requires a zero exit, so
    // every MSVC compiler fell through to the string `cl` and one toolset's object
    // was served to another toolset's compile (issue #195). An EMPTY row is what
    // fixes it: bare `cl` prints the same banner and exits 0.
    //
    // Asserted as `.empty()` rather than by comparing spans, because the fact under
    // test is that MSVC is asked with NO flags at all -- a row that grew a flag
    // would put every `cl` back on the fallback, silently.
    CHECK(DriverOf(Flavor::Cl).versionFlags.empty());

    // Every other driver, `clang-cl` included: it is clang's own option, not a
    // GNU-family one, and clang-cl exits 0 from it.
    for (auto const flavor: { Flavor::ClangCl, Flavor::Gcc, Flavor::Clang, Flavor::Unknown })
    {
        auto const flags = DriverOf(flavor).versionFlags;
        REQUIRE(flags.size() == 1);
        CHECK(flags.front() == "--version");
    }
}

TEST_CASE("PreprocessCommand asks each driver for its dependencies in one spelling")
{
    // The cache key must be a function of BOTH artefacts a hit reproduces. The
    // dependency set is captured on the preprocess run the launcher already makes
    // — measured at +1.5% on a 45 ms preprocess — rather than in a second probe.
    //
    // The build's own spelling is dropped and ours appended in its place, so the
    // set does not depend on whether the compile asked for `-MD`, `-MMD`, or for
    // no dependencies at all. There must be exactly one of each flag on the line:
    // a surviving `-MF` would send the probe's dependencies to the build's real
    // depfile, which a hit that is then discarded must not have touched.
    std::vector<std::string> const gnu { "g++", "-c", "a.cpp", "-o", "a.o", "-MMD", "-MF", "dep/a.d" };
    auto const gnuProbe = PreprocessCommand(Parse(gnu), gnu, "build/a.o.fcdep");

    CHECK(std::ranges::count(gnuProbe, "-MD") == 1);
    CHECK(std::ranges::count(gnuProbe, "-MF") == 1);
    CHECK_FALSE(std::ranges::contains(gnuProbe, "-MMD"));
    CHECK_FALSE(std::ranges::contains(gnuProbe, "dep/a.d"));
    // The destination must FOLLOW its flag, or the driver reads the next argument
    // as the depfile and the probe path as a second input file.
    auto const flag = std::ranges::find(gnuProbe, "-MF");
    REQUIRE(flag != gnuProbe.end());
    REQUIRE(std::next(flag) != gnuProbe.end());
    CHECK(*std::next(flag) == "build/a.o.fcdep");

    // MSVC drivers report on a stream instead, so they take the request but no
    // path — /showIncludes with a trailing path would be a stray input file.
    std::vector<std::string> const msvc { "cl.exe", "/c", "/showIncludes", "/Foout.obj", "a.cpp" };
    auto const msvcProbe = PreprocessCommand(Parse(msvc), msvc, "out.obj.fcdep");

    CHECK(std::ranges::count(msvcProbe, "/showIncludes") == 1);
    CHECK_FALSE(std::ranges::contains(msvcProbe, "out.obj.fcdep"));
    CHECK_FALSE(std::ranges::contains(msvcProbe, "-MF"));
}

TEST_CASE("PreprocessCommand omits the dependency probe when no path is given")
{
    // The path is what requests the probe. Without one the line is exactly what
    // it was before dependency capture existed, which is what lets the callers
    // that only want text (and every older test above) keep asking for it.
    std::vector<std::string> const gnu { "g++", "-c", "a.cpp", "-o", "a.o" };
    auto const pp = PreprocessCommand(Parse(gnu), gnu);

    CHECK_FALSE(std::ranges::contains(pp, "-MD"));
    CHECK_FALSE(std::ranges::contains(pp, "-MF"));

    std::vector<std::string> const msvc { "cl.exe", "/c", "/Foout.obj", "a.cpp" };
    CHECK_FALSE(std::ranges::contains(PreprocessCommand(Parse(msvc), msvc), "/showIncludes"));
}

TEST_CASE("PreprocessCommand sends MSVC preprocessed text to stdout, never to a file")
{
    // Regression guard, and the sharpest kind: a probe line carrying BOTH /EP and
    // /P is accepted by the compiler, exits 0, and writes the preprocessed text to
    // `<base>.i` — so the launcher hashed an empty stdout and every Windows key
    // carried no content from the source at all. An edited file then re-fetched the
    // object built from the old text. Direct mode masked it (its manifest hashes
    // the source's own bytes), so nothing failed until FASTCACHE_NO_DIRECT=1.
    //
    // /EP and /P are alternatives, not modifiers: /EP preprocesses to stdout, /P to
    // a file, and MSVC documents the pair as "to the file, without #line".
    for (auto const& compiler: { "cl.exe", "clang-cl.exe" })
    {
        std::vector<std::string> const argv { compiler, "/c", "/Foout.obj", "a.cpp" };
        auto const pp = PreprocessCommand(Parse(argv), argv);

        CHECK(std::ranges::contains(pp, "/EP"));
        CHECK_FALSE(std::ranges::contains(pp, "/P"));
    }
}

// --- the one path-valued flag table ------------------------------------------

TEST_CASE("A path-valued flag's family is honoured, so /MT's dash spelling is not a depfile target")
{
    // The reason a flag row carries a driver family rather than being matched by
    // its introducer alone. `-MT` names a dependency target for a GNU driver and
    // selects the static multithreaded runtime for an MSVC one, and MSVC drivers
    // accept both introducers — so a row matched on `-` would make `cl -MT`
    // consume the next argument. Here that argument is the source file, and the
    // line would silently stop being cacheable.
    auto const msvc = Parse({ "cl.exe", "/c", "-MT", "a.cpp", "/Foa.obj" });
    CHECK(msvc.source == "a.cpp");
    CHECK(msvc.objPath == "a.obj");
    CHECK(msvc.parsedOk);
    CHECK(msvc.depPath.empty());

    // ...while the GNU driver, whose family the row does belong to, still reads it.
    CHECK(Parse({ "g++", "-c", "a.cpp", "-o", "a.o", "-MF", "dep/a.d" }).depPath == "dep/a.d");
}

TEST_CASE("An MSVC driver reads the object output in every spelling it accepts")
{
    // `/Fo` and `-o` were two hand-written branches, and `-Fo` — which cl and
    // clang-cl both accept — was neither, so such a line parsed with no object
    // path and fell back to an uncached compile. All three are rows of one table
    // now, matched joined or separated by the same code.
    CHECK(Parse({ "cl.exe", "/c", R"(/Fob\a.obj)", "a.cpp" }).objPath == R"(b\a.obj)");
    CHECK(Parse({ "cl.exe", "/c", R"(-Fob\a.obj)", "a.cpp" }).objPath == R"(b\a.obj)");
    CHECK(Parse({ "cl.exe", "/c", "-o", R"(b\a.obj)", "a.cpp" }).objPath == R"(b\a.obj)");
    CHECK(Parse({ "clang-cl", "/c", R"(-Fo)", R"(b\a.obj)", "a.cpp" }).objPath == R"(b\a.obj)");
}

TEST_CASE("PreprocessCommand drops the object output in every spelling, from the shared table")
{
    // The drop list no longer spells `/Fo` or `-MF` itself; it drops every
    // path-valued flag whose role has no business on a preprocess line. So a
    // spelling added to that table is dropped by construction rather than by
    // someone remembering to add it here too — which is how `-Fo` used to reach
    // the probe line and ask a `/EP` run to write an object.
    for (auto const& flag: { R"(/Fob\a.obj)", R"(-Fob\a.obj)" })
    {
        INFO("flag: " << flag);
        std::vector<std::string> const argv { "cl.exe", "/c", flag, "a.cpp" };
        auto const pp = PreprocessCommand(Parse(argv), argv);
        CHECK_FALSE(std::ranges::contains(pp, flag));
        CHECK(std::ranges::contains(pp, "a.cpp"));
    }

    // An include directory is the one role that stays: the preprocessor needs it.
    std::vector<std::string> const gnu { "g++", "-c", "a.cpp", "-o", "a.o", "-Iinc", "-MF", "dep.d" };
    auto const pp = PreprocessCommand(Parse(gnu), gnu);
    CHECK(std::ranges::contains(pp, "-Iinc"));
    CHECK_FALSE(std::ranges::contains(pp, "dep.d"));
}

TEST_CASE("Every path-valued flag row is spelled with a known introducer")
{
    // The table drives matching by introducer, so a row whose spelling begins
    // with anything else could never match and would be dead configuration —
    // the mistake `header-state-v1` records: a thing declared with no work to do.
    for (PathValueFlag const& row: PathValueFlags())
    {
        INFO("row: " << row.spelling);
        REQUIRE_FALSE(row.spelling.empty());
        CHECK(IntroducersOf(row.families).contains(row.spelling.front()));
    }
}

// --- RemoteCompileArgs -------------------------------------------------------

TEST_CASE("RemoteCompileArgs keeps the flags that change generated code")
{
    // These are the whole point: -std, -O, -g, -W, -f and their kin decide what
    // the compiler emits, so an object compiled without them is not the object the
    // client asked for.
    std::vector<std::string> const argv { "g++", "-std=c++23", "-O2", "-g", "-Wall", "-fPIC", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);
    for (auto const* kept: { "-std=c++23", "-O2", "-g", "-Wall", "-fPIC" })
    {
        INFO("flag " << kept);
        CHECK(std::ranges::find(remote, kept) != remote.end());
    }
}

TEST_CASE("cc and c++ are classified by what the driver says, not by what it is called")
{
    // `cc` and `c++` name a policy, not a product. On macOS `/usr/bin/c++` is Apple
    // clang and is CMake's default C++ compiler, so the spelling a build system uses
    // most is the one that says least -- and by name it landed in the gcc row, which
    // is now the row deciding how a target is found and whether it can be stated.
    CHECK(ClassifyCompiler("c++") == Flavor::Gcc);
    CHECK(ClassifyCompilerFromBanner(Flavor::Gcc, "Apple clang version 17.0.0 (clang-1700.0.13.3)") == Flavor::Clang);
    CHECK(ClassifyCompilerFromBanner(Flavor::Gcc, "Ubuntu clang version 20.1.2 (0ubuntu1~24.04.3)") == Flavor::Clang);

    // A real GCC keeps its row.
    CHECK(ClassifyCompilerFromBanner(Flavor::Gcc, "g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0") == Flavor::Gcc);
}

TEST_CASE("An unrecognised banner leaves the name-based guess standing")
{
    // The correction runs in ONE direction, on positive evidence only. Seeing
    // `clang version` is proof; not seeing it is not proof of the opposite, because
    // a driver that could not be RUN falls back to its own basename. Demoting on
    // absence would put a real clang++ whose --version failed into the gcc row --
    // unversioned target, no pin, on a driver whose name said exactly what it was.
    CHECK(ClassifyCompilerFromBanner(Flavor::Clang, "clang++") == Flavor::Clang);
    CHECK(ClassifyCompilerFromBanner(Flavor::Gcc, "cc") == Flavor::Gcc);

    // GCC has no marker as reliable as clang's -- vanilla prints `(GCC)`, Ubuntu
    // prints `(Ubuntu 14.2.0-...)` -- so nothing here tries to prove it.
    CHECK(ClassifyCompilerFromBanner(Flavor::Clang, "g++ (GCC) 14.2.0") == Flavor::Clang);
}

TEST_CASE("A banner cannot reclassify clang-cl, because it does not distinguish it")
{
    // `clang-cl --version` prints `clang version ...`, byte for byte what plain
    // clang prints. The NAME is the only thing separating the two drivers, so a
    // banner test here would collapse clang-cl into clang and take its `/`-spelled
    // command line, its `/EP` preprocess and its MSVC-preprocessed-input flags with
    // it.
    CHECK(ClassifyCompilerFromBanner(Flavor::ClangCl, "clang version 22.1.3") == Flavor::ClangCl);
    CHECK(ClassifyCompilerFromBanner(Flavor::Cl, "clang version 22.1.3") == Flavor::Cl);
    CHECK(ClassifyCompilerFromBanner(Flavor::Unknown, "clang version 22.1.3") == Flavor::Unknown);
}

TEST_CASE("The gcc and clang rows differ in nothing but how a target is discovered")
{
    // What makes correcting the flavour AFTER the command line was parsed safe: the
    // parse read this row, and every column it read is the same in both. Only the
    // target columns may move. If this ever fails, the correction in main.cpp has to
    // move above ParseCommand instead of beside the banner.
    auto const& gcc = DriverOf(Flavor::Gcc);
    auto const& clang = DriverOf(Flavor::Clang);

    CHECK(gcc.family == clang.family);
    CHECK(gcc.preprocessFlags.data() == clang.preprocessFlags.data());
    CHECK(gcc.dispatchPreprocessFlags.data() == clang.dispatchPreprocessFlags.data());
    CHECK(gcc.preprocessedInput.data() == clang.preprocessedInput.data());
    CHECK(gcc.preprocessDropFlags.data() == clang.preprocessDropFlags.data());
    CHECK(gcc.dependencyProbeFlags.data() == clang.dependencyProbeFlags.data());
    CHECK(gcc.usesDepfile == clang.usesDepfile);
    CHECK(gcc.includeDiscovery == clang.includeDiscovery);
    CHECK(gcc.includeProbeFlags.data() == clang.includeProbeFlags.data());
    CHECK(gcc.targetProbeFlags.data() == clang.targetProbeFlags.data());

    // The one column that is allowed to differ, and does.
    CHECK(gcc.targetDiscovery != clang.targetDiscovery);
}

TEST_CASE("gcc is identified but never pinned, because it has no --target")
{
    // Discovering and stating are separate questions. gcc's target belongs in the
    // cache key -- one g++ version string covers x86_64 and aarch64 alike -- and
    // must never reach a dispatched command line, where the driver would reject the
    // flag and fail the compile outright.
    CHECK(TargetPinPrefixFor(TargetDiscovery::GnuTargetLine).empty());
    CHECK(TargetPinPrefixFor(TargetDiscovery::ClangDriverLine) == "--target=");
    CHECK(TargetPinPrefixFor(TargetDiscovery::None).empty());

    std::vector<std::string> const argv { "g++", "-c", "-O2", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, "x86_64-linux-gnu");

    REQUIRE(parsed.has_value());
    CHECK(std::ranges::none_of(*parsed, [](std::string const& a) { return a.starts_with("--target"); }));
}

TEST_CASE("RemoteCompileArgs states the client's target so the worker cannot pick its own")
{
    std::vector<std::string> const argv { "clang-cl", "/c", "/O2", "a.cpp", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);

    auto const parsed = RemoteCompileArgs(cmd, argv, "x86_64-pc-windows-msvc19.51.36252");

    REQUIRE(parsed.has_value());
    // FIRST, before anything the build said for itself. What it states is the
    // DEFAULT the client's own driver would have used, so a build's own
    // `--target=` or `-m32` comes later on the line and still wins -- which is
    // exactly what happens when the same line is compiled locally.
    REQUIRE_FALSE(parsed->empty());
    CHECK(parsed->front() == "--target=x86_64-pc-windows-msvc19.51.36252");

    // Fused, not two arguments: clang-cl rejects `--target x` and accepts
    // `--target=x`, so one spelling covers both drivers.
    CHECK(std::ranges::count_if(*parsed, [](std::string const& a) { return a.starts_with("--target"); }) == 1);
}

TEST_CASE("RemoteCompileArgs states nothing when there is no target to state")
{
    // `cl` has no `--target`: which code generator runs is decided by WHICH cl.exe
    // is invoked. A caller handing it a triple anyway must not put one on the line,
    // because the driver would refuse the argument and the whole compile with it.
    std::vector<std::string> const argv { "cl", "/c", "a.cpp", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);

    auto const parsed = RemoteCompileArgs(cmd, argv, "x86_64-pc-windows-msvc19.51.36252");

    REQUIRE(parsed.has_value());
    CHECK(std::ranges::none_of(*parsed, [](std::string const& a) { return a.starts_with("--target"); }));
}

TEST_CASE("An empty triple leaves the dispatched line exactly as it was")
{
    // The probe fails open, so empty is an answer this receives in practice: a
    // driver that would not say, or one with nothing to say. It must cost nothing.
    std::vector<std::string> const argv { "clang-cl", "/c", "/O2", "a.cpp", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);

    auto const pinned = RemoteCompileArgs(cmd, argv, "x86_64-pc-windows-msvc19.33.0");
    auto const bare = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});

    REQUIRE(pinned.has_value());
    REQUIRE(bare.has_value());
    REQUIRE(pinned->size() == bare->size() + 1);
    CHECK(std::ranges::equal(std::next(pinned->begin()), pinned->end(), bare->begin(), bare->end()));
}

TEST_CASE("A build that names its own target still wins over the stated default")
{
    // The pin goes first precisely so this holds. Overriding a target the build
    // asked for would be a wrong object rather than a failed one, and the two are
    // not comparable: one fails the link, the other is cached and shipped.
    std::vector<std::string> const argv { "clang-cl", "/c", "--target=aarch64-pc-windows-msvc", "a.cpp", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);

    auto const parsed = RemoteCompileArgs(cmd, argv, "x86_64-pc-windows-msvc19.51.36252");

    REQUIRE(parsed.has_value());
    auto const stated = std::ranges::find(*parsed, "--target=x86_64-pc-windows-msvc19.51.36252");
    auto const built = std::ranges::find(*parsed, "--target=aarch64-pc-windows-msvc");
    REQUIRE(stated != parsed->end());
    REQUIRE(built != parsed->end());
    // The driver takes the last one, so the build's must come after ours.
    CHECK(stated < built);
}

TEST_CASE("RemoteCompileArgs drops the source, the output and the compile marker")
{
    // The worker compiles preprocessed text from a file of its own, into a path
    // only it knows, and adds its own -c.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-o", "build/a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);
    CHECK(std::ranges::find(remote, "a.cpp") == remote.end());
    CHECK(std::ranges::find(remote, "build/a.o") == remote.end());
    CHECK(std::ranges::find(remote, "-o") == remote.end());
    CHECK(std::ranges::find(remote, "-c") == remote.end());
    CHECK(std::ranges::find(remote, "g++") == remote.end());
}

TEST_CASE("RemoteCompileArgs drops include directories in both spellings")
{
    // The difference between this list and the preprocess line's: the probe still
    // needs -I because it is resolving headers; the worker must NOT have it,
    // because the headers are already inlined and the path names nothing there.
    std::vector<std::string> const argv { "g++", "-Iinc", "-I", "other", "-O2", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);
    for (auto const* gone: { "-Iinc", "-I", "other" })
    {
        INFO("argument " << gone);
        CHECK(std::ranges::find(remote, gone) == remote.end());
    }
    CHECK(std::ranges::find(remote, "-O2") != remote.end());
}

TEST_CASE("RemoteCompileArgs refuses a command line it cannot fully account for")
{
    // PathValueFlags() does not know -isystem, --sysroot, -B, -specs= or -fplugin=,
    // and it should not have to: it exists to answer questions about the cache key.
    // Several of those point a compiler at an EXECUTABLE. Dropping them silently
    // would change the generated code; sending them would hand a client the ability
    // to make a worker read or run a file of its choosing. Refusing costs one local
    // compile, which is the only one of the three that is not a defect.
    auto const refuses = [](std::vector<std::string> const& argv) {
        auto const cmd = ParseCommand(argv);
        return !RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {}).has_value();
    };

    CHECK(refuses({ "g++", "-isystem", "/opt/sdk/include", "-c", "a.cpp", "-o", "a.o" }));
    CHECK(refuses({ "g++", "--sysroot=/opt/sysroot", "-c", "a.cpp", "-o", "a.o" }));
    CHECK(refuses({ "g++", "-fplugin=/tmp/evil.so", "-c", "a.cpp", "-o", "a.o" }));
    CHECK(refuses({ "g++", "-specs=/tmp/specs", "-c", "a.cpp", "-o", "a.o" }));
    // A response file names a path with no separator at all when it sits in the
    // working directory, so it is called out on its own.
    CHECK(refuses({ "g++", "@args.rsp", "-c", "a.cpp", "-o", "a.o" }));
    // A define whose value happens to hold a path is refused too. Over-strict, and
    // deliberately: the cost is a local compile.
    CHECK(refuses({ "g++", "-DCONFIG=\"/etc/app.conf\"", "-c", "a.cpp", "-o", "a.o" }));

    // The introducer is skipped before the separator is looked for, so an MSVC
    // compile is not refused wholesale for spelling its options with `/` -- but a
    // separator INSIDE one still refuses.
    CHECK(refuses({ "cl", "/DCONFIG=C:\\app\\x.conf", "/c", "a.cpp", "/Foa.obj" }));
    CHECK_FALSE(refuses({ "cl", "/std:c++20", "/O2", "/EHsc", "/c", "a.cpp", "/Foa.obj" }));
}

TEST_CASE("RemoteCompileArgs drops a separated flag together with its value")
{
    // The failure this rejects is a stray bare value: dropping `-MF` but keeping
    // `dep.d` would hand the worker a filename it would treat as a second source.
    std::vector<std::string> const argv { "g++", "-MF", "dep.d", "-O2", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);
    CHECK(std::ranges::find(remote, "dep.d") == remote.end());
    CHECK(std::ranges::find(remote, "-MF") == remote.end());
    CHECK(std::ranges::find(remote, "-O2") != remote.end());
}

TEST_CASE("RemoteCompileArgs leaves nothing that names a path")
{
    // The security-relevant property, stated as an assertion rather than a hope:
    // every argument naming a filesystem path is removed, so nothing in this list
    // can make the worker read or write somewhere of the client's choosing. The
    // worker separately refuses to take its COMPILER from the client.
    std::vector<std::string> const argv { "clang++", "-Isecret", "-o",  "/etc/passwd", "-MF",  "/tmp/x.d",
                                          "-MT",     "target",   "-O2", "-c",          "a.cpp" };
    auto const cmd = ParseCommand(argv);

    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);
    for (auto const& arg: remote)
    {
        INFO("surviving argument: " << arg);
        CHECK_FALSE(arg.contains('/'));
        CHECK_FALSE(arg.contains('\\'));
        CHECK_FALSE(arg.starts_with('@'));
    }
}

TEST_CASE("RemoteCompileArgs drops the MSVC spellings too")
{
    std::vector<std::string> const argv { "cl", "/std:c++20", "/O2", "/Iinc", "/Foa.obj", "/c", "a.cpp" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);
    CHECK(std::ranges::find(remote, "/std:c++20") != remote.end());
    CHECK(std::ranges::find(remote, "/O2") != remote.end());
    CHECK(std::ranges::find(remote, "/Iinc") == remote.end());
    CHECK(std::ranges::find(remote, "/c") == remote.end());
    CHECK(std::ranges::find(remote, "a.cpp") == remote.end());
}

// --- preprocessing for a remote compile --------------------------------------

TEST_CASE("The dispatch preprocess keeps line markers that the key's suppresses")
{
    // The two runs answer different questions. The key's text must carry no path,
    // so it suppresses markers; a worker's must carry them, because they are what
    // tells the compiler which lines came from a system header.
    std::vector<std::string> const argv { "g++", "-O2", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const forKey = PreprocessCommand(cmd, argv);
    auto const forWorker = DispatchPreprocessCommand(cmd, argv);

    CHECK(std::ranges::find(forKey, "-P") != forKey.end());
    CHECK(std::ranges::find(forWorker, "-P") == forWorker.end());
    CHECK(std::ranges::find(forWorker, "-E") != forWorker.end());
    // The source still has to be there, or the run preprocesses nothing.
    CHECK(std::ranges::find(forWorker, "a.cpp") != forWorker.end());
    // ...and the object output must not, or the run writes one.
    CHECK(std::ranges::find(forWorker, "-o") == forWorker.end());
}

TEST_CASE("The dispatch preprocess asks for no dependency reporting")
{
    // The key's probe already reported them. Asking again would make this run write
    // a depfile the caller has no use for -- and, worse, over the build's own.
    std::vector<std::string> const argv { "g++", "-MD", "-MF", "dep.d", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    auto const forWorker = DispatchPreprocessCommand(cmd, argv);

    CHECK(std::ranges::find(forWorker, "-MD") == forWorker.end());
    CHECK(std::ranges::find(forWorker, "-MF") == forWorker.end());
    CHECK(std::ranges::find(forWorker, "dep.d") == forWorker.end());
}

TEST_CASE("A remote compile is told its input is already preprocessed")
{
    // Keeping the markers fixes system-header warnings and immediately creates a
    // second problem: under -pedantic the markers themselves are a GNU extension,
    // so clang reports -Wgnu-line-marker and -Werror turns that into a failed
    // compile. Naming the language as preprocessed output is what makes the driver
    // expect them -- the same thing ccache and distcc do. Verified end to end:
    // -Wall -Wextra -pedantic -Werror dispatches and returns a byte-identical
    // object, where before it failed and was retried locally.
    std::vector<std::string> const argv { "g++", "-O2", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);

    auto const x = std::ranges::find(remote, "-x");
    REQUIRE(x != remote.end());
    REQUIRE(std::next(x) != remote.end());
    CHECK(*std::next(x) == "c++-cpp-output");
}

TEST_CASE("A C source is told the C preprocessed language, not the C++ one")
{
    // `c++-cpp-output` on a C translation unit compiles it as C++, which changes
    // overload resolution, name mangling and what even parses.
    std::vector<std::string> const argv { "gcc", "-O2", "-c", "a.c", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);

    auto const x = std::ranges::find(remote, "-x");
    REQUIRE(x != remote.end());
    CHECK(*std::next(x) == "cpp-output");
}

TEST_CASE("An MSVC remote compile is told its language too, in MSVC's own spelling")
{
    // This used to append nothing, on the reasoning that `/E` emits standard #line
    // and so there is nothing to tell -- true about the MARKERS, and it left the
    // LANGUAGE unstated. The worker writes its scratch file as `tu.cpp` and MSVC
    // reads the language off that extension, so a dispatched `.c` came back
    // compiled as C++: a failed remote compile where C is not valid C++, and an
    // object with C++ mangling under the C key where it is.
    std::vector<std::string> const argv { "cl", "/O2", "/c", "a.cpp", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);

    // Appended LAST, so it overrides a `/TC` the build itself passed.
    REQUIRE_FALSE(remote.empty());
    CHECK(remote.back() == "/TP");
    // And still no GNU spelling, which the driver would reject outright.
    CHECK(std::ranges::find(remote, "-x") == remote.end());
}

TEST_CASE("An MSVC C translation unit is told /TC")
{
    std::vector<std::string> const argv { "cl", "/O2", "/c", "a.c", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const remote = Unwrap(parsed);

    REQUIRE_FALSE(remote.empty());
    CHECK(remote.back() == "/TC");
}

TEST_CASE("A language the driver has no spelling for is refused, and says so")
{
    // Objective-C is a GNU-driver language: an MSVC driver compiles no such thing,
    // so there is no flag that would make this work and sending it anyway is how a
    // job comes back compiled as something else.
    std::vector<std::string> const argv { "cl", "/c", "a.m", "/Foa.obj" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().contains("Objective-C"));

    // The same source IS dispatchable to a GNU driver.
    std::vector<std::string> const gnu { "clang", "-c", "a.m", "-o", "a.o" };
    auto const gnuCmd = ParseCommand(gnu);
    auto const gnuParsed = RemoteCompileArgs(gnuCmd, gnu, /*targetTriple=*/ {});
    REQUIRE(gnuParsed.has_value());
    CHECK(Unwrap(gnuParsed).back() == "objective-c-cpp-output");
}

TEST_CASE("An extension whose language depends on the driver is never guessed at")
{
    // `.C` is C++ to a GNU driver and C to an MSVC one, so the extension does not
    // answer the question and a guess would hand a worker the wrong language.
    std::vector<std::string> const argv { "g++", "-c", "a.C", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().contains("a.C"));
}

TEST_CASE("A compile that writes a second artefact is not cacheable")
{
    // What a hit reproduces is the object and the dependency record, so a compile
    // that also writes a BMI or a precompiled header cannot be replayed: the second
    // artefact is afterwards missing, which fails loudly, or left over from a
    // previous build, which does not.
    //
    // Every spelling here is the FUSED one, deliberately -- that is how a build
    // writes them, and matching only the bare form would have caught the shape
    // nobody uses.
    for (auto const& argv: { std::vector<std::string> { "cl", "/c", "/Ycpch.h", "a.cpp", "/Foa.obj" },
                             std::vector<std::string> { "cl", "/c", "/interface", "a.cpp", "/Foa.obj" },
                             std::vector<std::string> { "cl", "/c", "/ifcOutputa.ifc", "a.cpp", "/Foa.obj" },
                             std::vector<std::string> { "clang++", "-c", "-fmodule-output=a.pcm", "a.cpp", "-o", "a.o" },
                             std::vector<std::string> { "g++", "-c", "-fmodule-mapper=a.modmap", "a.cpp", "-o", "a.o" } })
    {
        auto const cmd = ParseCommand(argv);
        INFO("flag: " << argv[2]);
        CHECK(cmd.sideArtefact);
        CHECK_FALSE(cmd.parsedOk);
    }

    // And an ordinary compile is untouched -- including `/Yu`, which USES a
    // precompiled header rather than writing one.
    for (auto const& argv: { std::vector<std::string> { "cl", "/c", "/Yupch.h", "a.cpp", "/Foa.obj" },
                             std::vector<std::string> { "cl", "/c", "/O2", "a.cpp", "/Foa.obj" },
                             std::vector<std::string> { "g++", "-c", "-fmodules-ts", "a.cpp", "-o", "a.o" } })
    {
        auto const cmd = ParseCommand(argv);
        INFO("flag: " << argv[2]);
        CHECK_FALSE(cmd.sideArtefact);
        CHECK(cmd.parsedOk);
    }
}

TEST_CASE("A module interface unit is not cacheable, by extension")
{
    // Recognised so it can be refused with its reason said out loud. Before this
    // they were not in IsSourceSuffix at all, so the line fell through as "no
    // source file found" and was passed through in silence -- and adding `.ixx`
    // there, the obvious way to "support modules", would have started replaying
    // objects whose BMI nobody reproduced.
    for (auto const& source: { std::string { "m.ixx" },
                               std::string { "m.cppm" },
                               std::string { "m.ccm" },
                               std::string { "m.cxxm" },
                               std::string { "m.mxx" } })
    {
        std::vector<std::string> const argv { "clang++", "-c", source, "-o", "m.o" };
        auto const cmd = ParseCommand(argv);
        INFO("source: " << source);
        CHECK(cmd.source == source); // recognised, not ignored
        CHECK(cmd.sideArtefact);
        CHECK_FALSE(cmd.parsedOk);
    }
}

TEST_CASE("A `++` driver compiles .c as C++, and the worker is told so")
{
    // "g++ treats .c, .h and .i files as C++ source files instead of C source
    // files", in as many words. Taking the language off the extension alone would
    // tell a worker to compile as C what this machine compiles as C++ -- a wrong
    // object rather than a failed one, stored under the key for everybody.
    std::vector<std::string> const argv { "g++", "-c", "a.c", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    CHECK(Unwrap(parsed).back() == "c++-cpp-output");

    // The same source through the C driver is C, which is the whole point of the
    // column: `gcc` and `g++` are one Flavor and disagree about this.
    std::vector<std::string> const c { "gcc", "-c", "a.c", "-o", "a.o" };
    auto const cCmd = ParseCommand(c);
    auto const cParsed = RemoteCompileArgs(cCmd, c, /*targetTriple=*/ {});
    REQUIRE(cParsed.has_value());
    CHECK(Unwrap(cParsed).back() == "cpp-output");

    // Version suffixes and .exe are recognised here exactly as ClassifyCompiler
    // recognises them, because it is the same table walked for another column.
    for (auto const& spelling: { std::string { "g++-14" }, std::string { "clang++" }, std::string { "c++" } })
    {
        std::vector<std::string> const each { spelling, "-c", "a.c", "-o", "a.o" };
        auto const eachCmd = ParseCommand(each);
        auto const eachParsed = RemoteCompileArgs(eachCmd, each, /*targetTriple=*/ {});
        INFO("driver: " << spelling);
        REQUIRE(eachParsed.has_value());
        CHECK(Unwrap(eachParsed).back() == "c++-cpp-output");
    }
}

TEST_CASE("A language the build named is restated to the worker, not refused")
{
    // #232. CMake emits `/TP` on EVERY C++ source it compiles with MSVC, and this
    // used to be a refusal -- so no CMake + MSVC translation unit was dispatchable
    // at all. The fleet cached normally and distributed nothing, with every
    // scheduler counter reading zero because no lease was ever requested.
    //
    // The refusal was the best answer available to a table that recorded only THAT
    // a language was named, never WHICH. Now the row carries the language, so the
    // flag is folded into the language and dropped -- and what the launcher appends
    // says the same thing and more: "this text is preprocessed C++" rather than
    // "this source is C++".
    struct Case
    {
        std::vector<std::string> argv;
        std::string_view spelling; ///< The appended flag that must end the line.
        std::string_view value;    ///< Its value for a GNU driver; empty for MSVC.
    };

    // Every case names a language the EXTENSION contradicts, which is the whole
    // point: if the flag were ignored rather than read, the appended spelling would
    // be the extension's answer and the worker would compile the wrong language.
    for (auto const& [argv, spelling, value]:
         { Case { .argv = { "cl", "/c", "/TP", "a.c", "/Foa.obj" }, .spelling = "/TP", .value = "" },
           Case { .argv = { "cl", "/c", "/TC", "a.cpp", "/Foa.obj" }, .spelling = "/TC", .value = "" },
           Case { .argv = { "g++", "-c", "-x", "c", "a.cpp", "-o", "a.o" }, .spelling = "-x", .value = "cpp-output" },
           Case { .argv = { "clang", "-c", "-xc++", "a.c", "-o", "a.o" }, .spelling = "-x", .value = "c++-cpp-output" } })
    {
        auto const cmd = ParseCommand(argv);
        auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
        INFO("driver: " << argv.front() << " selector: " << spelling);
        REQUIRE(parsed.has_value());
        auto const& out = Unwrap(parsed);

        if (value.empty())
        {
            CHECK(out.back() == spelling);
            // ONE occurrence: the build's own copy was dropped rather than
            // forwarded, so the worker is not handed the language twice.
            CHECK(std::ranges::count(out, spelling) == 1);
        }
        else
        {
            REQUIRE(out.size() >= 2);
            CHECK(out.back() == value);
            CHECK(out.at(out.size() - 2) == spelling);
            CHECK(std::ranges::count(out, spelling) == 1);
            // The separated form's value went with it. Left behind, a bare `c++`
            // is an argument the worker would try to open as a file.
            CHECK(std::ranges::count(out, "c++") == 0);
            CHECK(std::ranges::count(out, "c") == 0);
        }
    }
}

TEST_CASE("A language selector this launcher cannot restate is still refused")
{
    // The half that must not change. `-x assembler` has no `SourceLanguage`, so
    // there is no "this text is preprocessed <language>" to append -- and guessing
    // hands back an object nobody asked for, which is worse than a local compile.
    for (auto const& argv: { std::vector<std::string> { "g++", "-c", "-x", "assembler", "a.cpp", "-o", "a.o" },
                             std::vector<std::string> { "clang", "-c", "-xc-header", "a.c", "-o", "a.o" } })
    {
        auto const cmd = ParseCommand(argv);
        auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
        INFO("argv: " << argv.at(2));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().contains("names the input language itself"));
    }

    // `/Tc` and `/Tp` name a FILE as well, and with a bare name they carry no
    // separator -- so the path filter would have let them past to a worker that
    // has no such file.
    std::vector<std::string> const named { "cl", "/c", "/Tcother.c", "/Foa.obj", "a.c" };
    auto const namedCmd = ParseCommand(named);
    CHECK_FALSE(RemoteCompileArgs(namedCmd, named, /*targetTriple=*/ {}).has_value());
}

TEST_CASE("A CMake-shaped MSVC command line is dispatchable")
{
    // The shape that actually broke, written the way CMake writes it rather than
    // the way a test author would. The hand-written argv above is what let #232
    // through review: the refusal reads as correct on a minimal line, and CMake
    // puts `/TP` on every C++ source it compiles.
    std::vector<std::string> const argv { "cl",
                                          "/nologo",
                                          "/TP",
                                          "-DWIN32",
                                          "-D_WINDOWS",
                                          "-DNDEBUG",
                                          "/EHsc",
                                          "/O2",
                                          "/MD",
                                          "-std:c++23",
                                          "/showIncludes",
                                          "/FoCMakeFiles\tgt.dir\a.cpp.obj",
                                          "/FdCMakeFiles\tgt.dir\tgt.pdb",
                                          "/FS",
                                          "/c",
                                          "a.cpp" };

    auto const cmd = ParseCommand(argv);
    auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
    REQUIRE(parsed.has_value());
    auto const& out = Unwrap(parsed);

    // Dispatchable, and the language survives as the appended spelling.
    CHECK(out.back() == "/TP");
    CHECK(std::ranges::count(out, "/TP") == 1);

    // The flags that change generated code still travel; the object path and the
    // source do not, because both are the worker's to name.
    CHECK(std::ranges::count(out, "/EHsc") == 1);
    CHECK(std::ranges::count(out, "-std:c++23") == 1);
    CHECK(std::ranges::count(out, "a.cpp") == 0);
    CHECK(std::ranges::none_of(out, [](std::string const& a) { return a.starts_with("/Fo"); }));

    // `/Fd` names a path on THIS machine, and under /Z7 nothing writes a PDB at
    // all, so it is dropped rather than refused. It was the second of the two
    // blockers: with `/TP` handled, this one alone still made every CMake + MSVC
    // compile fall back to a local build.
    CHECK(std::ranges::none_of(out, [](std::string const& a) { return a.starts_with("/Fd"); }));
}

TEST_CASE("A compile writing a shared PDB is not dispatched")
{
    // `/Fd` is dropped because with `/Z7` nothing writes a PDB. Say `/Zi` and one
    // IS written -- a second artefact beside the object, and only the object comes
    // back. `cmake/portable/CompileCache.cmake` rewrites `/Zi` to `/Z7` whenever a
    // launcher is active for this very reason, so this guards the build that
    // reached the launcher another way.
    for (auto const& flag: { "/Zi", "/ZI", "-Zi" })
    {
        std::vector<std::string> const argv { "cl", "/nologo", "/TP", flag, "/FdX.pdb", "/c", "a.cpp" };
        auto const cmd = ParseCommand(argv);
        auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
        INFO("flag: " << flag);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().contains("shared PDB"));
    }
}

TEST_CASE("A module interface unit is never dispatched, whatever the driver")
{
    // It writes a BMI beside the object and a dispatched compile carries back only
    // the object, so the BMI would be missing (a loud failure) or left over from a
    // previous build (a quiet wrong one).
    for (auto const& source: { std::string { "m.ixx" }, std::string { "m.cppm" } })
    {
        std::vector<std::string> const argv { "clang++", "-c", source, "-o", "m.o" };
        auto const cmd = ParseCommand(argv);
        auto const parsed = RemoteCompileArgs(cmd, argv, /*targetTriple=*/ {});
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().contains("BMI"));
    }
}

// --- MappedCompileDirectory --------------------------------------------------

TEST_CASE("MappedCompileDirectory answers nothing when the build maps nothing")
{
    // The load-bearing case, and the whole argument against the worker choosing a
    // token of its own: a build that asked for no mapping must get back an object
    // recording no invented directory. Nothing here is what carries that to the
    // worker (#506).
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-o", "a.o", "-O2", "-g" };
    CHECK_FALSE(MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci/build").has_value());
}

TEST_CASE("MappedCompileDirectory answers nothing when no rule reaches this directory")
{
    // A rule mapping the SOURCE tree says nothing about a working directory outside
    // it. Reporting its replacement anyway would have the worker record a directory
    // this compile never records.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/ci/src=../.." };
    CHECK_FALSE(MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci/build").has_value());
}

TEST_CASE("MappedCompileDirectory reads the build-tree rule this project emits")
{
    // The shape `_fc_debug_prefix_map_rules` produces: source rule first, build-tree
    // rule last, the working directory being the build tree. Measured on gcc 14 and
    // clang 20, such a compile records `.` as its `DW_AT_comp_dir`.
    std::vector<std::string> const argv {
        "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/ci/src=../../..", "-fdebug-prefix-map=/home/ci/out/build/x=."
    };
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci/out/build/x");
    REQUIRE(mapped.has_value());
    CHECK(Unwrap(mapped).replacement == ".");
    // The directory travels beside it, because a worker needs both halves of a rule.
    CHECK(Unwrap(mapped).directory == "/home/ci/out/build/x");
}

TEST_CASE("MappedCompileDirectory follows the LAST matching rule, not the first")
{
    // Both drivers honour the last match -- measured, two rules over one directory
    // giving the second replacement. Not a detail: `_fc_debug_prefix_map_rules` emits
    // the source rule first and the build-tree rule last SO THAT the build tree wins,
    // so a first-match model would predict the source rule's replacement for every
    // build this project does.
    std::vector<std::string> const argv {
        "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/ci=A", "-fdebug-prefix-map=/home/ci=B"
    };
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci");
    REQUIRE(mapped.has_value());
    CHECK(Unwrap(mapped).replacement == "B");
}

TEST_CASE("MappedCompileDirectory keeps the tail below a mapped root")
{
    // A recursive make runs the compiler in a subdirectory of the build tree, so the
    // rule matches a PREFIX and the remainder survives. Measured: `/root=.` with a
    // working directory of `/root/sub/deeper` gives `./sub/deeper` on both drivers.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/ci/build=." };
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci/build/sub/deeper");
    REQUIRE(mapped.has_value());
    CHECK(Unwrap(mapped).replacement == "./sub/deeper");
    CHECK(Unwrap(mapped).directory == "/home/ci/build/sub/deeper");
}

TEST_CASE("MappedCompileDirectory matches a BYTE prefix, as the drivers do")
{
    // Not a path prefix. Measured on gcc 14 and clang 20:
    // `-fdebug-prefix-map=/tmp/work=X` against a working directory of `/tmp/worker`
    // yields `Xer`. Modelling it as a component-boundary match would read better and
    // would make this client predict a replacement neither compiler writes, which is
    // exactly the disagreement the field exists to end.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-fdebug-prefix-map=/tmp/work=X" };
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, "/tmp/worker");
    REQUIRE(mapped.has_value());
    CHECK(Unwrap(mapped).replacement == "Xer");
}

TEST_CASE("MappedCompileDirectory splits a rule where GCC splits it")
{
    // The launcher follows gcc, which cuts `<from>=<to>` at the LAST separator, so a
    // mapped root containing one still isolates the whole root. `MatchPathValueFlag`
    // owns that rule; this asserts it is the rule reaching the replacement too.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/a=b/build=." };
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/a=b/build");
    REQUIRE(mapped.has_value());
    CHECK(Unwrap(mapped).replacement == ".");
}

TEST_CASE("MappedCompileDirectory reports a rule that maps a root to nothing")
{
    // `-fdebug-prefix-map=<builddir>=` is a standard reproducible-build spelling and it
    // IS a mapping: the local object records an empty compilation directory. So the
    // directory travels with an empty replacement, and the worker must treat that as a
    // mapping rather than as half a pair -- refusing it there cost such a build
    // distribution entirely.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/ci/build=" };
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci/build");
    REQUIRE(mapped.has_value());
    CHECK(Unwrap(mapped).directory == "/home/ci/build");
    CHECK(Unwrap(mapped).replacement.empty());
}

TEST_CASE("MappedCompileDirectory ignores a rule with no replacement")
{
    // `-fdebug-prefix-map=/abs` is malformed and the driver says so; it maps nothing,
    // so it must not read as a mapping to the empty string -- which would travel as
    // "this client maps nothing" and be indistinguishable from a build with no rule at
    // all, while meaning something different.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-fdebug-prefix-map=/home/ci/build" };
    CHECK_FALSE(MappedCompileDirectory(argv, DriverFamily::Gnu, "/home/ci/build").has_value());
}

TEST_CASE("MappedCompileDirectory reads no rule on an MSVC line")
{
    // `/` introduces an option under a Windows layout, and the prefix-map row is
    // GNU-only because neither COFF driver honours such a flag. A line carrying the
    // spelling anyway must not make this client promise a mapping its own compiler
    // will not perform.
    std::vector<std::string> const argv { "cl.exe", "/c", "a.cpp", "-fdebug-prefix-map=C:/ci/build=." };
    CHECK_FALSE(MappedCompileDirectory(argv, DriverFamily::Msvc, "C:/ci/build").has_value());
}

// --- CompilerWorkingDirectory ------------------------------------------------

TEST_CASE("CompilerWorkingDirectory falls back to the resolved directory in every unusable case")
{
    // Each row was measured against gcc 14 and clang 20 -- see the table on the
    // declaration -- and each one is a spelling the driver ignored, leaving
    // `DW_AT_comp_dir` at getcwd(3)'s answer. A test that only covered the symlink case
    // would let this function trust any `PWD` at all, which is the shape that puts a
    // directory nothing is in on the wire.
    //
    // **Every row here must reach the guard it names.** Two of them did not: written
    // against a `/tmp/real/build` that does not exist on the test host, they fell back
    // through `equivalent`'s ERROR path whichever guard was deleted, so the relative-PWD
    // rule and the empty-directory rule were both asserted by rows that could not fail.
    // The directories are real now, and the two rows that turn on a guard are separated
    // out below.
    FastCache::Testing::ScratchDirectory const scratch { "fc-cc-cwdfallback" };
    auto const& base = scratch.Path();
    std::filesystem::create_directories(base / "build");
    std::filesystem::create_directories(base / "other");
    auto const physical = (base / "build").string();

    CHECK(CompilerWorkingDirectory(physical, "") == physical);
    CHECK(CompilerWorkingDirectory(physical, "/nonexistent/xyz") == physical);

    // A real, absolute directory that is simply not this one. `equivalent` succeeds and
    // answers false, so this is the only row that exercises the identity test itself.
    CHECK(CompilerWorkingDirectory(physical, (base / "other").string()) == physical);

    std::filesystem::remove_all(base);
}

TEST_CASE("CompilerWorkingDirectory consults PWD only for a POSIX-rooted spelling")
{
    // The guard, asserted with no filesystem and no symlink privilege, because the
    // symlink cases above cannot run everywhere and this rule holds everywhere.
    //
    // `path::is_absolute()` is the same test on POSIX and strictly MORE permissive on a
    // Windows layout: it admits `D:/work` and `\\host\share`, for neither of which does
    // any Windows driver read `PWD`. Predicting from one would send a worker a mapping
    // the local compile never applied -- #506's asymmetry, rebuilt by its own fix. Rows
    // that cannot exist on the running platform are still meaningful: the question is
    // what the SPELLING is, and these strings carry it.
    CHECK(CompilerWorkingDirectory("/tmp/real/build", "D:/other/build") == "/tmp/real/build");
    CHECK(CompilerWorkingDirectory("/tmp/real/build", "//host/share/build") == "/tmp/real/build");
    CHECK(CompilerWorkingDirectory("/tmp/real/build", "C:build") == "/tmp/real/build");
}

TEST_CASE("CompilerWorkingDirectory refuses a relative PWD that names this very directory")
{
    // The row this replaces could not fail. `equivalent` resolves a RELATIVE path
    // against the calling process's own working directory, so the input that reaches
    // the rooted-path guard has to be relative AND name the same directory as
    // `physicalDirectory` -- which means this process's cwd, and nothing else.
    //
    // Without the guard, `equivalent(".", cwd)` is true and the function returns `"."`
    // as a compilation directory. That is not merely wrong, it is the one answer that
    // would be accepted by the byte-prefix test downstream against a relative rule and
    // then sent to a worker. Measured, both drivers ignore a relative `PWD`.
    std::error_code ec;
    auto const cwd = std::filesystem::current_path(ec);
    REQUIRE_FALSE(ec);

    CHECK(CompilerWorkingDirectory(cwd.string(), ".") == cwd.string());
    CHECK(CompilerWorkingDirectory(cwd.string(), "") == cwd.string());
}

TEST_CASE("CompilerWorkingDirectory has no answer without a physical directory")
{
    // Also a guard that a `/tmp/real/build` row could not reach: with the empty-physical
    // half deleted, `equivalent(pwd, "")` errors and the fallback returns `""` anyway,
    // so the assertion held either way. An EXISTING `PWD` is what separates them --
    // the guard must fire before the identity test can succeed against nothing.
    FastCache::Testing::ScratchDirectory const scratch { "fc-cc-cwdempty" };
    auto const& base = scratch.Path();
    std::filesystem::create_directories(base);

    CHECK(CompilerWorkingDirectory("", base.string()).empty());

    std::filesystem::remove_all(base);
}

TEST_CASE("CompilerWorkingDirectory reads PWD from the environment through its one-argument form")
{
    // The overload BOTH call sites use, and it had no test: every other case drives the
    // two-argument form. Its two parameters are `std::string_view`, so a swapped pair or
    // the wrong variable name in the wrapper is type-checked, silent, and would leave
    // the launcher predicting a directory from nothing.
    FastCache::Testing::ScratchDirectory const scratch { "fc-cc-cwdenv" };
    auto const& base = scratch.Path();
    std::filesystem::create_directories(base / "real" / "build");

    std::error_code ec;
    std::filesystem::create_directory_symlink(base / "real", base / "link", ec);
    if (ec)
    {
        std::filesystem::remove_all(base);
        SKIP("symlinks unavailable on this host, so PWD cannot be made to differ from the resolved path");
    }

    auto const physical = (base / "real" / "build").string();
    auto const logical = (base / "link" / "build").string();

    {
        FastCache::Testing::ScopedEnv const pwd { "PWD", logical };
#if defined(_WIN32)
        // A Windows layout spells this `C:\...`, which carries a root NAME, and no
        // Windows driver consults `PWD` -- libiberty's `getpwd()` gates on a leading
        // `/` and LLVM does the `PWD` dance only in `Unix/Path.inc`. So the honest
        // expectation here is the RESOLVED directory, and asserting it is what stops
        // the guard being loosened to `is_absolute()` by someone reading only the
        // POSIX case.
        CHECK(CompilerWorkingDirectory(physical) == physical);
#else
        CHECK(CompilerWorkingDirectory(physical) == logical);
#endif
    }
    {
        // And a `PWD` naming somewhere else is ignored on every platform, so the
        // wrapper cannot be passing the variable through unread.
        FastCache::Testing::ScopedEnv const pwd { "PWD", (base / "real").string() };
        CHECK(CompilerWorkingDirectory(physical) == physical);
    }

    std::filesystem::remove_all(base);
}

TEST_CASE("CompilerWorkingDirectory keeps the symlinked spelling a driver reports")
{
    // #506's macOS failure, in one call. `current_path()` is getcwd(3) and resolves
    // every link, while both drivers report and prefix-match `$PWD` when it names the
    // same directory -- so on a build reached through a link the rule on the line
    // spells the link, the resolved cwd does not match it, and the client concludes
    // that a build which maps perfectly well maps nothing. It then sends no mapping and
    // the dispatched object keeps the WORKER's directory while the local one is `.`.
    //
    // The same arrangement as `AnchorWorkingDirectory`'s test, and for the same reason:
    // macOS's `/var` is a symlink, so `$TMPDIR` is where CI meets this.
    FastCache::Testing::ScratchDirectory const scratch { "fc-cc-logicalcwd" };
    auto const& base = scratch.Path();
    std::filesystem::create_directories(base / "real" / "build");

    std::error_code ec;
    std::filesystem::create_directory_symlink(base / "real", base / "link", ec);
    if (ec)
    {
        // Symlinks need a privilege or developer mode on Windows. The rule is
        // unconditional; only this way of demonstrating it is not -- so this is SKIPPED
        // and not a pass. `SUCCEED` here would report green for the one property the
        // change exists to establish, on every host that cannot make a link.
        // `catch_discover_tests` carries `SKIP_RETURN_CODE 4`, so ctest scores it as a
        // skip rather than as a failure.
        std::filesystem::remove_all(base);
        SKIP("symlinks unavailable on this host");
    }

    auto const physical = (base / "real" / "build").string();
    auto const logical = (base / "link" / "build").string();
    REQUIRE(physical != logical);

    // The whole point: a different spelling of the same directory is KEPT -- on a
    // POSIX layout. A Windows one carries a root name, no Windows driver reads `PWD`,
    // and the resolved directory is then the right answer; see the one-argument case
    // above for why that is a rule rather than a limitation.
#if defined(_WIN32)
    CHECK(CompilerWorkingDirectory(physical, logical) == physical);
#else
    CHECK(CompilerWorkingDirectory(physical, logical) == logical);
#endif

    // A real directory that is not this one is not kept -- the driver checks identity
    // rather than existence, and `equivalent` is that check.
    std::filesystem::create_directories(base / "real" / "decoy");
    CHECK(CompilerWorkingDirectory(physical, (base / "real" / "decoy").string()) == physical);

    // And end to end through the consumer, which is where the bug actually bit: the
    // build's rule names the directory the way the shell reached it, so the resolved
    // spelling matches nothing and the pair never travels. This half is POSIX-only
    // because the mechanism is -- see above -- and a Windows `-fdebug-prefix-map` is
    // a GNU-layout driver on a host whose drivers do not read `PWD`.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-g", "-fdebug-prefix-map=" + logical + "=." };
    CHECK_FALSE(MappedCompileDirectory(argv, DriverFamily::Gnu, physical).has_value());

#if !defined(_WIN32)
    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, CompilerWorkingDirectory(physical, logical));
    REQUIRE(mapped.has_value());
    // `Unwrap`, not `mapped->`: the pinned clang-tidy reports a bare `operator->` as
    // `bugprone-unchecked-optional-access` whatever the `REQUIRE` one line above
    // established, and neither MSVC nor GCC says a word about it.
    auto const& pair = FastCache::Testing::Unwrap(mapped);
    CHECK(pair.directory == logical);
    CHECK(pair.replacement == ".");
#endif

    std::filesystem::remove_all(base);
}
