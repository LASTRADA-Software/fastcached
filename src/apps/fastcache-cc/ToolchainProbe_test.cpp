// SPDX-License-Identifier: Apache-2.0
#include "ToolchainHostTestUtils.hpp"
#include "ToolchainProbe.hpp"

#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using namespace FastCache::Cc::Testing;

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

/// Real bare-`cl` combined output, on two MSVC toolsets, exactly as a pipe
/// carries it.
///
/// Captured rather than invented, down to the CRLF, because three separate things
/// under test are properties of the real bytes and not of a plausible-looking
/// string: that the banner is the FIRST line even though `cl` writes it to stderr
/// and the usage text to stdout; that the two toolsets differ in it; and that it
/// ends with a carriage return a key input must not carry.
constexpr std::string_view ClBareBanner1451 = "Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36252 for x64\r\n"
                                              "Copyright (C) Microsoft Corporation.  All rights reserved.\r\n"
                                              "\r\n"
                                              "usage: cl [ option... ] filename... [ /link linkoption... ]\r\n";

constexpr std::string_view ClBareBanner1444 = "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35228 for x64\r\n"
                                              "Copyright (C) Microsoft Corporation.  All rights reserved.\r\n"
                                              "\r\n"
                                              "usage: cl [ option... ] filename... [ /link linkoption... ]\r\n";

/// Real `cl --version` combined output: a REFUSAL that still names the compiler.
///
/// Kept because it is the shape of the defect. `RunCaptureCombined` merges both
/// streams into `out`, so the banner IS in the buffer -- what discarded it was the
/// exit code, and the launcher no longer asks `cl` this way at all. The test below
/// pins that it does not.
constexpr std::string_view ClVersionRefusal =
    R"(Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207 for x64
Copyright (C) Microsoft Corporation.  All rights reserved.

cl : Command line warning D9002 : ignoring unknown option '--version'
cl : Command line error D8003 : missing source filename
)";

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
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-relative" };
    tree.Write("a/x.hpp", "content-x");
    tree.Write("a/nested/y.hpp", "content-y");

    std::vector<std::string> const roots { (tree / "a").string() };
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
    FastCache::Testing::ScratchDirectory one { "fc-tcp-prefix-one" };
    FastCache::Testing::ScratchDirectory two { "fc-tcp-prefix-two" };

    // The second root sits DELIBERATELY deeper. A prefix that differed only in its
    // last component would still pass if the code folded absolute paths on a
    // machine whose temp directory names happened to be the same length.
    constexpr std::string_view deeper = "and/deeper/still";

    one.Write("inc/vector", "template <class T> struct vector {};");
    one.Write("inc/detail/config.h", "#define CONFIG 1");
    two.Write(std::string { deeper } + "/inc/vector", "template <class T> struct vector {};");
    two.Write(std::string { deeper } + "/inc/detail/config.h", "#define CONFIG 1");

    std::vector<std::string> const rootsOne { (one / "inc").string() };
    std::vector<std::string> const rootsTwo { (two / (std::string { deeper } + "/inc")).string() };

    auto const first = ComputeToolchainFingerprint("cc 1.0", ProbeToolchainFiles(rootsOne));
    auto const second = ComputeToolchainFingerprint("cc 1.0", ProbeToolchainFiles(rootsTwo));
    CHECK(first == second);
    CHECK(!first.empty());
}

TEST_CASE("One changed header changes the fingerprint", "[toolchain-probe]")
{
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-changed" };
    tree.Write("inc/a.hpp", "original");
    std::vector<std::string> const roots { (tree / "inc").string() };
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
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-missing-root" };
    tree.Write("inc/a.hpp", "x");

    std::vector<std::string> const roots { (tree / "does-not-exist").string(), (tree / "inc").string() };
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
    ScriptedToolchainHost host;

    auto const paths = DiscoverIncludePaths(runner, host, "/usr/bin/c++", SpecFor(Flavor::Clang));

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
    ScriptedToolchainHost host;
    CHECK(DiscoverIncludePaths(runner, host, "cc", SpecFor(Flavor::Clang)).size() == 4);
}

TEST_CASE("An unknown driver is not interrogated at all", "[toolchain-probe]")
{
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = std::string { AppleClangVerbose } } };
    ScriptedToolchainHost host;
    CHECK(DiscoverIncludePaths(runner, host, "mystery", SpecFor(Flavor::Unknown)).empty());
    CHECK(runner.Calls() == 0);
}

TEST_CASE("An MSVC driver is not spawned to discover its paths", "[toolchain-probe]")
{
    // cl has no such switch; its list comes from the environment. Spawning it
    // would cost a process per launcher invocation and return nothing.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = {} } };
    ScriptedToolchainHost host;
    (void) DiscoverIncludePaths(runner, host, "cl.exe", SpecFor(Flavor::Cl));
    CHECK(runner.Calls() == 0);
}

// --- the MSVC install layout -------------------------------------------------
//
// Every case here runs on EVERY platform, against a scripted machine. That is the
// whole reason the host is a seam: a Visual Studio install and a Windows SDK
// cannot exist on the Linux and macOS runners that make up most of this project's
// CI, so a layout tested only where it can be installed is a layout tested in one
// job out of six.

namespace
{
/// A scripted machine carrying one Visual Studio install and one Windows SDK,
/// laid out exactly as a real one is.
///
/// Described in place rather than returned, because `IToolchainHost` deletes its
/// copy and move constructors on purpose -- a seam is held by reference, never
/// passed around by value.
///
/// Written once and reused, so a case that changes the shape says which part it
/// changed rather than restating the whole machine. The version numbers are the
/// ones from the machine this was written on -- real values, because the parsing
/// they exercise (a four-component SDK version, a three-component toolset) is
/// where the interesting failures are.
///
/// @param host The scripted machine to describe.
void DescribeWindowsMachine(ScriptedToolchainHost& host)
{
    constexpr std::string_view vs = "C:/Program Files/Microsoft Visual Studio/18/Community";
    constexpr std::string_view toolset = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231";
    constexpr std::string_view kits = "C:/Program Files (x86)/Windows Kits/10";

    host.AddExecutable(std::string { toolset } + "/bin/Hostx64/x64/cl.exe");
    host.AddDirectory(std::string { toolset } + "/include");
    host.AddDirectory(std::string { toolset } + "/atlmfc/include");
    host.AddDirectory(std::string { vs } + "/VC/Auxiliary/VS/include");
    host.AddFile(std::string { vs } + "/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt", "14.51.36231\n");

    // The trailing separator is what `KitsRoot10` really contains.
    host.AddRegistryValue(RegistryHive::LocalMachine,
                          R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)",
                          "KitsRoot10",
                          std::string { kits } + "/",
                          RegistryView::ThirtyTwoBit);
    for (auto const& subdirectory: { "ucrt", "um", "shared", "winrt", "cppwinrt" })
        host.AddDirectory(std::string { kits } + "/Include/10.0.26100.0/" + subdirectory);
    host.AddDirectory(std::string { kits } + "/Include/10.0.22621.0/ucrt");

    host.SetSearchPath({ std::string { toolset } + "/bin/Hostx64/x64" });
}

