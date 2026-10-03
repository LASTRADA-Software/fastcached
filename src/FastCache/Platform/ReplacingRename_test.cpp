// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/ReplacingRename.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#endif

#include <core/Ranges.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Platform;

namespace
{
/// A POSIX-semantics rename that answers what a case scripts, without touching the files.
class ScriptedRename final: public IReplacingRename
{
  public:
    /// @param answer What every rename answers.
    explicit ScriptedRename(std::error_code answer) noexcept:
        _answer { answer }
    {
    }

    [[nodiscard]] std::error_code RenameReplacing(std::filesystem::path const& /*from*/,
                                                  std::filesystem::path const& /*to*/) const override
    {
        return _answer;
    }

  private:
    std::error_code _answer;
};

/// @param path A file. @param text What it holds.
void WriteText(std::filesystem::path const& path, std::string const& text)
{
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out << text;
}

/// @param path A file. @return What it holds.
[[nodiscard]] std::string TextOf(std::filesystem::path const& path)
{
    // Through the stream BUFFER, never `std::istreambuf_iterator`: GCC at -O3 inlines its `sgetc`
    // far enough to see a null buffer and refuses it under -Werror=null-dereference (#1029).
    std::ifstream in { path, std::ios::binary };
    std::ostringstream out;
    out << in.rdbuf();
    return std::move(out).str();
}

/// The answer a filesystem with no POSIX-semantics rename gives, where one exists.
/// @return It, or nothing on a platform whose rename has the semantics already.
[[nodiscard]] std::optional<std::error_code> NoPosixRenameAnswer()
{
#if defined(_WIN32)
    return std::error_code { ERROR_NOT_SUPPORTED, std::system_category() };
#else
    return std::nullopt;
#endif
}
} // namespace

TEST_CASE("The system rename moves a file over the one it replaces, by the POSIX-semantics route", "[platform][rename]")
{
    Testing::ScratchDirectory const scratch { "replacing-rename" };
    auto const from = scratch / "state.tmp";
    auto const to = scratch / "state";
    WriteText(to, "old");
    WriteText(from, "new");

    auto const replaced = RenameIntoPlace(from, to, SystemReplacingRename {});
    REQUIRE(replaced.has_value());
    CHECK(Testing::Unwrap(replaced).route == ReplaceRoute::PosixSemantics);
    CHECK_FALSE(Testing::Unwrap(replaced).posixRefusal);
    CHECK(TextOf(to) == "new");
    CHECK_FALSE(std::filesystem::exists(from));
}

TEST_CASE("A refusal about the files is the answer, and leaves the file it would replace as it was", "[platform][rename]")
{
    // Only a filesystem saying it HAS no such rename is answered by the classic one: a reader that
    // does not share delete refuses for a reason the classic rename would refuse too.
    Testing::ScratchDirectory const scratch { "replacing-rename-refused" };
    auto const from = scratch / "state.tmp";
    auto const to = scratch / "state";
    WriteText(to, "old");
    WriteText(from, "new");
    auto const denied = std::make_error_code(std::errc::permission_denied);
    REQUIRE_FALSE(MeansNoPosixRename(denied));

    auto const refused = RenameIntoPlace(from, to, ScriptedRename { denied });
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == denied);
    CHECK(TextOf(to) == "old");
    CHECK(std::filesystem::exists(from)); // the caller's to remove
}

TEST_CASE("A filesystem with no POSIX-semantics rename is renamed over the classic way, and says so", "[platform][rename]")
{
    Testing::ScratchDirectory const scratch { "replacing-rename-classic" };
    auto const from = scratch / "state.tmp";
    auto const to = scratch / "state";
    WriteText(to, "old");
    WriteText(from, "new");

    auto const answer = NoPosixRenameAnswer();
    if (!answer.has_value())
    {
        // `rename(2)` has the semantics already: nothing it answers asks for another rename.
        CHECK_FALSE(MeansNoPosixRename(std::make_error_code(std::errc::not_supported)));
        CHECK_FALSE(MeansNoPosixRename(std::make_error_code(std::errc::invalid_argument)));
        return;
    }
    REQUIRE(MeansNoPosixRename(*answer));
    auto const replaced = RenameIntoPlace(from, to, ScriptedRename { *answer });
    REQUIRE(replaced.has_value());
    CHECK(Testing::Unwrap(replaced).route == ReplaceRoute::Classic);
    CHECK(Testing::Unwrap(replaced).posixRefusal == *answer);
    CHECK(TextOf(to) == "new");
}

