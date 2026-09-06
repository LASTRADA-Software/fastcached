// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/IDaemonHost.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace
{
/// One binary that daemonizes, and what its working directory has to be.
struct DaemonCallSite
{
    std::string_view file;      ///< Path under the repository root.
    bool spawnsCompiler {};     ///< Whether the program this file starts runs a compiler.
    std::string_view rationale; ///< Why that answer, printed on failure.
};

/// Every `MakePosixDaemonHost` call site in this tree, and what each one owes.
///
/// The middle column is a CLAIM about the PROGRAM and is not derivable from the
/// call: what a binary does after it has daemonized is a fact about the binary. So
/// it is tabulated with its reason beside it, the way
/// `scripts/check-node-working-directory.cmake` tabulates the same claim for the
/// shipped systemd units — this is that check's other route, the one it says it
/// cannot see.
///
/// A file named here that does not exist, or that contains no call, is a FAILURE
/// and never a skip: a scan matching nothing is the failure mode a consistency
/// check must not have.
constexpr std::array CallSites {
    DaemonCallSite { .file = "src/apps/fastcache-compile-node/main.cpp",
                     .spawnsCompiler = true,
                     .rationale = "The compile worker. It spawns a compiler per job and derives "
                                  "-fdebug-prefix-map rules from its own working directory, so `/` makes every "
                                  "absolute path in a dispatched object match a rule that keeps its tail." },
    DaemonCallSite { .file = "src/apps/fastcached/main.cpp",
                     .spawnsCompiler = false,
                     .rationale = "The cache daemon. It executes nothing at all, so no path-mapping rule is ever "
                                  "derived from its directory and `/` costs it nothing but a busy mount point." },
};

/// The file's text with whole-line `//` comments removed.
///
/// **A comment is not a call site** (#723). The line this check looks for is
/// surrounded, in both files, by a paragraph explaining why the argument is what it
/// is — and a paragraph naming the call is exactly what a scan matching anywhere on
/// a line would read as the call itself.
/// @param text The file's contents.
/// @return The same text with comment-only lines blanked, so line structure survives.
[[nodiscard]] std::string WithoutCommentLines(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (auto const line: std::views::split(text, '\n'))
    {
        std::string_view const view { line.begin(), line.end() };
        auto const first = view.find_first_not_of(" \t");
        if (first == std::string_view::npos || !view.substr(first).starts_with("//"))
            out.append(view);
        out.push_back('\n');
    }
    return out;
}

/// The argument list of the one `MakePosixDaemonHost(...)` call in @p text.
///
/// Balanced over parentheses rather than stopping at the first `)`, because an
/// argument may itself be a call — `daemonWorkingDirectory.string()` is one — and a
/// reader that stopped early would judge a fragment. That is not a hypothetical
/// refinement: the first spelling of the node's call site put a `"/"` fallback
/// inside the argument list, where a `[^)]*` reader saw the literal and would have
/// refused a correct tree.
/// @param text The file's text, comments already removed.
/// @return The text between the call's parentheses, or nothing when there is no call.
[[nodiscard]] std::optional<std::string> DaemonHostArguments(std::string_view text)
{
    constexpr std::string_view Call = "MakePosixDaemonHost(";
    auto const start = text.find(Call);
    if (start == std::string_view::npos)
        return std::nullopt;

    auto depth = 1;
    auto const open = start + Call.size();
    for (auto index = open; index < text.size(); ++index)
    {
        if (text[index] == '(')
            ++depth;
        else if (text[index] == ')' && --depth == 0)
            return std::string { text.substr(open, index - open) };
    }
    return std::nullopt;
}

/// The call's second argument, trimmed.
/// @param arguments The text between the call's parentheses.
/// @return The second argument, or nothing when the call passes fewer than two.
[[nodiscard]] std::optional<std::string> SecondArgument(std::string_view arguments)
{
    // Split at the top-level comma only: an argument may carry parentheses or braces
    // of its own.
    auto depth = 0;
    for (auto const index: std::views::iota(std::size_t { 0 }, arguments.size()))
    {
        auto const c = arguments[index];
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;
        else if (c == ',' && depth == 0)
        {
            auto tail = arguments.substr(index + 1);
            auto const first = tail.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return std::nullopt;
            tail = tail.substr(first);
            auto const last = tail.find_last_not_of(" \t\r\n");
            return std::string { tail.substr(0, last + 1) };
        }
    }
    return std::nullopt;
}
} // namespace