/// A machine with LLVM installed, described the way `DescribeWindowsMachine` is.
///
/// Only the half `clang-cl`'s identity is derived from: the resource directory
/// that ships beside the driver. The driver's own ANSWER is the other half, and it
/// belongs to the runner rather than to the filesystem -- `ResourceDirRun` below.
///
/// The search path is APPENDED rather than set, so composing this with
/// `DescribeWindowsMachine` leaves `cl` resolvable. A fixture that quietly
/// unresolved another fixture's compiler would make a case that looks like it
/// covers both compilers cover one.
///
/// @param host The scripted machine to describe.
/// @param prefix Where LLVM is installed.
/// @param version The resource directory's version, as clang spells it.
void DescribeLlvmInstall(ScriptedToolchainHost& host, std::string_view prefix, std::string_view version)
{
    host.AddExecutable(std::string { prefix } + "/bin/clang-cl.exe");
    host.AddDirectory(std::string { prefix } + "/lib/clang/" + std::string { version } + "/include");
    host.AppendSearchPath(std::string { prefix } + "/bin");
}

/// What `clang-cl -print-resource-dir` prints on such a machine.
///
/// With the trailing newline a real driver emits, on stdout: the parsing that has
/// to survive it is the reason this is not written as a bare path.
///
/// @param prefix Where LLVM is installed.
/// @param version The resource directory's version.
/// @return A scripted run for `ScriptedRunner`.
[[nodiscard]] CompileRun ResourceDirRun(std::string_view prefix, std::string_view version)
{
    return CompileRun { .exitCode = 0,
                        .out = std::string { prefix } + "/lib/clang/" + std::string { version } + "\n",
                        .err = {} };
}
} // namespace

TEST_CASE("An MSVC toolchain's roots come from its own install layout", "[toolchain-probe]")
{
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);

    auto const roots = MsvcToolsetIncludeRoots(host,
                                               "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/"
                                               "MSVC/14.51.36231/bin/Hostx64/x64/cl.exe");

    // The same three directories, in the same order, that `vcvarsall` puts at the
    // front of `INCLUDE` -- checked against a real developer prompt.
    CHECK(roots
          == std::vector<std::string> {
              "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include",
              "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/atlmfc/include",
              "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/VS/include",
          });
}

TEST_CASE("A bare cl and its absolute path derive the same roots", "[toolchain-probe]")
{
    // The rule the whole mechanism turns on. A build system invokes `cl` while a
    // worker is configured with the full path; if those derived different roots
    // they would derive different fingerprints, and a fingerprint disagreement is
    // invisible from both ends -- the scheduler simply never matches.
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);

    auto const bare = MsvcToolsetIncludeRoots(host, "cl");
    auto const absolute = MsvcToolsetIncludeRoots(host,
                                                  "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/"
                                                  "MSVC/14.51.36231/bin/Hostx64/x64/cl.exe");

    CHECK_FALSE(bare.empty());
    CHECK(bare == absolute);
}

TEST_CASE("A cross-targeting toolset is found at its own depth", "[toolchain-probe]")
{
    // `bin/Hostx64/arm64` is the same depth as `x64` today and nothing promises it
    // stays that way, which is why the walk looks for the `MSVC/<version>` pair
    // rather than counting levels.
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);
    constexpr std::string_view toolset = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231";
    host.AddExecutable(std::string { toolset } + "/bin/Hostx64/arm64/cl.exe");

    auto const roots = MsvcToolsetIncludeRoots(host, std::string { toolset } + "/bin/Hostx64/arm64/cl.exe");
    REQUIRE_FALSE(roots.empty());
    CHECK(roots.front() == std::string { toolset } + "/include");
}

TEST_CASE("A directory absent from the layout is not offered as a root", "[toolchain-probe]")
{
    // A toolchain without ATL/MFC installed is ordinary. `ProbeToolchainFiles`
    // would skip the root anyway; emitting it only makes the list harder to read
    // beside an operator's own INCLUDE when a fingerprint disagrees.
    ScriptedToolchainHost host;
    constexpr std::string_view toolset = "C:/VS/VC/Tools/MSVC/14.0.0";
    host.AddExecutable(std::string { toolset } + "/bin/Hostx64/x64/cl.exe");
    host.AddDirectory(std::string { toolset } + "/include");

    CHECK(MsvcToolsetIncludeRoots(host, std::string { toolset } + "/bin/Hostx64/x64/cl.exe")
          == std::vector<std::string> { std::string { toolset } + "/include" });
}

TEST_CASE("A compiler outside any MSVC layout derives no roots", "[toolchain-probe]")
{
    ScriptedToolchainHost host;
    host.AddExecutable("C:/wrappers/cl.exe");
    host.AddDirectory("C:/wrappers/include");

    CHECK(MsvcToolsetIncludeRoots(host, "C:/wrappers/cl.exe").empty());
    CHECK(MsvcToolsetIncludeRoots(host, "C:/not/installed/cl.exe").empty());
}

TEST_CASE("The Windows SDK's roots come from the registry and the newest kit", "[toolchain-probe]")
{
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);

    CHECK(WindowsKitIncludeRoots(host)
          == std::vector<std::string> {
              "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt",
              "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um",
              "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared",
              "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/winrt",
              "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt",
          });
}

TEST_CASE("SDK versions are ordered numerically, not as text", "[toolchain-probe]")
{
    // `10.0.9` sorts ABOVE `10.0.22621.0` as text, so a string compare picks a kit
    // years out of date and fingerprints headers the compiler will never open.
    ScriptedToolchainHost host;
    host.AddRegistryValue(RegistryHive::LocalMachine,
                          R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)",
                          "KitsRoot10",
                          "C:/Kits/10/",
                          RegistryView::ThirtyTwoBit);
    host.AddDirectory("C:/Kits/10/Include/10.0.9/ucrt");
    host.AddDirectory("C:/Kits/10/Include/10.0.22621.0/ucrt");

    CHECK(WindowsKitIncludeRoots(host) == std::vector<std::string> { "C:/Kits/10/Include/10.0.22621.0/ucrt" });
}

TEST_CASE("A kit named only in the registry still counts", "[toolchain-probe]")
{
    // Some machines record each installed kit as a version-named value under
    // `Installed Roots`; the one this was written on records `KitsRoot10` and two
    // hundred GUIDs and nothing else. Both sources are read because neither
    // answers everywhere -- and the GUIDs must not be mistaken for versions.
    ScriptedToolchainHost host;
    constexpr std::string_view roots = R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)";
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "KitsRoot10", "C:/Kits/10/", RegistryView::ThirtyTwoBit);
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "10.0.26100.0", "1", RegistryView::ThirtyTwoBit);
    host.AddRegistryValue(
        RegistryHive::LocalMachine, roots, "{EC4535F2-0CE9-1B42-8E73-B32BCDE21496}", "1", RegistryView::ThirtyTwoBit);
    host.AddDirectory("C:/Kits/10/Include/10.0.26100.0/um");

    CHECK(WindowsKitIncludeRoots(host) == std::vector<std::string> { "C:/Kits/10/Include/10.0.26100.0/um" });
}

TEST_CASE("A registry version with no headers on disk is not chosen", "[toolchain-probe]")
{
    // An uninstall that left the value behind, or a kit installed without its
    // headers. Choosing it would fingerprint nothing at all while reporting that
    // the layout was found.
    ScriptedToolchainHost host;
    constexpr std::string_view roots = R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)";
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "KitsRoot10", "C:/Kits/10/", RegistryView::ThirtyTwoBit);
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "99.0.0.0", "1", RegistryView::ThirtyTwoBit);
    host.AddDirectory("C:/Kits/10/Include/10.0.26100.0/um");

    CHECK(WindowsKitIncludeRoots(host) == std::vector<std::string> { "C:/Kits/10/Include/10.0.26100.0/um" });
}