namespace
{
/// A first-party source allowed to rename a file, and why.
struct RenameSiteRow
{
    std::string_view file; ///< Relative to the repository root.
    std::size_t calls;     ///< How many renames it spells.
    std::string_view why;  ///< Why it is not a replace through the one rename.
};

/// Every file that spells a rename. A replace of one file by another goes through
/// `RenameIntoPlace` -- the node's durable writer and the launcher's atomic write both do -- so a
/// new rename is a new row saying why it is not one.
constexpr auto RenameSites = std::to_array<RenameSiteRow>({
    { .file = "src/FastCache/Platform/ReplacingRename.cpp",
      .calls = 2,
      .why = "the rename itself: rename(2) on POSIX, and the classic fallback where a filesystem has no "
             "POSIX-semantics rename" },
    { .file = "src/apps/fastcache-compile-node/RaftStoreArchiver.cpp",
      .calls = 2,
      .why = "moves a consensus store's DIRECTORY aside and into the archive, never one file over another" },
});

/// @param code A source file's text.
/// @return How many renames it spells outside full-line comments.
[[nodiscard]] std::size_t RenamesIn(std::string_view code)
{
    constexpr auto Needle = std::string_view { "filesystem::rename(" };
    auto count = std::size_t { 0 };
    auto stream = std::istringstream { std::string { code } };
    auto line = std::string {};
    while (std::getline(stream, line))
    {
        auto const first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0)
            continue;
        auto at = line.find(Needle);
        while (at != std::string::npos)
        {
            ++count;
            at = line.find(Needle, at + 1);
        }
    }
    return count;
}

/// @param path A file. @return Its text.
[[nodiscard]] std::string ReadWhole(std::filesystem::path const& path)
{
    std::ifstream input { path, std::ios::binary };
    std::ostringstream read;
    read << input.rdbuf();
    return std::move(read).str();
}
} // namespace

TEST_CASE("A file replaces another through the one rename, and every other rename says why it is not one",
          "[platform][rename]")
{
    // The node's durable writer and the launcher's atomic write were each a copy of the
    // POSIX-semantics rename, and two copies are two places a fix lands in one of. Counted per file,
    // so a second rename in a row's own file is a finding too.
    auto const root = std::filesystem::path { FASTCACHED_SOURCE_DIR };
    auto filesRead = std::size_t { 0 };
    auto hits = std::vector<std::pair<std::string, std::size_t>> {};
    for (auto const* const tree: { "src/FastCache", "src/apps" })
    {
        for (auto const& entry: std::filesystem::recursive_directory_iterator { root / tree })
        {
            auto const& path = entry.path();
            auto const isSource = path.extension() == ".cpp" || path.extension() == ".hpp";
            if (!entry.is_regular_file() || !isSource || path.filename().string().ends_with("_test.cpp"))
                continue;
            ++filesRead;
            if (auto const calls = RenamesIn(ReadWhole(path)); calls != 0)
                hits.emplace_back(std::filesystem::relative(path, root).generic_string(), calls);
        }
    }
    // The walk read the tree, not an empty directory.
    CHECK(filesRead > 200);

    for (auto const& [file, calls]: hits)
    {
        INFO(file << " renames " << calls << " time(s). A file replacing another goes through "
                  << "Platform::RenameIntoPlace -- the node's through Consensus::ReplaceFileAtomically. If this "
                  << "rename is not such a replace, add a RenameSites row saying why.");
        auto const* const row = core::findOrNull(RenameSites, std::string_view { file }, &RenameSiteRow::file);
        REQUIRE(row != nullptr);
        CHECK(row->calls == calls);
    }
    // And every row still describes a file that renames: the rename's own row going missing would
    // mean it stopped being the one.
    for (auto const& row: RenameSites)
    {
        INFO(row.file << ": " << row.why);
        CHECK(std::ranges::contains(hits, row.file, [](auto const& hit) { return std::string_view { hit.first }; }));
    }
}
