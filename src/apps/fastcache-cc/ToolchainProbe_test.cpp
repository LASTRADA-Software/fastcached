// SPDX-License-Identifier: Apache-2.0
#include "ToolchainProbe.hpp"

#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{
/// Real `clang -E -v -x c++ /dev/null` stderr, trimmed to the shape that matters.
///
/// Captured rather than invented: the surrounding noise is what the parser has to
/// step over, and a hand-written fixture would only ever contain the lines whose
/// handling the author already thought of.
constexpr std::string_view AppleClangVerbose = R"(Apple clang version 21.0.0 (clang-2100.1.1.101)
Target: arm64-apple-darwin25.5.0
Thread model: posix
InstalledDir: /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
 "/Applications/Xcode.app/.../clang" -cc1 -triple arm64-apple-macosx15.0.0 -E
clang -cc1 version 21.0.0 based upon LLVM 21.0.0 default target arm64-apple-darwin25.5.0
ignoring nonexistent directory "/usr/local/include"
#include "..." search starts here:
#include <...> search starts here:
 /Applications/Xcode.app/Contents/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1
 /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/21/include
 /Applications/Xcode.app/Contents/Developer/SDKs/MacOSX.sdk/usr/include
 /Applications/Xcode.app/Contents/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks (framework directory)
End of search list.
# 1 "/dev/null"
)";

/// A scratch directory tree that removes itself.
class ScratchTree
{
  public:
    explicit ScratchTree(std::string_view name):
        _root { std::filesystem::temp_directory_path()
                / std::filesystem::path { std::string { "fc-tcp-" } + std::string { name } } }
    {
        std::error_code ec;
        std::filesystem::remove_all(_root, ec);
        std::filesystem::create_directories(_root, ec);
    }
    ~ScratchTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(_root, ec);
    }
    ScratchTree(ScratchTree const&) = delete;
    ScratchTree& operator=(ScratchTree const&) = delete;
    ScratchTree(ScratchTree&&) = delete;
    ScratchTree& operator=(ScratchTree&&) = delete;

    /// Write `content` to `relative`, creating parent directories.
    void Write(std::string_view relative, std::string_view content) const
    {
        auto const path = _root / std::filesystem::path { std::string { relative } };
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out { path, std::ios::binary };
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    [[nodiscard]] std::string Root() const
    {
        return _root.string();
    }

  private:
    std::filesystem::path _root;
};

[[nodiscard]] bool HasPath(std::vector<ToolchainFile> const& files, std::string_view relative)
{
    return std::ranges::any_of(files, [&](ToolchainFile const& f) { return f.relativePath == relative; });
}
} // namespace

// --- the GNU verbose parser -------------------------------------------------

TEST_CASE("The GNU verbose parser reads only the system search list", "[toolchain-probe]")
{
    auto const paths = ParseGnuIncludeSearchPaths(AppleClangVerbose);

    REQUIRE(paths.size() == 4);
    CHECK(paths[0] == "/Applications/Xcode.app/Contents/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1");
    CHECK(paths[2] == "/Applications/Xcode.app/Contents/Developer/SDKs/MacOSX.sdk/usr/include");

    // Everything outside the markers is noise the parser has to step over. The
    // InstalledDir line in particular IS a plausible-looking absolute path, and a
    // parser that merely looked for indented paths would collect it.
    CHECK(std::ranges::none_of(paths, [](std::string const& p) { return p.contains("/usr/bin"); }));
    CHECK(std::ranges::none_of(paths, [](std::string const& p) { return p.contains("nonexistent"); }));
}

TEST_CASE("A framework directory keeps its path and loses its annotation", "[toolchain-probe]")
{
    // Dropping these would silently narrow the fingerprint on macOS, where the
    // SDK frameworks carry the system headers an Objective-C++ TU reads.
    auto const paths = ParseGnuIncludeSearchPaths(AppleClangVerbose);
    REQUIRE(paths.size() == 4);
    CHECK(paths[3] == "/Applications/Xcode.app/Contents/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks");
    CHECK(!paths[3].contains("framework directory"));
}