TEST_CASE("An SDK recorded only in the native view is still found", "[toolchain-probe]")
{
    // `Installed Roots` is written by a 32-bit installer and normally lands in
    // WOW6432Node, but a native-only record must not make the kit invisible.
    ScriptedToolchainHost host;
    host.AddRegistryValue(RegistryHive::LocalMachine,
                          R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)",
                          "KitsRoot10",
                          "C:/Kits/10/",
                          RegistryView::Native);
    host.AddDirectory("C:/Kits/10/Include/10.0.26100.0/ucrt");

    CHECK(WindowsKitIncludeRoots(host) == std::vector<std::string> { "C:/Kits/10/Include/10.0.26100.0/ucrt" });
}

TEST_CASE("A machine with no SDK registered reports no kit", "[toolchain-probe]")
{
    ScriptedToolchainHost host;
    CHECK(WindowsKitIncludeRoots(host).empty());
}

TEST_CASE("An empty 32-bit KitsRoot10 still reaches the native view", "[toolchain-probe]")
{
    // Present and EMPTY is a third answer, and the fallback has to treat it as
    // absent. `ReadRegistryString` returns a zero-length value as an empty string
    // rather than as `nullopt` -- which is what a partial or rolled-back SDK
    // uninstall leaves in `WOW6432Node` -- so a fallback gated on `has_value()`
    // alone never read the native view, and the machine's `cl` lost the whole SDK
    // half of its identity while the VC half kept `MsvcLayout` off its `INCLUDE`
    // fallback too. It then fingerprinted differently from an identically
    // toolchained peer, with no diagnostic at either end.
    ScriptedToolchainHost host;
    constexpr std::string_view roots = R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)";
    constexpr std::string_view kits = "C:/Program Files (x86)/Windows Kits/10";

    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "KitsRoot10", "", RegistryView::ThirtyTwoBit);
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "KitsRoot10", std::string { kits } + "/", RegistryView::Native);
    host.AddDirectory(std::string { kits } + "/Include/10.0.26100.0/ucrt");

    CHECK(WindowsKitIncludeRoots(host) == std::vector<std::string> { std::string { kits } + "/Include/10.0.26100.0/ucrt" });
}

TEST_CASE("An MSVC service and a developer prompt derive the same roots", "[toolchain-probe]")
{
    // The defect this whole mechanism exists for. A Windows service inherits no
    // `INCLUDE`, so a worker started as a service walked no include tree and
    // fingerprinted on its banner alone -- which, before issue #195, was the string
    // `cl` on every MSVC toolset in existence. Two nodes on different toolsets were
    // interchangeable to the scheduler, which is the false match the fingerprint
    // exists to prevent and the one that yields a silently wrong object. The banner
    // names the toolset now; the roots are still what make the digest sensitive to
    // a patched header, which no banner is.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = {} } };

    ScriptedToolchainHost service;
    DescribeWindowsMachine(service);
    ScriptedToolchainHost developerPrompt;
    DescribeWindowsMachine(developerPrompt);
    developerPrompt.SetEnvironment("INCLUDE", "C:/somewhere/else;C:/and/another");

    auto const underService = DiscoverIncludePaths(runner, service, "cl", SpecFor(Flavor::Cl));
    auto const underPrompt = DiscoverIncludePaths(runner, developerPrompt, "cl", SpecFor(Flavor::Cl));

    CHECK_FALSE(underService.empty());
    CHECK(underService == underPrompt);

    // And the layout, not the variable, is what answered -- the ordering that
    // keeps the two ends agreeing wherever the layout is derivable at all.
    CHECK(std::ranges::find(underPrompt, "C:/somewhere/else") == underPrompt.end());
}

TEST_CASE("Two MSVC toolsets do not fingerprint identically", "[toolchain-probe]")
{
    // The other half of the guarantee, and the half nothing asserted. The case
    // above pins that one toolchain reaches ONE digest however it was reached;
    // this pins that two toolchains reach TWO -- "never two different ones",
    // which is precisely what a banner-only MSVC fingerprint dropped and what the
    // layout exists to restore. It was evidenced only by a measurement on one
    // machine, so nothing would have caught its regression.
    //
    // Driven through the layout walk rather than the launcher's whole path, which
    // the case above already covers. Each link here is tested on its own too --
    // the walk yields per-toolset roots, the digest is content-sensitive -- so a
    // reader had to compose them to believe the property the fleet depends on.
    // Asserting it in one place is the point, not extra coverage of the links.
    FastCache::Testing::ScratchDirectory machine { "fc-tcp-two-toolsets" };
    ScriptedToolchainHost host;

    // Real files, because `ProbeToolchainFiles` walks the filesystem rather than
    // the scripted machine -- the scripted half only has to agree with it.
    auto const describe = [&](std::string_view version, std::string_view stlUpdate) {
        auto const toolset = "vs/VC/Tools/MSVC/" + std::string { version };
        machine.Write(toolset + "/include/yvals_core.h", stlUpdate);
        machine.Write(toolset + "/include/vector", "template <class T> struct vector {};");

        auto const compiler = (machine / (toolset + "/bin/Hostx64/x64/cl.exe")).generic_string();
        host.AddExecutable(compiler);
        host.AddDirectory((machine / (toolset + "/include")).generic_string());
        return compiler;
    };

    // Two toolsets differ in what their headers SAY -- `yvals_core.h` carries
    // `_MSVC_STL_UPDATE` -- and that is what has to separate them, not the version
    // in the path: `ProbeToolchainFiles` records every file relative to its own
    // root, so that one toolchain at two install prefixes still matches.
    auto const older = describe("14.29.30133", "#define _MSVC_STL_UPDATE 202008L\n");
    auto const newer = describe("14.51.36231", "#define _MSVC_STL_UPDATE 202506L\n");

    auto const digestOf = [&](std::string const& compiler) {
        auto const roots = MsvcToolsetIncludeRoots(host, compiler);
        REQUIRE_FALSE(roots.empty());
        auto const files = ProbeToolchainFiles(roots);
        // Not vacuous: two empty walks would digest equal and the check below would
        // pass having compared nothing.
        REQUIRE(files.size() == 2);
        // "cl" for both, which is what the banner fallback gives every MSVC
        // toolchain -- so the roots are the only thing that can tell these apart.
        return ComputeToolchainFingerprint("cl", files);
    };

    CHECK(digestOf(older) != digestOf(newer));
}

TEST_CASE("INCLUDE is the fallback when no toolset layout can be determined", "[toolchain-probe]")
{
    // A wrapper named cl.exe outside any VC layout -- ON A MACHINE THAT HAS AN SDK,
    // which is the whole point of the case. `WindowsKitIncludeRoots` answers from
    // the registry alone and knows nothing about which compiler is being
    // identified, so gating the fallback on the MERGED roots would leave such a
    // compiler with SDK roots only: no VC headers in its identity, and two
    // different wrapped toolsets digesting identically. That is the false match the
    // whole mechanism exists to prevent, reached by the fix for it.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = {} } };
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);
    host.AddExecutable("C:/wrappers/cl.exe");
    host.SetEnvironment("INCLUDE", "C:/legacy/include;C:/legacy/atl");

    REQUIRE_FALSE(WindowsKitIncludeRoots(host).empty());
    CHECK(DiscoverIncludePaths(runner, host, "C:/wrappers/cl.exe", SpecFor(Flavor::Cl))
          == std::vector<std::string> { "C:/legacy/include", "C:/legacy/atl" });
    CHECK(runner.Calls() == 0);
}

