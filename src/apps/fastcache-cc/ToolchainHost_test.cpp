// SPDX-License-Identifier: Apache-2.0
#include "ToolchainHost.hpp"
#include "ToolchainHostTestUtils.hpp"

#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using namespace FastCache::Cc::Testing;

namespace
{
/// A scratch tree that removes itself, for the cases that must exercise the REAL
/// host rather than the scripted one.
///
/// Both halves are tested deliberately. The scripted host is what every layout
/// case runs against, so its own semantics have to be right or those cases assert
/// against a fiction; the real host is what production uses, so the two must agree
/// about what "a directory", "an executable" and "a listing" mean.
class ScratchTree
{
  public:
    explicit ScratchTree(std::string_view name):
        // The unique parent is KEPT, not just used and forgotten: the destructor
        // removes it rather than the root, or every case leaves an empty directory
        // behind in the system temp for good. Same reasoning, and the same shape,
        // as the helper in ToolchainProbe_test.cpp.
        _base { FastCache::Testing::UniqueScratchPath("fc-tch") },
        _root { _base / std::filesystem::path { std::string { name } } }
    {
        std::error_code ec;
        std::filesystem::create_directories(_root, ec);
    }

    ~ScratchTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(_base, ec);
    }

    ScratchTree(ScratchTree const&) = delete;
    ScratchTree& operator=(ScratchTree const&) = delete;
    ScratchTree(ScratchTree&&) = delete;
    ScratchTree& operator=(ScratchTree&&) = delete;

    /// @return The tree's root.
    [[nodiscard]] std::filesystem::path const& Root() const noexcept
    {
        return _root;
    }

    /// Write a file, creating the directories above it.
    ///
    /// Non-const because it changes the tree this object stands for -- which is
    /// also what keeps the returned path from having to be `[[nodiscard]]`, since
    /// most callers want the write and not the name.
    ///
    /// @param relative Path under the root.
    /// @param contents What to write.
    /// @return The absolute path written.
    std::filesystem::path Write(std::string_view relative, std::string_view contents)
    {
        auto const path = _root / std::filesystem::path { relative };
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out { path, std::ios::binary };
        out << contents;
        return path;
    }

  private:
    std::filesystem::path _base;
    std::filesystem::path _root;
};
} // namespace

TEST_CASE("The scripted host creates every directory above a file", "[toolchain-host]")
{
    // A layout case names one deep path; having to also name the five directories
    // above it would put the shape of the fake into every test that uses it.
    ScriptedToolchainHost host;
    host.AddFile("C:/VS/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt", "14.51.36231\n");

    CHECK(host.DirectoryExists("C:/VS"));
    CHECK(host.DirectoryExists("C:/VS/VC/Auxiliary/Build"));
    CHECK_FALSE(host.DirectoryExists("C:/VS/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"));
}

TEST_CASE("The scripted host reads a path spelled with either separator", "[toolchain-host]")
{
    // The reason the fake normalizes at all: a layout row spells a Windows path
    // with backslashes, a registry value comes back with backslashes, and the code
    // joining them uses std::filesystem::path, whose preferred separator differs by
    // host. All three name one location.
    ScriptedToolchainHost host;
    host.AddFile("C:/Kits/10/version.txt", "10.0.26100.0");

    auto const contents = host.ReadTextFile(R"(C:\Kits\10\version.txt)");
    REQUIRE(contents.has_value());
    CHECK(FastCache::Testing::Unwrap(contents) == "10.0.26100.0");
    CHECK(host.DirectoryExists(R"(C:\Kits\10)"));
}

TEST_CASE("The scripted host lists only immediate children", "[toolchain-host]")
{
    ScriptedToolchainHost host;
    host.AddDirectory("/opt/vc/14.44.35207");
    host.AddDirectory("/opt/vc/14.51.36231");
    host.AddDirectory("/opt/vc/14.51.36231/bin");
    host.AddExecutable("/opt/vc/cc");
    host.AddFile("/opt/vc/readme.txt", "");

    auto directories = host.ListDirectories("/opt/vc");
    std::ranges::sort(directories);
    CHECK(directories == std::vector<std::string> { "14.44.35207", "14.51.36231" });

    auto files = host.ListFiles("/opt/vc");
    std::ranges::sort(files);
    CHECK(files == std::vector<std::string> { "cc", "readme.txt" });
}

TEST_CASE("The scripted host separates executables from ordinary files", "[toolchain-host]")
{
    // The distinction discovery turns on: a POSIX bindir holds `gcc` beside
    // `gcc-ar.1`, and offering the manual page as a compiler would register a
    // toolchain that cannot run.
    ScriptedToolchainHost host;
    host.AddExecutable("/usr/bin/gcc");
    host.AddFile("/usr/bin/gcc.1", "manual page");

    CHECK(host.ExecutableExists("/usr/bin/gcc"));
    CHECK_FALSE(host.ExecutableExists("/usr/bin/gcc.1"));
    CHECK(host.ReadTextFile("/usr/bin/gcc").has_value());
}