TEST_CASE("The quoted-include marker does not open the list", "[toolchain-probe]")
{
    // `#include "..." search starts here:` precedes the real marker and is NOT
    // it. Matching on a prefix or on "search starts here" alone would open the
    // list one line early and, in a driver that lists quoted-include paths,
    // collect the build's own -I directories as if they were toolchain content --
    // which would make the fingerprint depend on the PROJECT.
    constexpr std::string_view Output = R"(#include "..." search starts here:
 /home/dev/project/include
#include <...> search starts here:
 /usr/include/c++/13
End of search list.
)";
    auto const paths = ParseGnuIncludeSearchPaths(Output);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == "/usr/include/c++/13");
}

TEST_CASE("Parsing stops at the end marker", "[toolchain-probe]")
{
    // A driver can echo an inner invocation after its own list. Continuing past
    // the terminator would append paths belonging to a different command line.
    constexpr std::string_view Output = R"(#include <...> search starts here:
 /usr/include
End of search list.
#include <...> search starts here:
 /somewhere/else
End of search list.
)";
    auto const paths = ParseGnuIncludeSearchPaths(Output);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == "/usr/include");
}

TEST_CASE("CRLF line endings do not become part of a path", "[toolchain-probe]")
{
    // Left on, the `\r` rides into the digest AND into every root prefix test,
    // where it fails silently -- a path that does not exist is skipped, so the
    // fingerprint would quietly lose that root.
    constexpr std::string_view Output = "#include <...> search starts here:\r\n C:/tools/include\r\nEnd of search list.\r\n";
    auto const paths = ParseGnuIncludeSearchPaths(Output);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == "C:/tools/include");
}

TEST_CASE("Output with no search list yields nothing rather than garbage", "[toolchain-probe]")
{
    CHECK(ParseGnuIncludeSearchPaths("").empty());
    CHECK(ParseGnuIncludeSearchPaths("clang: error: no input files\n").empty());
}

TEST_CASE("A truncated list returns what it read", "[toolchain-probe]")
{
    // No terminator: a capture cut short, or a driver that died partway. What was
    // collected still identifies the toolchain better than the banner alone, so
    // it is returned rather than discarded.
    constexpr std::string_view Output = "#include <...> search starts here:\n /usr/include\n";
    auto const paths = ParseGnuIncludeSearchPaths(Output);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == "/usr/include");
}

// --- the MSVC environment parser --------------------------------------------

TEST_CASE("INCLUDE splits on semicolons", "[toolchain-probe]")
{
    auto const paths = ParseIncludeEnvironment(R"(C:\VC\include;C:\SDK\ucrt;C:\SDK\um)");
    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == R"(C:\VC\include)");
    CHECK(paths[2] == R"(C:\SDK\um)");
}

TEST_CASE("Empty INCLUDE entries are dropped", "[toolchain-probe]")
{
    // A doubled or trailing separator is ordinary in a value several vcvars
    // invocations have appended to. An empty entry is not a directory, and kept
    // it would resolve to the current working directory -- making the fingerprint
    // depend on where the launcher happened to run.
    auto const paths = ParseIncludeEnvironment(R"(C:\a;;C:\b;)");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == R"(C:\a)");
    CHECK(paths[1] == R"(C:\b)");
}

TEST_CASE("An unset INCLUDE yields no paths", "[toolchain-probe]")
{
    CHECK(ParseIncludeEnvironment("").empty());
    CHECK(ParseIncludeEnvironment(";;;").empty());
}

// --- the filesystem walk ----------------------------------------------------