TEST_CASE("Every daemonizing binary states its own working directory, and a compiler-spawning one does not state /",
          "[platform][daemon][prefix-map]")
{
    // #784. `PosixDaemonHost` chdir'd to `/` unconditionally, so a compile worker
    // started with `--daemon` landed there however its unit was written — a
    // `WorkingDirectory=` does not survive the double fork, because the chdir happens
    // after it. From `/`, the worker's own `-fdebug-prefix-map=/=<replacement>` matches
    // every absolute path in the object and keeps its tail: `/usr/include/stdio.h`
    // becomes `.usr/include/stdio.h`, under a cache key that is correct.
    //
    // The parameter is what fixes it; this is what asserts the WIRING. Removing the
    // default was enough to make a third binary decide, and nothing at all stops the
    // two that already exist from being edited back to `"/"` — which is the whole of
    // this defect, one line of `main.cpp`.
    //
    // `scripts/check-node-working-directory.cmake` is the same rule for the shipped
    // systemd units and says in its own header that it cannot see this route.
    for (auto const& site: CallSites)
    {
        INFO(site.file << ": " << site.rationale);

        auto const path = std::filesystem::path { FASTCACHED_SOURCE_DIR } / site.file;
        REQUIRE(std::filesystem::exists(path));

        // Through the stream BUFFER, never `std::istreambuf_iterator`. GCC at `-O3`
        // inlines that iterator's `sbumpc()` far enough to see a path where the
        // buffer pointer could be null and rejects it under
        // `-Werror=null-dereference`, which for an `ifstream` it never is. Measured:
        // g++ 14 on CI's `Linux-gcc-release` refused exactly this file while g++ 16.2.1
        // locally did not, so a green local gate said nothing about it. Inserting a
        // `streambuf*` handles null by setting failbit, so there is nothing to
        // complain about. `DefaultConfigPath_test`, `AdminEndpoint.cpp`,
        // `FleetHistory.cpp` and `FileBytes.hpp` each carry this same note.
        std::ifstream in { path, std::ios::binary };
        REQUIRE(in);
        std::ostringstream contents;
        contents << in.rdbuf();
        auto const text = std::move(contents).str();
        REQUIRE_FALSE(text.empty());

        // A file named by the table with no call in it is a failure. Both of these
        // daemonize; a binary that stops doing so is a row to delete, not a case to
        // pass vacuously.
        auto const arguments = DaemonHostArguments(WithoutCommentLines(text));
        REQUIRE(arguments.has_value());

        auto const directory = SecondArgument(Unwrap(arguments));
        REQUIRE(directory.has_value());

        if (site.spawnsCompiler)
            // Not a literal, and specifically not `/`. The rule is "no string literal"
            // rather than "not the string `/`" because a literal is the only spelling a
            // binary that has to derive its directory could be given — this one takes
            // it from `Node::ScratchBaseDirectory()`, whose value `ScratchClaim_test`
            // asserts leaves the worker's own rule standing.
            CHECK_FALSE(Unwrap(directory).contains('"'));
        else
            // The control, and it is not decoration: without it the rule reads "never
            // pass a literal", which is wrong for a daemon that executes nothing and
            // would make this check refuse a correct tree the day somebody obeyed it.
            CHECK(Unwrap(directory) == "\"/\"");
    }
}
