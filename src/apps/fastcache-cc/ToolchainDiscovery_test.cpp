// SPDX-License-Identifier: Apache-2.0
#include "ToolchainDiscovery.hpp"
#include "ToolchainHostTestUtils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;
using namespace FastCache::Cc::Testing;

namespace
{
/// A runner that answers each invocation from a script keyed on `argv[1]`.
///
/// Keyed on the argument rather than by call order because two rows spawn
/// processes -- `vswhere` once and `xcrun` once per compiler name -- and a
/// positional script would have to be rewritten whenever the table's order moved.
class ScriptedRunner final: public IProcessRunner
{
  public:
    /// Answer `argv` containing @p marker with @p output and a zero exit.
    /// @param marker A substring identifying the invocation.
    /// @param output What the process prints.
    /// @return This runner, for chaining.
    ScriptedRunner& Answer(std::string marker, std::string output)
    {
        _answers.emplace_back(std::move(marker), std::move(output));
        return *this;
    }

    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        ++_calls;
        // The pair is captured whole rather than through a structured binding: a
        // binding captured by a lambda is a C++20 corner the static analyser reads
        // as an uninitialized pointer, and the build treats that as an error.
        for (auto const& answer: _answers)
            if (std::ranges::any_of(argv, [&answer](std::string const& a) { return a.contains(answer.first); }))
                return CompileRun { .exitCode = 0, .out = answer.second, .err = {} };
        return CompileRun { .exitCode = 1, .out = {}, .err = {} };
    }

    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        return RunCaptureCombined(argv);
    }

    /// @return How many processes were spawned.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::vector<std::pair<std::string, std::string>> _answers;
    int _calls { 0 };
};

/// The compiler paths a discovery reported, in order.
/// @param candidates What discovery returned.
/// @return Their paths.
[[nodiscard]] std::vector<std::string> PathsOf(std::vector<ToolchainCandidate> const& candidates)
{
    std::vector<std::string> paths;
    paths.reserve(candidates.size());
    for (auto const& candidate: candidates)
        paths.push_back(candidate.compiler);
    return paths;
}

/// Whether a discovery reported @p path.
/// @param candidates What discovery returned.
/// @param path The path sought.
/// @return True when it is there.
[[nodiscard]] bool Found(std::vector<ToolchainCandidate> const& candidates, std::string_view path)
{
    return std::ranges::any_of(candidates, [&](ToolchainCandidate const& c) { return c.compiler == path; });
}

/// Real `vswhere -format value -property installationPath` output.
///
/// Captured from the machine this was written on, CRLF and all. A hand-written
/// fixture would carry only the lines whose handling the author already thought
/// of, which for a parser is the one thing it must not be tested against.
constexpr std::string_view VsWhereOutput = "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\r\n";
} // namespace

TEST_CASE("Every layout row names a binary and a distinct layout", "[toolchain-discovery]")
{
    // The table IS the design, so its shape is worth asserting directly: a row with
    // no binaries searches directories and reports nothing, and two rows sharing a
    // name make the startup log unable to say where a compiler came from.
    std::vector<std::string_view> names;
    for (auto const& layout: ToolchainLayouts())
    {
        CHECK_FALSE(layout.name.empty());
        CHECK_FALSE(layout.binaries.empty());
        // Every mechanism except `xcrun` searches directories, so a row using one of
        // them without a bindir would walk nothing.
        if (layout.root != LayoutRoot::Xcrun)
            CHECK_FALSE(layout.binPaths.empty());
        names.push_back(layout.name);
    }

    std::ranges::sort(names);
    CHECK(std::ranges::adjacent_find(names) == names.end());
}

TEST_CASE("vswhere output is parsed into installation paths", "[toolchain-discovery]")
{
    auto const installations = ParseVsWhereInstallations(VsWhereOutput);
    REQUIRE(installations.size() == 1);
    // The CRLF is gone. Left on, it becomes part of the path and every probe beneath
    // it finds nothing -- silently, because a directory that is not there is skipped.
    CHECK(installations.front() == R"(C:\Program Files\Microsoft Visual Studio\18\Community)");
}