TEST_CASE("Probing records every file relative to its own root", "[toolchain-probe]")
{
    ScratchTree const tree { "relative" };
    tree.Write("a/x.hpp", "content-x");
    tree.Write("a/nested/y.hpp", "content-y");

    std::vector<std::string> const roots { tree.Root() + "/a" };
    auto const files = ProbeToolchainFiles(roots);

    REQUIRE(files.size() == 2);
    CHECK(HasPath(files, "x.hpp"));
    CHECK(HasPath(files, "nested/y.hpp"));
}

TEST_CASE("The same tree at two prefixes fingerprints identically", "[toolchain-probe]")
{
    // The whole reason paths are relative. Two machines with the same toolchain
    // at different install prefixes must agree, or distribution is disabled
    // between exactly the machines it exists to connect.
    ScratchTree const one { "prefix-one" };
    ScratchTree const two { "prefix-two-deeper/and/deeper" };
    for (auto const* tree: { &one, &two })
    {
        tree->Write("inc/vector", "template <class T> struct vector {};");
        tree->Write("inc/detail/config.h", "#define CONFIG 1");
    }

    std::vector<std::string> const rootsOne { one.Root() + "/inc" };
    std::vector<std::string> const rootsTwo { two.Root() + "/inc" };

    auto const first = ComputeToolchainFingerprint("cc 1.0", ProbeToolchainFiles(rootsOne));
    auto const second = ComputeToolchainFingerprint("cc 1.0", ProbeToolchainFiles(rootsTwo));
    CHECK(first == second);
    CHECK(!first.empty());
}

TEST_CASE("One changed header changes the fingerprint", "[toolchain-probe]")
{
    ScratchTree const tree { "changed" };
    tree.Write("inc/a.hpp", "original");
    std::vector<std::string> const roots { tree.Root() + "/inc" };
    auto const before = ComputeToolchainFingerprint("cc 1.0", ProbeToolchainFiles(roots));

    tree.Write("inc/a.hpp", "edited!");
    auto const after = ComputeToolchainFingerprint("cc 1.0", ProbeToolchainFiles(roots));

    CHECK(before != after);
}

TEST_CASE("A missing search root is skipped, not fatal", "[toolchain-probe]")
{
    // Drivers list paths they WOULD search; `/usr/local/include` is routinely
    // absent. Failing on one would make the fingerprint unavailable on a
    // perfectly ordinary machine.
    ScratchTree const tree { "missing-root" };
    tree.Write("inc/a.hpp", "x");

    std::vector<std::string> const roots { tree.Root() + "/does-not-exist", tree.Root() + "/inc" };
    auto const files = ProbeToolchainFiles(roots);

    REQUIRE(files.size() == 1);
    CHECK(HasPath(files, "a.hpp"));
}

TEST_CASE("Probing nothing yields nothing", "[toolchain-probe]")
{
    CHECK(ProbeToolchainFiles({}).empty());
}

// --- discovery over the driver table ----------------------------------------

namespace
{
/// A runner that replays one scripted result and records what it was asked.
class ScriptedRunner final: public IProcessRunner
{
  public:
    explicit ScriptedRunner(CompileRun result):
        _result { std::move(result) }
    {
    }

    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        _lastArgv.assign(argv.begin(), argv.end());
        ++_calls;
        return _result;
    }
    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        _lastArgv.assign(argv.begin(), argv.end());
        ++_calls;
        return _result;
    }

    /// The argv of the most recent spawn.
    [[nodiscard]] std::vector<std::string> const& LastArgv() const noexcept
    {
        return _lastArgv;
    }

    /// How many times a process was spawned.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    CompileRun _result;
    std::vector<std::string> _lastArgv;
    int _calls { 0 };
};

[[nodiscard]] DriverSpec const& SpecFor(Flavor flavor)
{
    return DriverOf(flavor);
}
} // namespace

