// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iterator>
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
    // the mistake PathCanon::CanonError already records.
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

    auto const remote = RemoteCompileArgs(cmd, argv);
    REQUIRE(remote.has_value());
    for (auto const* kept: { "-std=c++23", "-O2", "-g", "-Wall", "-fPIC" })
    {
        INFO("flag " << kept);
        CHECK(std::ranges::find(*remote, kept) != remote->end());
    }
}

TEST_CASE("RemoteCompileArgs drops the source, the output and the compile marker")
{
    // The worker compiles preprocessed text from a file of its own, into a path
    // only it knows, and adds its own -c.
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-o", "build/a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const remote = RemoteCompileArgs(cmd, argv);
    REQUIRE(remote.has_value());
    CHECK(std::ranges::find(*remote, "a.cpp") == remote->end());
    CHECK(std::ranges::find(*remote, "build/a.o") == remote->end());
    CHECK(std::ranges::find(*remote, "-o") == remote->end());
    CHECK(std::ranges::find(*remote, "-c") == remote->end());
    CHECK(std::ranges::find(*remote, "g++") == remote->end());
}

TEST_CASE("RemoteCompileArgs drops include directories in both spellings")
{
    // The difference between this list and the preprocess line's: the probe still
    // needs -I because it is resolving headers; the worker must NOT have it,
    // because the headers are already inlined and the path names nothing there.
    std::vector<std::string> const argv { "g++", "-Iinc", "-I", "other", "-O2", "-c", "a.cpp", "-o", "a.o" };
    auto const cmd = ParseCommand(argv);
    REQUIRE(cmd.parsedOk);

    auto const remote = RemoteCompileArgs(cmd, argv);
    REQUIRE(remote.has_value());
    for (auto const* gone: { "-Iinc", "-I", "other" })
    {
        INFO("argument " << gone);
        CHECK(std::ranges::find(*remote, gone) == remote->end());
    }
    CHECK(std::ranges::find(*remote, "-O2") != remote->end());
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
        return !RemoteCompileArgs(cmd, argv).has_value();
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

    auto const remote = RemoteCompileArgs(cmd, argv);
    REQUIRE(remote.has_value());
    CHECK(std::ranges::find(*remote, "dep.d") == remote->end());
    CHECK(std::ranges::find(*remote, "-MF") == remote->end());
    CHECK(std::ranges::find(*remote, "-O2") != remote->end());
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

    auto const remote = RemoteCompileArgs(cmd, argv);
    REQUIRE(remote.has_value());
    for (auto const& arg: *remote)
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

    auto const remote = RemoteCompileArgs(cmd, argv);
    REQUIRE(remote.has_value());
    CHECK(std::ranges::find(*remote, "/std:c++20") != remote->end());
    CHECK(std::ranges::find(*remote, "/O2") != remote->end());
    CHECK(std::ranges::find(*remote, "/Iinc") == remote->end());
    CHECK(std::ranges::find(*remote, "/c") == remote->end());
    CHECK(std::ranges::find(*remote, "a.cpp") == remote->end());
}