TEST_CASE("vswhere diagnostics are not mistaken for installations", "[toolchain-discovery]")
{
    // It writes its own errors on the same stream, and a message registered as a
    // path would have the node probing a directory named after an error.
    CHECK(ParseVsWhereInstallations("").empty());
    CHECK(ParseVsWhereInstallations("vswhere: error: no instances found\n").empty());
    CHECK(ParseVsWhereInstallations("\r\n   \r\n").empty());

    auto const mixed = ParseVsWhereInstallations("vswhere: warning: something\r\nC:\\VS\r\n");
    CHECK(mixed == std::vector<std::string> { R"(C:\VS)" });
}

TEST_CASE("A version suffix is a version, not any suffix", "[toolchain-discovery]")
{
    // `gcc-ar`, `gcc-nm` and `gcc-ranlib` sit in the same bindir as `gcc`. Offering
    // one as a compiler registers a toolchain that fails every job it is sent, and
    // the failure surfaces to a client as a spawn error rather than as a
    // misconfiguration anybody can see.
    CHECK(MatchesCompilerName("gcc", "gcc", NameMatch::Exact));
    CHECK_FALSE(MatchesCompilerName("gcc-13", "gcc", NameMatch::Exact));

    CHECK(MatchesCompilerName("gcc-13", "gcc", NameMatch::ExactOrVersionSuffixed));
    CHECK(MatchesCompilerName("clang++-18", "clang++", NameMatch::ExactOrVersionSuffixed));
    CHECK(MatchesCompilerName("gcc-13.2", "gcc", NameMatch::ExactOrVersionSuffixed));

    CHECK_FALSE(MatchesCompilerName("gcc-ar", "gcc", NameMatch::ExactOrVersionSuffixed));
    CHECK_FALSE(MatchesCompilerName("gcc-", "gcc", NameMatch::ExactOrVersionSuffixed));
    CHECK_FALSE(MatchesCompilerName("gccx", "gcc", NameMatch::ExactOrVersionSuffixed));
    CHECK_FALSE(MatchesCompilerName("g++", "gcc", NameMatch::ExactOrVersionSuffixed));
}

TEST_CASE("A Visual Studio install is found through vswhere", "[toolchain-discovery]")
{
    constexpr std::string_view vs = "C:/Program Files/Microsoft Visual Studio/18/Community";
    ScriptedToolchainHost host;
    host.SetEnvironment("ProgramFiles(x86)", "C:/Program Files (x86)");
    host.AddExecutable("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe");
    for (auto const& version: { "14.44.35207", "14.51.36231" })
    {
        host.AddExecutable(std::string { vs } + "/VC/Tools/MSVC/" + version + "/bin/Hostx64/x64/cl.exe");
        host.AddExecutable(std::string { vs } + "/VC/Tools/MSVC/" + version + "/bin/Hostarm64/arm64/cl.exe");
    }
    host.AddFile(std::string { vs } + "/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt", "14.51.36231\r\n");

    ScriptedRunner runner;
    runner.Answer("vswhere", std::string { vs } + "\r\n");

    auto const candidates = DiscoverToolchainCandidates(host, runner);
    auto const paths = PathsOf(candidates);

#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr std::string_view nativeBin = "bin/Hostarm64/arm64";
    constexpr std::string_view otherBin = "bin/Hostx64/x64";
#else
    constexpr std::string_view nativeBin = "bin/Hostx64/x64";
    constexpr std::string_view otherBin = "bin/Hostarm64/arm64";
#endif

    // Both installed toolsets, because a client pinned to the older one needs a
    // worker matching it -- and the HINTED one first, so the toolchain a plain `cl`
    // resolves to is what an operator watching a cold start sees register before the
    // others.
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == std::string { vs } + "/VC/Tools/MSVC/14.51.36231/" + std::string { nativeBin } + "/cl.exe");
    CHECK(paths[1] == std::string { vs } + "/VC/Tools/MSVC/14.44.35207/" + std::string { nativeBin } + "/cl.exe");

    // And ONLY the native target. Every target variant of one toolset shares an
    // include tree and, because `cl` has no `--version`, a banner of the normalized
    // basename -- so they all fingerprint identically, and offering them all would
    // register one machine several times under one identity.
    CHECK(std::ranges::none_of(paths, [&](std::string const& p) { return p.contains(otherBin); }));

    CHECK(candidates.front().flavor == Flavor::Cl);
    CHECK(candidates.front().layout == "visual-studio");
}