TEST_CASE("A toolset with no SDK keeps its VC roots rather than falling back", "[toolchain-probe]")
{
    // The other half of the gate. A partial layout answer is kept, because both
    // ends of a dispatch run this code and so reach the same partial answer --
    // whereas topping it up from a variable only one of them has is how the two
    // stop agreeing.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = {} } };
    ScriptedToolchainHost host;
    constexpr std::string_view toolset = "C:/VS/VC/Tools/MSVC/14.0.0";
    host.AddExecutable(std::string { toolset } + "/bin/Hostx64/x64/cl.exe");
    host.AddDirectory(std::string { toolset } + "/include");
    host.SetEnvironment("INCLUDE", "C:/should/not/be/used");

    CHECK(DiscoverIncludePaths(runner, host, std::string { toolset } + "/bin/Hostx64/x64/cl.exe", SpecFor(Flavor::Cl))
          == std::vector<std::string> { std::string { toolset } + "/include" });
}

TEST_CASE("clang-cl is asked where its own headers are", "[toolchain-probe]")
{
    ScriptedRunner runner { ResourceDirRun("C:/Program Files/LLVM", "21") };
    ScriptedToolchainHost host;
    DescribeLlvmInstall(host, "C:/Program Files/LLVM", "21");

    CHECK(ClangResourceIncludeRoots(runner, host, "C:/Program Files/LLVM/bin/clang-cl.exe")
          == std::vector<std::string> { "C:/Program Files/LLVM/lib/clang/21/include" });

    REQUIRE(runner.LastArgv().size() == 2);
    CHECK(runner.LastArgv().front() == "C:/Program Files/LLVM/bin/clang-cl.exe");
    CHECK(runner.LastArgv().back() == "-print-resource-dir");
}

TEST_CASE("The driver's answer wins over anything derivable from its path", "[toolchain-probe]")
{
    // The case that made asking non-negotiable, and it is not hypothetical: this is
    // an ordinary Debian, where `/usr/lib/clang` holds `20`, `20.1.2`, `22` and
    // `22.1.8` while `/usr/bin/clang-cl-20` is one of two drivers sharing the `/usr`
    // prefix. Walking up from the driver reaches `/usr` for both of them, and no
    // rule over those four names picks the right one -- "the newest" hands a clang
    // 20 driver the headers of clang 22. The driver knows, and answers.
    ScriptedRunner runner { ResourceDirRun("/usr/lib/llvm-20", "20") };
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/bin/clang-cl-20");
    host.AddDirectory("/usr/lib/llvm-20/lib/clang/20/include");
    for (auto const& stale: { "20", "20.1.2", "22", "22.1.8" })
        host.AddDirectory(std::string { "/usr/lib/clang/" } + stale + "/include");

    CHECK(ClangResourceIncludeRoots(runner, host, "/usr/bin/clang-cl-20")
          == std::vector<std::string> { "/usr/lib/llvm-20/lib/clang/20/include" });
}

TEST_CASE("A resource directory with no include beneath it is not a root", "[toolchain-probe]")
{
    // A partial install must yield NOTHING rather than the directory the driver
    // named: a root only one of the two ends can see is the disagreement this
    // mechanism exists to remove.
    ScriptedRunner runner { ResourceDirRun("C:/LLVM", "21") };
    ScriptedToolchainHost host;
    host.AddExecutable("C:/LLVM/bin/clang-cl.exe");
    host.AddDirectory("C:/LLVM/lib/clang/21");

    CHECK(ClangResourceIncludeRoots(runner, host, "C:/LLVM/bin/clang-cl.exe").empty());
}

TEST_CASE("A driver that does not understand the flag names no root", "[toolchain-probe]")
{
    // A wrapper called `clang-cl.exe` that is not clang. Its diagnostic is not a
    // path that exists, and that is what rejects it -- the exit code is not
    // consulted, for the same reason the GNU arm does not consult it.
    ScriptedRunner runner { CompileRun { .exitCode = 1, .out = "error: unknown argument\n", .err = {} } };
    ScriptedToolchainHost host;
    host.AddExecutable("C:/wrappers/clang-cl.exe");

    CHECK(ClangResourceIncludeRoots(runner, host, "C:/wrappers/clang-cl.exe").empty());
}

TEST_CASE("A resource directory answered with CRLF is still a path", "[toolchain-probe]")
{
    // Windows is where this driver mostly runs, so a `\r` is the ordinary case
    // rather than the exotic one. Left on, it becomes part of the path and the
    // directory test fails -- silently, since a root that does not exist is skipped.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = "C:/LLVM/lib/clang/21\r\n", .err = {} } };
    ScriptedToolchainHost host;
    host.AddDirectory("C:/LLVM/lib/clang/21/include");

    CHECK(ClangResourceIncludeRoots(runner, host, "C:/LLVM/bin/clang-cl.exe")
          == std::vector<std::string> { "C:/LLVM/lib/clang/21/include" });
}

TEST_CASE("A clang-cl service and a developer prompt derive the same roots", "[toolchain-probe]")
{
    // The defect this mechanism exists for, and the shape #140 fixed for `cl`
    // reached by a second route. A Windows service inherits no `INCLUDE`, so a
    // worker started as one fingerprinted `clang-cl` over the banner alone while a
    // launcher in a developer prompt fingerprinted it over the whole MSVC include
    // tree. The two never agreed, the scheduler answered `NoWorker`, and nothing at
    // either end said why.
    ScriptedRunner serviceRunner { ResourceDirRun("C:/Program Files/LLVM", "21") };
    ScriptedRunner promptRunner { ResourceDirRun("C:/Program Files/LLVM", "21") };

    ScriptedToolchainHost service;
    DescribeWindowsMachine(service);
    DescribeLlvmInstall(service, "C:/Program Files/LLVM", "21");
    ScriptedToolchainHost developerPrompt;
    DescribeWindowsMachine(developerPrompt);
    DescribeLlvmInstall(developerPrompt, "C:/Program Files/LLVM", "21");
    developerPrompt.SetEnvironment("INCLUDE", "C:/somewhere/else;C:/and/another");

    auto const underService = DiscoverIncludePaths(serviceRunner, service, "clang-cl", SpecFor(Flavor::ClangCl));
    auto const underPrompt = DiscoverIncludePaths(promptRunner, developerPrompt, "clang-cl", SpecFor(Flavor::ClangCl));

    CHECK_FALSE(underService.empty());
    CHECK(underService == underPrompt);
    CHECK(std::ranges::find(underPrompt, "C:/somewhere/else") == underPrompt.end());
}

TEST_CASE("An undiscoverable clang-cl layout does not fall back to INCLUDE", "[toolchain-probe]")
{
    // Where this mechanism deliberately parts company with `MsvcLayout`. That one
    // falls back to `INCLUDE` because a `cl` outside the VC layout would otherwise
    // be left with a degenerate identity -- its banner is the constant `cl`, so the
    // headers are the only identity it has. `clang-cl` announces a genuine version,
    // so a driver that cannot answer degrades to a banner-only fingerprint, which is
    // weaker but still tells one clang from another. Reading `INCLUDE` here would
    // buy that wrapper nothing and reintroduce the very asymmetry this fixes: a
    // service and a developer prompt disagreeing about a compiler neither can place.
    ScriptedRunner runner { CompileRun { .exitCode = 1, .out = {}, .err = {} } };
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);
    host.AddExecutable("C:/wrappers/clang-cl.exe");
    host.SetEnvironment("INCLUDE", "C:/from/the/prompt");

    CHECK(DiscoverIncludePaths(runner, host, "C:/wrappers/clang-cl.exe", SpecFor(Flavor::ClangCl)).empty());
}