TEST_CASE("A GNU driver is asked verbosely, and its stderr is what is read", "[toolchain-probe]")
{
    // The list goes to stderr, not stdout. Reading the wrong stream yields an
    // empty set and silently reduces the fingerprint to the banner -- the same
    // class of silent no-op the launcher already hit once, when a driver moved
    // its /showIncludes notes between streams.
    ScriptedRunner runner { CompileRun {
        .exitCode = 0, .out = "should not be read", .err = std::string { AppleClangVerbose } } };

    auto const paths = DiscoverIncludePaths(runner, "/usr/bin/c++", SpecFor(Flavor::Clang));

    REQUIRE(paths.size() == 4);
    CHECK(paths[0].contains("c++/v1"));
    REQUIRE(runner.LastArgv().size() >= 2);
    CHECK(runner.LastArgv().front() == "/usr/bin/c++");
    CHECK(std::ranges::find(runner.LastArgv(), "-v") != runner.LastArgv().end());
    CHECK(std::ranges::find(runner.LastArgv(), "-E") != runner.LastArgv().end());
}

TEST_CASE("A non-zero exit does not discard a printed search list", "[toolchain-probe]")
{
    // The list is printed before anything that could fail, and drivers exit
    // non-zero for reasons that leave it valid. Gating on the exit code would
    // silently drop the include tree on exactly those toolchains.
    ScriptedRunner runner { CompileRun { .exitCode = 1, .out = {}, .err = std::string { AppleClangVerbose } } };
    CHECK(DiscoverIncludePaths(runner, "cc", SpecFor(Flavor::Clang)).size() == 4);
}

TEST_CASE("An unknown driver is not interrogated at all", "[toolchain-probe]")
{
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = std::string { AppleClangVerbose } } };
    CHECK(DiscoverIncludePaths(runner, "mystery", SpecFor(Flavor::Unknown)).empty());
    CHECK(runner.Calls() == 0);
}

TEST_CASE("An MSVC driver is not spawned to discover its paths", "[toolchain-probe]")
{
    // cl has no such switch; its list comes from the environment. Spawning it
    // would cost a process per launcher invocation and return nothing.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = {} } };
    (void) DiscoverIncludePaths(runner, "cl.exe", SpecFor(Flavor::Cl));
    CHECK(runner.Calls() == 0);
}

// --- the validity stamp ------------------------------------------------------

TEST_CASE("A stamp follows a search root's modification time", "[toolchain-probe]")
{
    // The mtime is SET rather than waited for. Adding a file and re-reading races
    // the filesystem's timestamp granularity -- on a second-granular filesystem
    // the two readings are identical and the test fails for a reason that has
    // nothing to do with the stamp. Setting it states the property directly:
    // whatever the filesystem reports for this root is folded into the stamp.
    ScratchTree const tree { "stamp-root" };
    tree.Write("inc/a.hpp", "x");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = tree.Root() + "/cc";
    auto const includeDir = std::filesystem::path { tree.Root() } / "inc";
    std::vector<std::string> const roots { includeDir.string() };

    std::error_code ec;
    auto const original = std::filesystem::last_write_time(includeDir, ec);
    REQUIRE(!ec);

    auto const before = ComputeToolchainStamp("cc 1.0", compiler, roots);
    REQUIRE(!before.empty());

    std::filesystem::last_write_time(includeDir, original + std::chrono::hours { 1 }, ec);
    REQUIRE(!ec);
    auto const after = ComputeToolchainStamp("cc 1.0", compiler, roots);

    CHECK(before != after);
}

TEST_CASE("A stamp changes when the compiler binary changes size", "[toolchain-probe]")
{
    // Size as well as mtime, because a toolchain restored from an archive can
    // carry its original timestamps -- an upgrade that moves no clock but
    // certainly moves the bytes.
    ScratchTree const tree { "stamp-size" };
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = tree.Root() + "/cc";
    auto const before = ComputeToolchainStamp("cc 1.0", compiler, {});

    tree.Write("cc", "#!/bin/sh\nexec real-cc \"$@\"\n");
    std::error_code ec;
    auto const restored = std::filesystem::last_write_time(std::filesystem::path { compiler }, ec);
    REQUIRE(!ec);
    // Put the clock back, so only the SIZE differs between the two readings.
    std::filesystem::last_write_time(std::filesystem::path { compiler }, restored - std::chrono::hours { 2 }, ec);
    auto const afterSizeOnly = ComputeToolchainStamp("cc 1.0", compiler, {});

    CHECK(before != afterSizeOnly);
}