TEST_CASE("A scripted registry answers per hive and per view", "[toolchain-host]")
{
    // Four independent slots rather than one, because they are four independent
    // keys: a per-user LLVM install really is in HKCU and `Installed Roots` really
    // is in the 32-bit view, so a fake that let one stand in for another would make
    // a layout row look right while naming the wrong key.
    ScriptedToolchainHost host;
    host.AddRegistryValue(
        RegistryHive::LocalMachine, R"(SOFTWARE\LLVM\LLVM)", "", "C:/Program Files/LLVM", RegistryView::ThirtyTwoBit);

    CHECK(host.RegistryString(RegistryHive::LocalMachine, R"(SOFTWARE\LLVM\LLVM)", "", RegistryView::ThirtyTwoBit)
              .has_value());
    CHECK_FALSE(
        host.RegistryString(RegistryHive::LocalMachine, R"(SOFTWARE\LLVM\LLVM)", "", RegistryView::Native).has_value());
    CHECK_FALSE(
        host.RegistryString(RegistryHive::CurrentUser, R"(SOFTWARE\LLVM\LLVM)", "", RegistryView::ThirtyTwoBit).has_value());
}

TEST_CASE("A scripted registry key lists the value names under it", "[toolchain-host]")
{
    // The shape `Installed Roots` has: the installed kits ARE the value names.
    ScriptedToolchainHost host;
    constexpr std::string_view roots = R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)";
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "KitsRoot10", "C:/Kits/10/", RegistryView::ThirtyTwoBit);
    host.AddRegistryValue(RegistryHive::LocalMachine, roots, "10.0.26100.0", "1", RegistryView::ThirtyTwoBit);
    host.AddRegistryValue(RegistryHive::LocalMachine, R"(SOFTWARE\Other)", "Ignored", "x", RegistryView::ThirtyTwoBit);

    auto names = host.RegistryValueNames(RegistryHive::LocalMachine, roots, RegistryView::ThirtyTwoBit);
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string> { "10.0.26100.0", "KitsRoot10" });
}

TEST_CASE("A scripted search path resolves a bare name and passes a path through", "[toolchain-host]")
{
    ScriptedToolchainHost host;
    host.AddExecutable("C:/VS/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe");
    host.SetSearchPath({ "C:/nowhere", "C:/VS/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64" });

    auto const resolved = host.ResolveOnSearchPath("cl.exe");
    REQUIRE(resolved.has_value());
    CHECK(FastCache::Testing::Unwrap(resolved) == "C:/VS/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe");

    // A name that already spells a location is not a search-path lookup, and
    // treating it as one would be a second, wrong answer.
    auto const passedThrough = host.ResolveOnSearchPath("D:/elsewhere/cl.exe");
    REQUIRE(passedThrough.has_value());
    CHECK(FastCache::Testing::Unwrap(passedThrough) == "D:/elsewhere/cl.exe");

    CHECK_FALSE(host.ResolveOnSearchPath("nosuchcompiler").has_value());
    CHECK_FALSE(host.ResolveOnSearchPath("").has_value());
}

TEST_CASE("A bare name resolves to the .exe beside it", "[toolchain-host]")
{
    // The load-bearing case for fingerprint agreement: a build system invokes
    // `cl`, a worker is configured with the absolute path, and the MSVC include
    // roots are derived from that path -- so a bare name that did not resolve
    // would give the two ends different roots and a scheduler that matches
    // nothing. Asserted on every platform, because the fake describes a Windows
    // machine regardless of what is running the case.
    ScriptedToolchainHost host;
    host.AddExecutable("C:/VS/bin/cl.exe");
    host.SetSearchPath({ "C:/VS/bin" });

    auto const resolved = host.ResolveOnSearchPath("cl");
    REQUIRE(resolved.has_value());
    CHECK(FastCache::Testing::Unwrap(resolved) == "C:/VS/bin/cl.exe");
}

TEST_CASE("An extensionless wrapper does not shadow the real executable", "[toolchain-host]")
{
    // An MSYS2 or Cygwin bindir routinely holds a shell wrapper named `cl` beside
    // the launchable `cl.exe`. Windows resolves a bare name through PATHEXT and
    // never runs the extensionless one, so preferring it would hand back a path
    // nothing can launch -- and, through it, an include-root set derived from the
    // wrong place.
    ScriptedToolchainHost host;
    host.AddExecutable("C:/msys64/usr/bin/cl");
    host.AddExecutable("C:/msys64/usr/bin/cl.exe");
    host.SetSearchPath({ "C:/msys64/usr/bin" });

    auto const resolved = host.ResolveOnSearchPath("cl");
    REQUIRE(resolved.has_value());
    CHECK(FastCache::Testing::Unwrap(resolved) == "C:/msys64/usr/bin/cl.exe");
}