TEST_CASE("An MSVC layout on the machine contributes nothing to clang-cl", "[toolchain-probe]")
{
    // clang-cl does not own the VC toolset or the SDK -- it borrows whichever the
    // machine has -- and the worker compiles text the CLIENT already preprocessed,
    // so those headers are never read on the far end. Folding them in would add no
    // discrimination and plenty of mismatch: `WindowsKitIncludeRoots` picks the
    // newest kit ON THE MACHINE, so two boxes running the same clang-cl with
    // different SDKs installed would quietly stop matching each other.
    ScriptedRunner runner { ResourceDirRun("C:/Program Files/LLVM", "21") };
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);
    DescribeLlvmInstall(host, "C:/Program Files/LLVM", "21");

    REQUIRE_FALSE(WindowsKitIncludeRoots(host).empty());
    REQUIRE_FALSE(MsvcToolsetIncludeRoots(host, "cl").empty());
    CHECK(DiscoverIncludePaths(runner, host, "clang-cl", SpecFor(Flavor::ClangCl))
          == std::vector<std::string> { "C:/Program Files/LLVM/lib/clang/21/include" });
}

// --- the validity stamp ------------------------------------------------------

TEST_CASE("A stamp follows a search root's modification time", "[toolchain-probe]")
{
    // The mtime is SET rather than waited for. Adding a file and re-reading races
    // the filesystem's timestamp granularity -- on a second-granular filesystem
    // the two readings are identical and the test fails for a reason that has
    // nothing to do with the stamp. Setting it states the property directly:
    // whatever the filesystem reports for this root is folded into the stamp.
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-stamp-root" };
    tree.Write("inc/a.hpp", "x");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
    auto const includeDir = std::filesystem::path { tree.Path().string() } / "inc";
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
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-stamp-size" };
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
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
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-stamp-banner" };
    tree.Write("cc", "#!/bin/sh\n");
    std::vector<std::string> const roots {};
    auto const compiler = (tree / "cc").string();

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
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-cache-hit" };
    tree.Write("inc/a.hpp", "content");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
    auto const root = (std::filesystem::path { tree.Path().string() } / "inc").string();

    FastCache::Testing::ScratchDirectory state { "fc-tcp-cache-hit-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Path().string() };

    CountingRunner runner { VerboseNaming(root) };
    ScriptedToolchainHost host;
    auto const first = CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;
    auto const second = CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;

    CHECK(!first.empty());
    CHECK(first == second);
    // Discovery still runs each time -- it is one cheap process and its result is
    // what the stamp is computed FROM, so it cannot be cached behind the stamp.
    // What the cache saves is the walk, which is the 288 MB part.
    CHECK(runner.Calls() == 2);
}

TEST_CASE("A compiler invoked by bare name still caches its fingerprint", "[toolchain-probe]")
{
    // The stamp stats the compiler binary, and a BARE name cannot be stat'd from an
    // arbitrary working directory -- so it produced no stamp, nothing was ever
    // cached, and the multi-second include-tree walk ran again for every single
    // translation unit. Resolving the name for the stamp and the cache file also
    // gives the two spellings of one compiler ONE cache entry rather than two whose
    // contents are identical and each of which the other misses.
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-cache-bare" };
    tree.Write("inc/a.hpp", "content");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
    auto const root = (std::filesystem::path { tree.Path().string() } / "inc").string();

    FastCache::Testing::ScratchDirectory state { "fc-tcp-cache-bare-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Path().string() };

    // The scripted host is what turns `cc` into that path, exactly as a real PATH
    // lookup would; the real filesystem underneath is what the stamp then stats.
    CountingRunner runner { VerboseNaming(root) };
    ScriptedToolchainHost host;
    host.AddExecutable(compiler);
    host.SetSearchPath({ tree.Path().string() });

    // The full path is spelled the way the search-path lookup returns it. Two
    // SEPARATOR spellings of one location still key apart, here and in production
    // -- `ResolveOnSearchPath` hands a path back verbatim -- and that is a separate,
    // pre-existing concern that `IPathResolver` exists for (issue #66). Mixing it in
    // would leave this case unable to say which of the two effects it had caught.
    auto const fullPath = ScriptedToolchainHost::Normalize(compiler);

    auto const viaBareName = CachedToolchainFingerprint(runner, host, "cc", "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;
    auto const viaFullPath =
        CachedToolchainFingerprint(runner, host, fullPath, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;

    CHECK_FALSE(viaBareName.empty());
    CHECK(viaBareName == viaFullPath);

    // ONE cache file for both spellings, not two -- the half of this that a
    // matching fingerprint alone would not have caught, since `cc` and its absolute
    // path used to key different entries that each missed the other.
    std::error_code ec;
    std::size_t entries = 0;
    auto const cacheDirectory = std::filesystem::path { state.Path().string() } / "fastcache-cc" / "toolchains";
    for (auto const& entry: std::filesystem::directory_iterator { cacheDirectory, ec })
        if (entry.path().extension() == ".fingerprint")
            ++entries;
    REQUIRE_FALSE(ec);
    CHECK(entries == 1);
}

TEST_CASE("A changed toolchain invalidates the cached fingerprint", "[toolchain-probe]")
{
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-cache-invalidate" };
    tree.Write("inc/a.hpp", "original");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
    auto const includeDir = std::filesystem::path { tree.Path().string() } / "inc";

    FastCache::Testing::ScratchDirectory state { "fc-tcp-cache-invalidate-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Path().string() };

    CountingRunner runner { VerboseNaming(includeDir.string()) };
    ScriptedToolchainHost host;
    auto const before = CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;

    // Change the content AND move the directory clock, which is what a toolchain
    // upgrade does. Content alone would not restamp -- that is the documented
    // blind spot, and asserting it here would pin the wrong behaviour.
    tree.Write("inc/a.hpp", "upgraded");
    std::error_code ec;
    auto const original = std::filesystem::last_write_time(includeDir, ec);
    REQUIRE(!ec);
    std::filesystem::last_write_time(includeDir, original + std::chrono::hours { 1 }, ec);
    REQUIRE(!ec);

    auto const after = CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;
    CHECK(before != after);
}

TEST_CASE("A forced refresh ignores a cached value", "[toolchain-probe]")
{
    // What --print-toolchain-fingerprint relies on: it exists to answer "why did
    // no worker match", and a cached answer cannot tell a genuine difference from
    // a stale entry.
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-cache-force" };
    tree.Write("inc/a.hpp", "original");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
    auto const root = (std::filesystem::path { tree.Path().string() } / "inc").string();

    FastCache::Testing::ScratchDirectory state { "fc-tcp-cache-force-state" };
    FastCache::Testing::ScopedEnv const env { StateVariable, state.Path().string() };

    CountingRunner runner { VerboseNaming(root) };
    ScriptedToolchainHost host;
    auto const before = CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;

    // Content changed, clock untouched: the stamp cannot see this, so an
    // unforced call would return the stale value.
    tree.Write("inc/a.hpp", "edited in place");
    auto const stale = CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint;
    auto const forced =
        CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang), true).fingerprint;

    CHECK(stale == before);
    CHECK(forced != before);
}

TEST_CASE("No state directory still yields a fingerprint", "[toolchain-probe]")
{
    // A machine with nowhere to persist must still be able to dispatch. Caching
    // is an optimization; the fingerprint is not.
    FastCache::Testing::ScratchDirectory tree { "fc-tcp-cache-nowhere" };
    tree.Write("inc/a.hpp", "content");
    tree.Write("cc", "#!/bin/sh\n");
    auto const compiler = (tree / "cc").string();
    auto const root = (std::filesystem::path { tree.Path().string() } / "inc").string();

    FastCache::Testing::ScopedEnv const env { StateVariable, "" };
    CountingRunner runner { VerboseNaming(root) };
    ScriptedToolchainHost host;
    CHECK(!CachedToolchainFingerprint(runner, host, compiler, "cc 1.0", DriverOf(Flavor::Clang)).fingerprint.empty());
}

// --- the compiler banner ------------------------------------------------------

TEST_CASE("One command answers both questions asked of a compiler", "[toolchain-probe]")
{
    // `CompilerBanner` asks what a compiler IS and `NodeToolchains`' `CanSpawn` asks
    // whether it STARTS, and they must ask identically -- otherwise a node judges
    // spawnability from an invocation it then never uses. They diverged once, with
    // `--version` written out in `CanSpawn` while the banner probe learned that `cl`
    // answers only when asked bare, so this pins the shared builder rather than
    // leaving it implied by two call sites.
    CHECK(VersionProbeCommand("cl") == std::vector<std::string> { "cl" });
    CHECK(VersionProbeCommand(R"(C:\MSVC\CL.EXE)") == std::vector<std::string> { R"(C:\MSVC\CL.EXE)" });
    CHECK(VersionProbeCommand("clang-cl") == std::vector<std::string> { "clang-cl", "--version" });
    CHECK(VersionProbeCommand("/usr/bin/g++") == std::vector<std::string> { "/usr/bin/g++", "--version" });
    CHECK(VersionProbeCommand("mystery-cc") == std::vector<std::string> { "mystery-cc", "--version" });
}

TEST_CASE("MSVC is asked for its version the one way it answers", "[toolchain-probe]")
{
    // `cl` is spawned BARE. It has no `--version`, and the exit-code gate below is
    // deliberate, so asking it that way put every MSVC compiler on the fallback and
    // gave them all one identity: the string `cl`. That string is the cache key's
    // compiler identity, so a 14.51 object was replayed for a 14.44 compile under a
    // zero exit code (issue #195).
    //
    // The ARGV is asserted, not only the result, because the result alone cannot
    // tell "asked correctly" from "asked wrongly and the fixture answered anyway".
    // A row that grew a flag would put MSVC back on the fallback in silence.
    ScriptedRunner msvc { CompileRun { .exitCode = 0, .out = std::string { ClBareBanner1451 }, .err = {} } };
    CHECK(CompilerBanner(msvc, "cl") == "Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36252 for x64");
    CHECK(msvc.LastArgv() == std::vector<std::string> { "cl" });

    // Every other driver keeps `--version`, clang-cl included -- it is clang's own
    // option rather than a GNU-family one, and clang-cl exits 0 from it.
    for (auto const* compiler: { "clang-cl", "g++", "/usr/bin/clang", "some-unknown-driver" })
    {
        ScriptedRunner other { CompileRun { .exitCode = 0, .out = "v 1.0\n", .err = {} } };
        CHECK(CompilerBanner(other, compiler) == "v 1.0");
        CHECK(other.LastArgv() == std::vector<std::string> { compiler, "--version" });
    }
}

TEST_CASE("Two MSVC toolsets do not share one identity", "[toolchain-probe]")
{
    // The regression, stated as the thing that went wrong: both of these used to be
    // the string `cl`, so `ComputeManifestKey` and `ComputeKey` could not tell 14.44
    // from 14.51 and a direct-mode hit crossed between them -- silently, because
    // `ValidateManifest` compares this same value and was comparing "cl" with "cl".
    ScriptedRunner newer { CompileRun { .exitCode = 0, .out = std::string { ClBareBanner1451 }, .err = {} } };
    ScriptedRunner older { CompileRun { .exitCode = 0, .out = std::string { ClBareBanner1444 }, .err = {} } };

    auto const a = CompilerBanner(newer, "cl");
    auto const b = CompilerBanner(older, "cl");

    CHECK(a != b);
    // Neither is the fallback. A test that only checked they differ would still
    // pass with one of them left on the normalized name.
    CHECK(a != "cl");
    CHECK(b != "cl");
    // The target architecture rides along, which is what separates the x86 and x64
    // `cl.exe` of ONE toolset: they share their include roots exactly, so nothing
    // else in a fingerprint or in a direct-mode key can tell those two apart.
    CHECK(a.contains("x64"));
}

TEST_CASE("A banner carries no line ending", "[toolchain-probe]")
{
    // Measured: `cl` ends its banner with CRLF, `clang-cl` with LF. This value is a
    // cache key input and a fingerprint input, so a surviving `\r` would put a byte
    // describing the HOST's line-ending convention into the identity of the
    // COMPILER -- separating two things that are the same, and nothing else.
    ScriptedRunner crlf { CompileRun { .exitCode = 0, .out = "some-cc 1.2.3\r\nmore\r\n", .err = {} } };
    ScriptedRunner lf { CompileRun { .exitCode = 0, .out = "some-cc 1.2.3\nmore\n", .err = {} } };
    CHECK(CompilerBanner(crlf, "some-cc") == "some-cc 1.2.3");
    CHECK(CompilerBanner(lf, "some-cc") == "some-cc 1.2.3");
}

TEST_CASE("A banner falls back to a normalized name, not the spelling", "[toolchain-probe]")
{
    // The fallback is now reached only by a driver that cannot be run at all, or
    // answers nothing: the table gives every driver a probe it exits ZERO from.
    // Scripted here with `cl`'s real `--version` refusal, which is what used to
    // reach it and is still a faithful "ran, failed, and said something".
    //
    // Returning the basename AS SPELLED made the digest depend on how the compiler
    // was named rather than on which compiler it is: a worker configured with
    // `C:\path\cl.exe` and a build invoking bare `cl` computed different
    // fingerprints, and the scheduler matched neither to the other -- "no worker
    // matches this toolchain", on a fleet where both ends were pointed at the same
    // compiler.
    ScriptedRunner refuses { CompileRun { .exitCode = 2, .out = std::string { ClVersionRefusal }, .err = {} } };

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

// --- the target a driver will actually generate for --------------------------

namespace
{
/// Real `clang-cl -### /TP /c NUL` stderr from ONE clang-cl binary, run twice.
///
/// The PAIR is the defect, which is why both are here rather than one. Same driver,
/// same `--version` banner, same resource directory -- so the same toolchain
/// fingerprint, which is exactly what makes the two ends match once the VC toolset
/// and the Windows SDK left that digest. What differs is the only thing neither the
/// banner nor the resource tree records: which MSVC clang could see, and therefore
/// the compatibility version it bakes into code generation.
///
/// Captured rather than invented, and trimmed to the shape the parser must cope
/// with. The trimming deliberately KEEPS the trap: `Target:` on line two names a
/// triple as well, unversioned, three lines closer to the top than the answer.
constexpr std::string_view ClangClDriverLineService =
    R"(clang version 22.1.3 (https://github.com/llvm/llvm-project e9846648fd61)
Target: x86_64-pc-windows-msvc
Thread model: posix
InstalledDir: C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin
 (in-process)
 "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang-cl.exe" "-cc1" "-triple" "x86_64-pc-windows-msvc19.33.0" "-emit-obj" "-fms-compatibility-version=19.33" "-o" "NUL.obj" "-x" "c++" "NUL"
)";

/// The same binary again, from inside a developer command prompt.
constexpr std::string_view ClangClDriverLineDeveloperPrompt =
    R"(clang version 22.1.3 (https://github.com/llvm/llvm-project e9846648fd61)
Target: x86_64-pc-windows-msvc
Thread model: posix
InstalledDir: C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin
 (in-process)
 "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang-cl.exe" "-cc1" "-triple" "x86_64-pc-windows-msvc19.51.36252" "-emit-obj" "-fms-compatibility-version=19.51.36252" "-o" "NUL.obj" "-x" "c++" "NUL"
)";

/// Real `clang -### -x c++ -c /dev/null` stderr from an Ubuntu clang 20.
constexpr std::string_view GnuClangDriverLine =
    R"(Ubuntu clang version 20.1.2 (0ubuntu1~24.04.3)
Target: x86_64-pc-linux-gnu
Thread model: posix
InstalledDir: /usr/lib/llvm-20/bin
 (in-process)
 "/usr/lib/llvm-20/bin/clang" "-cc1" "-triple" "x86_64-pc-linux-gnu" "-emit-obj" "-disable-free" "-main-file-name" "null" "-o" "null.o" "-x" "c++" "/dev/null"
)";
} // namespace

TEST_CASE("The frontend line's triple is read, never the unversioned Target: header", "[toolchain-probe]")
{
    // Both lines name a triple and the header comes first, so a parser that took
    // whichever it found would take the wrong one -- and would look right doing it,
    // since the architecture it pins is correct. What the header drops is the
    // version suffix, which is where -fms-compatibility-version lives and therefore
    // the only part of the answer this whole probe exists to carry.
    CHECK(ParseDriverTargetTriple(ClangClDriverLineDeveloperPrompt) == "x86_64-pc-windows-msvc19.51.36252");
    CHECK(ParseDriverTargetTriple(ClangClDriverLineService) == "x86_64-pc-windows-msvc19.33.0");
    CHECK(ParseDriverTargetTriple(GnuClangDriverLine) == "x86_64-pc-linux-gnu");
}

TEST_CASE("One clang-cl reports two targets depending on the MSVC it can see", "[toolchain-probe]")
{
    // The regression this exists for. A worker runs as a Windows service and
    // inherits no developer prompt, so clang finds no MSVC and falls back to its own
    // built-in default; a launcher in a developer prompt finds the installed one.
    // Same binary, same banner, same resource directory -- so the same fingerprint,
    // and the scheduler matches them. The two targets differ, and clang's Microsoft
    // C++ ABI gates version-specific code generation on exactly that difference.
    auto const service = ParseDriverTargetTriple(ClangClDriverLineService);
    auto const developerPrompt = ParseDriverTargetTriple(ClangClDriverLineDeveloperPrompt);

    REQUIRE_FALSE(service.empty());
    REQUIRE_FALSE(developerPrompt.empty());
    CHECK(service != developerPrompt);
}

TEST_CASE("A quoted path with spaces does not shift the triple that follows it", "[toolchain-probe]")
{
    // The value is read POSITIONALLY -- the argument after `-triple` -- and clang
    // prints the driver's own path first, quoted, with spaces in it on every
    // ordinary Windows install. Splitting on whitespace alone would break that into
    // two tokens and slide the read along by one, which returns a neighbouring
    // token rather than failing.
    CHECK(ParseDriverTargetTriple(ClangClDriverLineService) == "x86_64-pc-windows-msvc19.33.0");
}

TEST_CASE("A driver line that names no triple this can trust yields nothing", "[toolchain-probe]")
{
    // Empty is the safe answer in every one of these: it leaves the cache key
    // spelled as it is today and the dispatch line unpinned, which costs a MISS
    // between two machines that disagree about whether the probe worked -- never a
    // hit on an object built for another target.

    // No frontend line at all: a driver that did not understand the probe.
    CHECK(ParseDriverTargetTriple("clang: error: no such file or directory: 'NUL'\n").empty());

    // A banner and a Target: header, and nothing else. The header must NOT answer.
    CHECK(ParseDriverTargetTriple("clang version 22.1.3\nTarget: x86_64-pc-windows-msvc\n").empty());

    // `-triple` as the last token on the line: nothing follows it to read.
    CHECK(ParseDriverTargetTriple(R"( "clang" "-cc1" "-triple")").empty());

    // A value carrying a path separator is a parse that went wrong. Returning it
    // would split the fleet's keys by install location and hand a worker an
    // argument its own filter has to refuse.
    CHECK(ParseDriverTargetTriple(R"( "clang" "-cc1" "-triple" "/usr/lib/llvm-20/bin")").empty());
    CHECK(ParseDriverTargetTriple(R"( "clang" "-cc1" "-triple" "C:\\Program Files\\x")").empty());

    // A token with no dash is not a triple.
    CHECK(ParseDriverTargetTriple(R"( "clang" "-cc1" "-triple" "x86_64")").empty());

    // `-triple` carrying no value leaves the next FLAG standing where the value
    // should be, and a flag satisfies every other rule on the list: it is short, it
    // has a dash in it, and it is made of nothing else. Read positionally, that
    // returns somebody else's option as the target -- and it would reach both a
    // cache key and a `--target=` argument.
    CHECK(ParseDriverTargetTriple(R"( "clang" "-cc1" "-triple" "-emit-obj" "-o" "a.o")").empty());
    CHECK(ParseDriverTargetTriple(R"( "clang" "-cc1" "-triple" "--target=x86_64-pc-linux-gnu")").empty());
}

TEST_CASE("CRLF in a captured driver line does not hide the triple", "[toolchain-probe]")
{
    // A capture taken on Windows, or piped through a tool that rewrote the line
    // endings, carries a trailing carriage return. Left on the final token it would
    // fail the shape check and the probe would answer nothing at all.
    CHECK(ParseDriverTargetTriple(" \"clang\" \"-cc1\" \"-triple\" \"x86_64-pc-linux-gnu\"\r\n") == "x86_64-pc-linux-gnu");
}

namespace
{
/// Real `g++ -### -x c++ -c /dev/null` stderr from an Ubuntu GCC 14.
///
/// GCC prints no `-cc1` invocation at all -- its frontend is `cc1plus` and takes no
/// `-triple` -- so the `Target:` header is not a lesser answer here, it is the only
/// one. That is the exact opposite of a clang driver, where the same header is the
/// unversioned half-answer, and it is why the two mechanisms are separate rather
/// than one parser trying both lines.
constexpr std::string_view GnuGccDriverLine =
    R"(Using built-in specs.
COLLECT_GCC=g++
OFFLOAD_TARGET_NAMES=nvptx-none:amdgcn-amdhsa
Target: x86_64-linux-gnu
Configured with: ../src/configure -v --with-pkgversion='Ubuntu 14.2.0-4ubuntu2~24.04.1'
Thread model: posix
gcc version 14.2.0 (Ubuntu 14.2.0-4ubuntu2~24.04.1)
)";
} // namespace