TEST_CASE("A stamp changes when the banner changes", "[toolchain-probe]")
{
    ScratchTree const tree { "stamp-banner" };
    tree.Write("cc", "#!/bin/sh\n");
    std::vector<std::string> const roots {};
    auto const compiler = tree.Root() + "/cc";

    CHECK(ComputeToolchainStamp("cc 1.0", compiler, roots) != ComputeToolchainStamp("cc 2.0", compiler, roots));
}

TEST_CASE("A compiler that cannot be stat'd yields no stamp", "[toolchain-probe]")
{
    // No stamp means no caching, which costs a rewalk. That is the right trade
    // against a stamp that cannot observe any change and would pin a stale
    // fingerprint forever.
    CHECK(ComputeToolchainStamp("cc 1.0", "/nonexistent/compiler-that-is-not-here", {}).empty());
}

// --- the cache ---------------------------------------------------------------

namespace
{
/// The variable `StateDirectory()` resolves the cache location from.
///
/// The same `#if` StateDirectory itself carries, because these are genuinely
/// different variables per platform rather than one variable spelled two ways.
/// The writing goes through Testing::ScopedEnv, which is the project's one
/// sanctioned way to script the environment.
#if defined(_WIN32)
constexpr char const* StateVariable = "LOCALAPPDATA";
#else
constexpr char const* StateVariable = "XDG_STATE_HOME";
#endif

/// A runner that reports a fixed search list and counts how often it was asked.
class CountingRunner final: public IProcessRunner
{
  public:
    explicit CountingRunner(std::string verbose):
        _verbose { std::move(verbose) }
    {
    }

    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }
    CompileRun RunCaptureSplit(std::span<std::string const> /*argv*/) override
    {
        ++_calls;
        return CompileRun { .exitCode = 0, .out = {}, .err = _verbose };
    }

    /// How many times a process was spawned.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::string _verbose;
    int _calls { 0 };
};

/// A verbose-output blob naming `root` as the one search path.
[[nodiscard]] std::string VerboseNaming(std::string const& root)
{
    return "#include <...> search starts here:\n " + root + "\nEnd of search list.\n";
}
} // namespace

TEST_CASE("A cached fingerprint is reused rather than rewalked", "[toolchain-probe]")
{
    ScratchTree const tree { "cache-hit" };
    tree.Write("inc/a.hpp", "content");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = tree.Root() + "/cc";
    auto const root = (std::filesystem::path { tree.Root() } / "inc").string();

    ScratchTree const state { "cache-hit-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Root() };

    CountingRunner runner { VerboseNaming(root) };
    auto const first = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang));
    auto const second = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang));

    CHECK(!first.empty());
    CHECK(first == second);
    // Discovery still runs each time -- it is one cheap process and its result is
    // what the stamp is computed FROM, so it cannot be cached behind the stamp.
    // What the cache saves is the walk, which is the 288 MB part.
    CHECK(runner.Calls() == 2);
}

TEST_CASE("A changed toolchain invalidates the cached fingerprint", "[toolchain-probe]")
{
    ScratchTree const tree { "cache-invalidate" };
    tree.Write("inc/a.hpp", "original");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = tree.Root() + "/cc";
    auto const includeDir = std::filesystem::path { tree.Root() } / "inc";

    ScratchTree const state { "cache-invalidate-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Root() };

    CountingRunner runner { VerboseNaming(includeDir.string()) };
    auto const before = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang));

    // Change the content AND move the directory clock, which is what a toolchain
    // upgrade does. Content alone would not restamp -- that is the documented
    // blind spot, and asserting it here would pin the wrong behaviour.
    tree.Write("inc/a.hpp", "upgraded");
    std::error_code ec;
    auto const original = std::filesystem::last_write_time(includeDir, ec);
    REQUIRE(!ec);
    std::filesystem::last_write_time(includeDir, original + std::chrono::hours { 1 }, ec);
    REQUIRE(!ec);

    auto const after = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang));
    CHECK(before != after);
}