TEST_CASE("No Visual Studio installer means no vswhere run at all", "[toolchain-discovery]")
{
    // The installer directory is what says Visual Studio might be here, and `xcrun`
    // on the search path is what says Xcode might be. Spawning a process that does
    // not exist costs a failed CreateProcess at every start, forever, to learn
    // something the filesystem already said -- and the Xcode row asks once per
    // compiler name, so unguarded it is six of them on every Windows and Linux node.
    ScriptedToolchainHost host;
    host.SetEnvironment("ProgramFiles(x86)", "C:/Program Files (x86)");

    ScriptedRunner runner;
    CHECK(DiscoverToolchainCandidates(host, runner).empty());
    CHECK(runner.Calls() == 0);
}

TEST_CASE("An LLVM install is found through the registry", "[toolchain-discovery]")
{
    ScriptedToolchainHost host;
    host.AddRegistryValue(
        RegistryHive::LocalMachine, R"(SOFTWARE\LLVM\LLVM)", "", "C:/Program Files/LLVM", RegistryView::ThirtyTwoBit);
    host.AddExecutable("C:/Program Files/LLVM/bin/clang-cl.exe");
    host.AddExecutable("C:/Program Files/LLVM/bin/clang++.exe");
    host.AddExecutable("C:/Program Files/LLVM/bin/lld-link.exe");

    ScriptedRunner runner;
    auto const candidates = DiscoverToolchainCandidates(host, runner);

    CHECK(Found(candidates, "C:/Program Files/LLVM/bin/clang-cl.exe"));
    CHECK(Found(candidates, "C:/Program Files/LLVM/bin/clang++.exe"));
    // A linker is not a compiler. The table names what it wants rather than taking
    // whatever the bindir holds.
    CHECK_FALSE(Found(candidates, "C:/Program Files/LLVM/bin/lld-link.exe"));

    auto const clangCl =
        std::ranges::find_if(candidates, [](ToolchainCandidate const& c) { return c.compiler.contains("clang-cl"); });
    REQUIRE(clangCl != candidates.end());
    CHECK(clangCl->flavor == Flavor::ClangCl);
}

TEST_CASE("One install reported by two rows is one candidate", "[toolchain-discovery]")
{
    // `vswhere` and a registry key both report one install; so do the 32-bit and
    // native LLVM rows on a machine that records both. `WorkerRegistry` keys on
    // (fingerprint, endpoint), so a node registering one machine under two
    // near-identical identities is exactly the double-counting a fleet view then has
    // to render.
    ScriptedToolchainHost host;
    for (auto const view: { RegistryView::ThirtyTwoBit, RegistryView::Native })
        host.AddRegistryValue(RegistryHive::LocalMachine, R"(SOFTWARE\LLVM\LLVM)", "", "C:/LLVM", view);
    host.SetEnvironment("ProgramFiles", "C:/Program Files");
    host.AddExecutable("C:/LLVM/bin/clang.exe");

    ScriptedRunner runner;
    auto const candidates = DiscoverToolchainCandidates(host, runner);

    CHECK(candidates.size() == 1);
    // Named by the row that found it FIRST, which is the table's own order.
    CHECK(candidates.front().layout == "llvm-registry");
}