TEST_CASE("A GNU driver names its target on a header, and that is all it names", "[toolchain-probe]")
{
    CHECK(ParseDriverTargetHeader(GnuGccDriverLine) == "x86_64-linux-gnu");

    // And the frontend reader finds nothing in it, which is the whole reason this
    // driver needs a mechanism of its own rather than a fallback inside that one.
    CHECK(ParseDriverTargetTriple(GnuGccDriverLine).empty());
}

TEST_CASE("The two target mechanisms disagree about a clang driver, deliberately", "[toolchain-probe]")
{
    // Both readers answer on the same clang output, and they answer DIFFERENTLY.
    // The header drops the version suffix that carries `-fms-compatibility-version`;
    // the frontend line keeps it. Which one is right is a property of the driver, so
    // it is settled by the table rather than by whichever line a parser reaches
    // first -- and pointing a clang driver at the header reader would be a silent
    // downgrade that still looked like an answer.
    CHECK(ParseDriverTargetHeader(ClangClDriverLineDeveloperPrompt) == "x86_64-pc-windows-msvc");
    CHECK(ParseDriverTargetTriple(ClangClDriverLineDeveloperPrompt) == "x86_64-pc-windows-msvc19.51.36252");
}

TEST_CASE("A GNU target header that names nothing usable yields nothing", "[toolchain-probe]")
{
    CHECK(ParseDriverTargetHeader("Target:\n").empty());
    CHECK(ParseDriverTargetHeader("Using built-in specs.\nThread model: posix\n").empty());
    // A path is a parse that went wrong, and it would reach a cache key.
    CHECK(ParseDriverTargetHeader("Target: /usr/lib/gcc/x86_64-linux-gnu\n").empty());
    // Tolerated, for the reason the other reader tolerates it.
    CHECK(ParseDriverTargetHeader("Target: x86_64-linux-gnu\r\n") == "x86_64-linux-gnu");
}