TEST_CASE("A forced refresh ignores a cached value", "[toolchain-probe]")
{
    // What --print-toolchain-fingerprint relies on: it exists to answer "why did
    // no worker match", and a cached answer cannot tell a genuine difference from
    // a stale entry.
    ScratchTree const tree { "cache-force" };
    tree.Write("inc/a.hpp", "original");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = tree.Root() + "/cc";
    auto const root = (std::filesystem::path { tree.Root() } / "inc").string();

    ScratchTree const state { "cache-force-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Root() };

    CountingRunner runner { VerboseNaming(root) };
    auto const before = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang));

    // Content changed, clock untouched: the stamp cannot see this, so an
    // unforced call would return the stale value.
    tree.Write("inc/a.hpp", "edited in place");
    auto const stale = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang));
    auto const forced = CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang), true);

    CHECK(stale == before);
    CHECK(forced != before);
}

TEST_CASE("No state directory still yields a fingerprint", "[toolchain-probe]")
{
    // A machine with nowhere to persist must still be able to dispatch. Caching
    // is an optimization; the fingerprint is not.
    ScratchTree const tree { "cache-nowhere" };
    tree.Write("inc/a.hpp", "content");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = tree.Root() + "/cc";
    auto const root = (std::filesystem::path { tree.Root() } / "inc").string();

    FastCache::Testing::ScopedEnv const env { StateVariable, "" };
    CountingRunner runner { VerboseNaming(root) };
    CHECK(!CachedToolchainFingerprint(runner, compiler, "cc 1.0", DriverOf(Flavor::Clang)).empty());
}

// --- the compiler banner ------------------------------------------------------

TEST_CASE("A banner falls back to a normalized name, not the spelling", "[toolchain-probe]")
{
    // Only MSVC reaches the fallback -- `cl` has no `--version` -- so this branch
    // decides every MSVC fingerprint. Returning the basename AS SPELLED made the
    // digest depend on how the compiler was named rather than on which compiler it
    // is: a worker configured with `C:\path\cl.exe` and a build invoking bare `cl`
    // computed different fingerprints, and the scheduler matched neither to the
    // other -- "no worker matches this toolchain", on a fleet where both ends were
    // pointed at the same compiler.
    ScriptedRunner refuses { CompileRun { .exitCode = 2, .out = {}, .err = "unknown option" } };

    auto const bare = CompilerBanner(refuses, "cl");
    auto const full = CompilerBanner(refuses, R"(C:\Program Files\MSVC\bin\cl.exe)");
    auto const posix = CompilerBanner(refuses, "/usr/bin/cl");
    auto const shouty = CompilerBanner(refuses, R"(C:\MSVC\CL.EXE)");

    CHECK(bare == "cl");
    CHECK(full == bare);
    CHECK(posix == bare);
    // Case-folded for the same reason ClassifyCompiler folds: "which driver is
    // this" and "what do we call it" must not disagree about CL.EXE.
    CHECK(shouty == bare);
}

TEST_CASE("A working --version wins over the fallback", "[toolchain-probe]")
{
    // The fallback is a last resort, not the normal path. A GNU driver answers
    // --version with real content, and that content -- not its filename -- is what
    // distinguishes two toolchains.
    ScriptedRunner answers { CompileRun { .exitCode = 0, .out = "g++ (GCC) 14.2.0\nCopyright ...", .err = {} } };
    CHECK(CompilerBanner(answers, "/usr/bin/g++") == "g++ (GCC) 14.2.0");
}