TEST_CASE("The real host tells a directory from a file and lists each", "[toolchain-host]")
{
    ScratchTree tree { "listing" };
    tree.Write("bin/marker.txt", "hello");
    tree.Write("include/header.h", "#pragma once\n");

    auto const host = MakeToolchainHost();
    auto const root = tree.Root().string();

    CHECK(host->DirectoryExists(root));
    CHECK_FALSE(host->DirectoryExists((tree.Root() / "bin" / "marker.txt").string()));
    CHECK_FALSE(host->DirectoryExists((tree.Root() / "absent").string()));

    auto directories = host->ListDirectories(root);
    std::ranges::sort(directories);
    CHECK(directories == std::vector<std::string> { "bin", "include" });

    CHECK(host->ListFiles(root).empty());
    CHECK(host->ListFiles((tree.Root() / "bin").string()) == std::vector<std::string> { "marker.txt" });

    // A directory that is not there lists nothing rather than failing: discovery
    // is best-effort by construction and most layouts are absent on most machines.
    CHECK(host->ListDirectories((tree.Root() / "absent").string()).empty());
    CHECK(host->ListFiles((tree.Root() / "absent").string()).empty());
}

TEST_CASE("The real host reads a text file whole and reports an absent one", "[toolchain-host]")
{
    ScratchTree tree { "read" };
    // A trailing newline is what `Microsoft.VCToolsVersion.default.txt` actually
    // carries, and it comes back rather than being trimmed here -- trimming is the
    // caller's decision, and a reader that did it silently would make a file whose
    // content IS whitespace indistinguishable from an empty one.
    tree.Write("version.txt", "14.51.36231\r\n");

    auto const host = MakeToolchainHost();
    auto const contents = host->ReadTextFile((tree.Root() / "version.txt").string());
    REQUIRE(contents.has_value());
    CHECK(FastCache::Testing::Unwrap(contents) == "14.51.36231\r\n");

    CHECK_FALSE(host->ReadTextFile((tree.Root() / "absent.txt").string()).has_value());

    // A DIRECTORY is absent, not empty. `ifstream` opens one perfectly happily on
    // Linux and only the read fails, so a plain `if (!in)` guard lets it through
    // and yields "" -- "a version file that says nothing" rather than "there is no
    // version file", which are different answers to a caller deciding whether a
    // layout is present at all.
    CHECK_FALSE(host->ReadTextFile(tree.Root().string()).has_value());
}

TEST_CASE("The real host resolves this process's own executable on PATH", "[toolchain-host]")
{
    ScratchTree tree { "search" };
#if defined(_WIN32)
    auto const executable = tree.Write("bin/fc-probe.exe", "not really a program");
    constexpr std::string_view bareName = "fc-probe";
    // A wrapper with no extension, beside the real thing -- ordinary in an MSYS2
    // or Cygwin bindir. Written FIRST so a search preferring the bare spelling
    // would find it, since `ExecutableExists` cannot see an execute bit on
    // Windows and accepts any regular file. Windows resolves a bare name through
    // PATHEXT and never runs this one.
    tree.Write("bin/fc-probe", "#!/bin/sh\n");
#else
    auto const executable = tree.Write("bin/fc-probe", "#!/bin/sh\n");
    constexpr std::string_view bareName = "fc-probe";
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
#endif

    auto const host = MakeToolchainHost();
    CHECK(host->ExecutableExists(executable.string()));

    // The variable is restored afterwards rather than removed, which matters in a
    // binary whose other cases spawn compilers.
    FastCache::Testing::ScopedEnv const path { "PATH", (tree.Root() / "bin").string() };

    auto const resolved = host->ResolveOnSearchPath(bareName);
    REQUIRE(resolved.has_value());
    CHECK(std::filesystem::path { FastCache::Testing::Unwrap(resolved) }.filename() == executable.filename());

    CHECK_FALSE(host->ResolveOnSearchPath("fc-definitely-not-installed").has_value());
}

TEST_CASE("The real host reaches the same registry the platform leaf does", "[toolchain-host]")
{
    auto const host = MakeToolchainHost();

    // Off Windows this asserts the seam still answers rather than failing to link
    // or throwing -- which is what lets the layout table carry registry rows
    // unconditionally instead of behind a `#if`.
    auto const value = host->RegistryString(RegistryHive::LocalMachine,
                                            R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)",
                                            "CurrentBuildNumber",
                                            RegistryView::Native);
    auto const names = host->RegistryValueNames(
        RegistryHive::LocalMachine, R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", RegistryView::Native);

#if defined(_WIN32)
    CHECK(value.has_value());
    CHECK_FALSE(names.empty());
#else
    CHECK_FALSE(value.has_value());
    CHECK(names.empty());
#endif

    CHECK_FALSE(host->RegistryString(RegistryHive::LocalMachine, R"(SOFTWARE\fastcached-absent)", "x", RegistryView::Native)
                    .has_value());
}

TEST_CASE("The real host reads the environment through the shared leaf", "[toolchain-host]")
{
    auto const host = MakeToolchainHost();
    FastCache::Testing::ScopedEnv const set { "FASTCACHE_TOOLCHAIN_HOST_PROBE", "present" };

    auto const value = host->Environment("FASTCACHE_TOOLCHAIN_HOST_PROBE");
    REQUIRE(value.has_value());
    CHECK(FastCache::Testing::Unwrap(value) == "present");

    CHECK_FALSE(host->Environment("FASTCACHE_TOOLCHAIN_HOST_ABSENT").has_value());
}
