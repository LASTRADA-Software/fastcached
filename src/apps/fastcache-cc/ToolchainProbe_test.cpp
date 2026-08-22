// SPDX-License-Identifier: Apache-2.0
#include "ToolchainProbe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