TEST_CASE("gcc is asked for its target and never told one", "[toolchain-probe]")
{
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = std::string { GnuGccDriverLine } } };

    CHECK(DiscoverTargetTriple(runner, "/usr/bin/g++", SpecFor(Flavor::Gcc)) == "x86_64-linux-gnu");
    CHECK(runner.Calls() == 1);
}

TEST_CASE("A driver with no target to state is not spawned at all", "[toolchain-probe]")
{
    // `cl` has no `-###` and no `--target`: which code generator runs is decided by
    // WHICH cl.exe is invoked, and no command line can restate that. Asking anyway
    // would spend a process per translation unit to learn nothing.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = std::string { GnuClangDriverLine } } };

    CHECK(DiscoverTargetTriple(runner, "cl.exe", SpecFor(Flavor::Cl)).empty());
    CHECK(DiscoverTargetTriple(runner, "mystery", SpecFor(Flavor::Unknown)).empty());
    CHECK(runner.Calls() == 0);
}

TEST_CASE("A clang driver is asked over an empty input, and its stderr is what is read", "[toolchain-probe]")
{
    // stdout is deliberately populated with something that would parse, because
    // `-###` writes every line to STDERR and reading the wrong stream is the defect
    // this pins: it would silently answer nothing on every machine.
    ScriptedRunner runner { CompileRun { .exitCode = 0,
                                         .out = R"( "clang" "-cc1" "-triple" "wrong-stream-triple")",
                                         .err = std::string { GnuClangDriverLine } } };

    CHECK(DiscoverTargetTriple(runner, "/usr/bin/clang", SpecFor(Flavor::Clang)) == "x86_64-pc-linux-gnu");

    auto const& argv = runner.LastArgv();
    REQUIRE(argv.size() >= 3);
    CHECK(argv.front() == "/usr/bin/clang");
    CHECK(std::ranges::contains(argv, "-###"));
    // The language is named because the input has no extension to read it from. A
    // driver that cannot tell what it is being handed prints no frontend line, and
    // that failure looks exactly like "this driver has no target".
    CHECK(std::ranges::contains(argv, "-x"));
    CHECK(std::ranges::contains(argv, "c++"));
}