TEST_CASE("One install reached through two separator spellings is one candidate", "[toolchain-discovery]")
{
    // The registry writes `C:\Program Files\LLVM`; `%ProgramFiles%` writes
    // `C:\Program Files` and the table hangs `LLVM` beneath it. Both name one
    // `clang.exe`, and before the join collapsed separators they produced two
    // strings and two candidates -- one machine registering under two
    // near-identical identities, which is the double-counting the duplicate check
    // exists to prevent. The scripted host cannot see it on its own, because it
    // normalizes on the way in; the paths DISCOVERY builds are what matter.
    ScriptedToolchainHost host;
    host.AddRegistryValue(
        RegistryHive::LocalMachine, R"(SOFTWARE\LLVM\LLVM)", "", R"(C:\Program Files\LLVM)", RegistryView::ThirtyTwoBit);
    host.SetEnvironment("ProgramFiles", R"(C:\Program Files)");
    host.AddExecutable("C:/Program Files/LLVM/bin/clang.exe");

    ScriptedRunner runner;
    auto const candidates = DiscoverToolchainCandidates(host, runner);

    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().compiler == "C:/Program Files/LLVM/bin/clang.exe");
}

TEST_CASE("An extensionless wrapper loses to the .exe beside it", "[toolchain-discovery]")
{
    // An MSYS2 bindir holds a shell wrapper named `gcc` beside the launchable
    // `gcc.exe`. Windows resolves a bare name through PATHEXT and never runs the
    // first, and `ExecutableExists` cannot separate them because that filesystem
    // has no execute bit -- so registering the wrapper would hand the fleet a
    // toolchain that fails every job with a spawn error.
    ScriptedToolchainHost host;
    host.AddExecutable("C:/msys64/ucrt64/bin/gcc");
    host.AddExecutable("C:/msys64/ucrt64/bin/gcc.exe");
    host.AddExecutable("C:/msys64/ucrt64/bin/g++.exe");

    ScriptedRunner runner;
    auto const paths = PathsOf(DiscoverToolchainCandidates(host, runner));

    CHECK(std::ranges::find(paths, "C:/msys64/ucrt64/bin/gcc") == paths.end());
    CHECK(std::ranges::find(paths, "C:/msys64/ucrt64/bin/gcc.exe") != paths.end());
    CHECK(std::ranges::find(paths, "C:/msys64/ucrt64/bin/g++.exe") != paths.end());
}

TEST_CASE("An extensionless compiler with no .exe sibling is still a candidate", "[toolchain-discovery]")
{
    // The other half of the rule: on POSIX nothing carries the suffix, so a rule
    // that simply demanded one would discover nothing at all on Linux and macOS.
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/bin/gcc");

    ScriptedRunner runner;
    CHECK(Found(DiscoverToolchainCandidates(host, runner), "/usr/bin/gcc"));
}

TEST_CASE("Version-suffixed compilers are found beside their plain names", "[toolchain-discovery]")
{
    // Every mainstream distribution installs versioned compilers side by side, and a
    // fleet serving only the unsuffixed one would miss the toolchain most CI images
    // actually build with.
    ScriptedToolchainHost host;
    for (auto const& name: { "gcc", "gcc-13", "g++-13", "clang-18", "gcc-ar", "gcc-13.1" })
        host.AddExecutable(std::string { "/usr/bin/" } + name);
    host.AddFile("/usr/bin/gcc.1", "manual page");

    ScriptedRunner runner;
    auto const candidates = DiscoverToolchainCandidates(host, runner);

    CHECK(Found(candidates, "/usr/bin/gcc"));
    CHECK(Found(candidates, "/usr/bin/gcc-13"));
    CHECK(Found(candidates, "/usr/bin/g++-13"));
    CHECK(Found(candidates, "/usr/bin/clang-18"));
    CHECK(Found(candidates, "/usr/bin/gcc-13.1"));

    CHECK_FALSE(Found(candidates, "/usr/bin/gcc-ar"));
    // Not an executable, and a discovery that took every entry would offer the
    // manual page as a compiler.
    CHECK_FALSE(Found(candidates, "/usr/bin/gcc.1"));
}

