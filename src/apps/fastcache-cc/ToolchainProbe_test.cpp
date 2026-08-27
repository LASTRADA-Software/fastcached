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

/// Real `cl --version` combined output, which is a REFUSAL that still names the
/// compiler.
///
/// Captured rather than invented, and the distinction is the whole point of this
/// fixture. `RunCaptureCombined` merges both streams into `out` and leaves `err`
/// empty, so scripting `cl` with an empty `out` models a driver that prints
/// NOTHING -- and the premise every MSVC identity rests on, that `cl` tells us
/// nothing, would then be asserted by the fixture rather than tested. `cl` in fact
/// prints its version line first and only then complains, so the banner IS in the
/// buffer; what discards it is the exit code, which is what the test below pins.
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

TEST_CASE("An MSVC service and a developer prompt derive the same roots", "[toolchain-probe]")
{
    // The defect this whole mechanism exists for. A Windows service inherits no
    // `INCLUDE`; `cl` has no `--version`, so its banner falls back to the
    // normalized basename -- and a worker started as a service therefore
    // fingerprinted as a digest of the string `cl`, IDENTICALLY on every MSVC
    // toolset in existence. Two nodes on different toolsets were interchangeable
    // to the scheduler, which is the false match the fingerprint exists to prevent
    // and the one that yields a silently wrong object.
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

TEST_CASE("clang-cl still reads INCLUDE", "[toolchain-probe]")
{
    // Deliberately unchanged: clang-cl's banner is a genuine version string, so a
    // service run degrades it to a banner-only fingerprint rather than collapsing
    // every version onto one digest -- and locating the VC headers for a driver
    // that lives outside the VC layout needs vswhere, a process spawn on the
    // launcher's per-translation-unit hot path. Issue #145 carries the rest.
    ScriptedRunner runner { CompileRun { .exitCode = 0, .out = {}, .err = {} } };
    ScriptedToolchainHost host;
    DescribeWindowsMachine(host);
    host.SetEnvironment("INCLUDE", "C:/from/the/prompt");

    CHECK(DiscoverIncludePaths(runner, host, "clang-cl.exe", SpecFor(Flavor::ClangCl))
          == std::vector<std::string> { "C:/from/the/prompt" });
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

TEST_CASE("A banner falls back to a normalized name, not the spelling", "[toolchain-probe]")
{
    // Only MSVC reaches the fallback -- `cl` has no `--version` -- so this branch
    // decides every MSVC fingerprint. Returning the basename AS SPELLED made the
    // digest depend on how the compiler was named rather than on which compiler it
    // is: a worker configured with `C:\path\cl.exe` and a build invoking bare `cl`
    // computed different fingerprints, and the scheduler matched neither to the
    // other -- "no worker matches this toolchain", on a fleet where both ends were
    // pointed at the same compiler.
    // The refusal carries `cl`'s real version line, in `out`, where a combined
    // capture puts it -- so this asserts the fallback is taken DESPITE a usable
    // banner sitting in the buffer, rather than for want of one. That is the
    // behaviour, and it is the reason relaxing the exit-code gate would be a
    // change of policy rather than a tidy-up: it would re-key every MSVC
    // fingerprint in a fleet, and a localized version line differs between two
    // machines holding the same toolset.
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