TEST_CASE("clang-cl is asked in its own spelling", "[toolchain-probe]")
{
    ScriptedRunner runner { CompileRun {
        .exitCode = 0, .out = {}, .err = std::string { ClangClDriverLineDeveloperPrompt } } };

    CHECK(DiscoverTargetTriple(runner, "clang-cl.exe", SpecFor(Flavor::ClangCl)) == "x86_64-pc-windows-msvc19.51.36252");

    auto const& argv = runner.LastArgv();
    CHECK(std::ranges::contains(argv, "-###"));
    // `/TP`, not `-x c++`: the MSVC driver spells the language its own way, and a
    // GNU spelling here would make clang-cl print a warning instead of a frontend
    // line.
    CHECK(std::ranges::contains(argv, "/TP"));
}

TEST_CASE("A driver that exits non-zero can still have named its target", "[toolchain-probe]")
{
    // The exit code is not consulted, for the same reason the include probe does not
    // consult it: the frontend line is printed before anything that could fail, and
    // a driver can exit non-zero over the empty input it was handed while its answer
    // is perfectly good.
    ScriptedRunner runner { CompileRun { .exitCode = 1, .out = {}, .err = std::string { GnuClangDriverLine } } };
    CHECK(DiscoverTargetTriple(runner, "clang", SpecFor(Flavor::Clang)) == "x86_64-pc-linux-gnu");
}

TEST_CASE("A toolchain label says what the compiler is, where a fingerprint cannot", "[cc][toolchain][label]")
{
    using FastCache::Cc::ToolchainLabel;

    // #194. Two MSVC toolsets on one machine are two opaque hashes on `/fleet`, and
    // an ordinary Visual Studio update leaves exactly that. The label is what tells
    // them apart for a person.
    CHECK(ToolchainLabel("cl.exe", "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207 for x64")
          == "cl 19.44.35207");
    CHECK(ToolchainLabel("cl.exe", "Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36231 for x64")
          == "cl 19.51.36231");

    // The distribution's own revision carries letters, so it is passed over for the
    // upstream version -- which is the number an operator matches against a compiler
    // they have elsewhere.
    CHECK(ToolchainLabel("/usr/bin/g++", "g++ (Ubuntu 14.2.0-4ubuntu2) 14.2.0") == "g++ 14.2.0");

    // A GNU driver prints its own NAME first, and a version-suffixed one is a
    // first-class discovered case. Anchoring on a token that merely contains digits
    // took `g++-13` for `13` and labelled it `g++-13 13` -- so two nodes on 13.2.0 and
    // 13.3.0 rendered identically, which is precisely the confusion this exists to
    // remove.
    CHECK(ToolchainLabel("/usr/bin/g++-13", "g++-13 (Ubuntu 13.3.0-6ubuntu2) 13.3.0") == "g++-13 13.3.0");

    // And a target triple is full of digits that are not versions.
    CHECK(ToolchainLabel("/opt/cross/bin/aarch64-none-elf-gcc", "aarch64-none-elf-gcc (GCC) 12.2.0")
          == "aarch64-none-elf-gcc 12.2.0");

    // A dot is required, so a stray number is not mistaken for a version. The tool's
    // name alone is still true, which is the safe direction -- a wrong number is worse
    // than none.
    CHECK(ToolchainLabel("g++", "g++ built in 2024") == "g++");

    // A parenthesised version is still a version: punctuation is trimmed from both
    // ends before the predicate is asked, or a banner that brackets its number would
    // be silently unlabelled.
    CHECK(ToolchainLabel("clang++", "Debian clang version 17.0.6 (3)") == "clang++ 17.0.6");

    // No version is not a failure. A translated banner, a wrapper that prints its own
    // name, and the basename `CompilerBanner` falls back to when a compiler cannot be
    // run all land here -- and the tool alone still separates two different
    // compilers, which the fingerprint alone did not do for a reader.
    CHECK(ToolchainLabel("/opt/toolchain/g++", "some wrapper with nothing numeric") == "g++");
    CHECK(ToolchainLabel("clang++", "") == "clang++");

    // Empty only when there is no compiler to name.
    CHECK(ToolchainLabel("", "Version 1.2.3").empty());
}