TEST_CASE("cc and gcc are two candidates, not one", "[toolchain-discovery]")
{
    // Usually one binary under two names, and they fingerprint DIFFERENTLY: a GNU
    // driver prints its own argv[0] in the banner its clients hash, so a worker
    // registered only as `gcc` never matches a build that invokes `cc`. Collapsing
    // them would look like tidiness and cost the fleet every `cc` build.
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/bin/gcc");
    host.AddExecutable("/usr/bin/cc");

    ScriptedRunner runner;
    auto const candidates = DiscoverToolchainCandidates(host, runner);

    CHECK(Found(candidates, "/usr/bin/gcc"));
    CHECK(Found(candidates, "/usr/bin/cc"));
    CHECK(candidates.size() == 2);
}

TEST_CASE("A local build takes precedence over the distribution's", "[toolchain-discovery]")
{
    // Both are reported -- they are different binaries and fingerprint differently --
    // but `/usr/local` is searched first, so it is the one the log names when a
    // reader asks where a compiler came from.
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/local/bin/gcc");
    host.AddExecutable("/usr/bin/gcc");

    ScriptedRunner runner;
    auto const candidates = DiscoverToolchainCandidates(host, runner);

    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].compiler == "/usr/local/bin/gcc");
    CHECK(candidates[0].layout == "usr-local");
    CHECK(candidates[1].layout == "usr");
}

TEST_CASE("Xcode's active toolchain is found through xcrun", "[toolchain-discovery]")
{
    ScriptedToolchainHost host;
    // `xcrun` itself has to be on the search path, or the row is skipped without
    // spawning anything -- which is what keeps it free on Windows and Linux.
    host.AddExecutable("/usr/bin/xcrun");
    host.SetSearchPath({ "/usr/bin" });

    ScriptedRunner runner;
    runner.Answer("clang++",
                  "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/"
                  "clang++\n");

    auto const candidates = DiscoverToolchainCandidates(host, runner);

    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().compiler
          == "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++");
    CHECK(candidates.front().flavor == Flavor::Clang);
    CHECK(candidates.front().layout == "xcode");
}

TEST_CASE("An xcrun error message is not a compiler", "[toolchain-discovery]")
{
    // `xcrun` writes its failures on the same stream, and one of them registered as
    // a compiler would be a toolchain that cannot be spawned -- which a client meets
    // as a spawn error at job time rather than as anything an operator can see.
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/bin/xcrun");
    host.SetSearchPath({ "/usr/bin" });

    ScriptedRunner runner;
    runner.Answer("clang", "xcrun: error: unable to find utility \"clang\"\n");

    CHECK(DiscoverToolchainCandidates(host, runner).empty());
}

TEST_CASE("A machine with nothing installed discovers nothing", "[toolchain-discovery]")
{
    // Reported as an empty list rather than as an error: discovery is best-effort by
    // construction, and "this host is not a Mac" is not a failure.
    ScriptedToolchainHost host;
    ScriptedRunner runner;
    CHECK(DiscoverToolchainCandidates(host, runner).empty());
}

TEST_CASE("The discovery seam reports what the table found", "[toolchain-discovery]")
{
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/bin/g++");

    ScriptedRunner runner;
    auto const discovery = MakeToolchainDiscovery(host, runner);
    auto const candidates = discovery->Discover();

    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().compiler == "/usr/bin/g++");
    CHECK(candidates.front().flavor == Flavor::Gcc);
}
