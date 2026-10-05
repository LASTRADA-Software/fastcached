// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>

    #include <unistd.h>
#endif

using FastCache::Testing::ClearScratch;
using FastCache::Testing::ReleaseScratch;
using FastCache::Testing::UniqueScratchPath;

namespace
{

/// A directory holding one file, which this object makes impossible to remove for as long as it
/// lives -- and removes itself afterwards.
///
/// The one way each platform has of refusing a removal to its owner: on Windows a file held open
/// WITHOUT `FILE_SHARE_DELETE`, which no deletion can get past; on POSIX a directory without write
/// permission, from which no entry can be unlinked. Root ignores permission bits, so `Pinned()`
/// says whether the refusal is actually in force and a case asks it before believing anything --
/// and it says so by ATTEMPTING the removal, never by trusting that the share flags or the mode
/// achieved it: a case that quietly degraded to "it could be removed after all" would assert
/// nothing while looking green.
class UncleanableDirectory
{
  public:
    /// @param path Where to plant it: a name of its own by default, or the name a case is about
    ///        to be handed.
    explicit UncleanableDirectory(std::filesystem::path path = UniqueScratchPath("scratch-uncleanable")):
        _path { std::move(path) },
        _pinned { Pin() }
    {
    }

    UncleanableDirectory(UncleanableDirectory const&) = delete;
    UncleanableDirectory(UncleanableDirectory&&) = delete;
    UncleanableDirectory& operator=(UncleanableDirectory const&) = delete;
    UncleanableDirectory& operator=(UncleanableDirectory&&) = delete;

    ~UncleanableDirectory()
    {
#if defined(_WIN32)
        if (_handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(_handle);
#else
        ::chmod(_path.c_str(), S_IRWXU);
#endif
        auto error = std::error_code {};
        std::filesystem::remove_all(_path, error);
    }

    /// @return The directory.
    [[nodiscard]] std::filesystem::path const& Path() const noexcept
    {
        return _path;
    }

    /// @return Whether the removal is really refused, which it is not for root on POSIX.
    [[nodiscard]] bool Pinned() const noexcept
    {
        return _pinned;
    }

  private:
    /// Create the directory and its file, then refuse their removal.
    /// @return Whether the refusal is in force.
    [[nodiscard]] bool Pin()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path);
        {
            std::ofstream { _path / "held.txt" } << "held";
        }
#if defined(_WIN32)
        // FILE_SHARE_READ only -- deliberately NOT FILE_SHARE_DELETE.
        _handle = ::CreateFileW((_path / "held.txt").c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        if (_handle == INVALID_HANDLE_VALUE)
            return false;
#else
        if (::chmod(_path.c_str(), S_IRUSR | S_IXUSR) != 0)
            return false;
#endif
        // The refusal, asked of the filesystem itself.
        auto const removed = std::filesystem::remove(_path / "held.txt", error);
        return !removed && static_cast<bool>(error);
    }

    std::filesystem::path _path;
#if defined(_WIN32)
    HANDLE _handle = INVALID_HANDLE_VALUE;
#endif
    bool _pinned;
};

} // namespace

TEST_CASE("A scratch directory that cannot be cleared is refused by name, never reused", "[tests][scratch]")
{
    // The inheritance `ClearScratch` exists to prevent: a directory an earlier run left behind
    // under this name, which the clear cannot empty. It used to discard `remove_all`'s error and
    // hand the directory back as it stood, leftovers and all.
    UncleanableDirectory const leftover;
    if (!leftover.Pinned())
        SKIP("nothing refuses a removal here (root on POSIX ignores permission bits)");

    auto message = std::string {};
    try
    {
        ClearScratch(leftover.Path());
    }
    catch (std::runtime_error const& refusal)
    {
        message = refusal.what();
    }
    CAPTURE(message);
    CHECK(message.contains(leftover.Path().string()));
    CHECK(message.contains("refusing to reuse"));
}

TEST_CASE("A scratch directory's leftovers are cleared, and it is created afresh", "[tests][scratch]")
{
    // The control for the refusal above: an ordinary leftover is emptied without complaint, so the
    // refusal is about the directory that cannot be cleared and not about clearing at all.
    auto const path = UniqueScratchPath("scratch-leftover");
    std::filesystem::create_directories(path / "nested");
    std::ofstream { path / "nested" / "stale.log" } << "an earlier run";

    ClearScratch(path);
    CHECK(std::filesystem::is_directory(path));
    CHECK(std::filesystem::is_empty(path));

    auto diagnostics = std::ostringstream {};
    CHECK(ReleaseScratch(path, diagnostics));
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(diagnostics.str().empty());
}

TEST_CASE("A scratch directory that cannot be removed is reported, not thrown about", "[tests][scratch]")
{
    // What the destructor does: it cannot throw, so it says so -- and the next construction under
    // the same name is what refuses the reuse.
    UncleanableDirectory const leftover;
    if (!leftover.Pinned())
        SKIP("nothing refuses a removal here (root on POSIX ignores permission bits)");

    auto diagnostics = std::ostringstream {};
    CHECK_FALSE(ReleaseScratch(leftover.Path(), diagnostics));
    CHECK(diagnostics.str().contains("WARNING"));
    CHECK(diagnostics.str().contains(leftover.Path().string()));
}

//
// `ScratchPath.hpp` is what four raw call sites in `ConsensusTier_test.cpp` bypassed --
// each created a directory at `UniqueScratchPath`'s name and never removed it, leaking
// hundreds of `consensus-unreadable-state-*` directories into %TEMP%. When Windows later
// handed a dead test process's pid to a fresh one, the fresh one computed the SAME name
// and reopened the dead one's leftover Raft log as its own, failing a `SaveLog` gap check
// the real history of that log never provoked.
//
// The fix folded the clearing into `UniqueScratchPath` itself, so no raw call site can
// reintroduce the leak by omission. This proves that guarantee directly, at the seam
// rather than through a caller: plant a leftover at the exact name the NEXT call will
// return, then show that call removes it -- and, separately, that a leftover it genuinely
// cannot remove is reported rather than swallowed.

using Catch::Matchers::ContainsSubstring;

namespace
{

/// Predicts the path the NEXT `UniqueScratchPath(prefix)` call will return, by calling it once
/// to see where "now" sits and incrementing the counter suffix the same way the function does
/// -- without assuming this is the first call `UniqueScratchPath` has seen for `prefix` in this
/// process, since the counter is shared with anything else this test binary runs first.
/// @param prefix Passed straight through to `UniqueScratchPath`.
/// @return The path the next call for the same prefix is expected to return.
[[nodiscard]] std::filesystem::path PredictNextScratchPath(std::string_view prefix)
{
    auto const baseline = FastCache::Testing::UniqueScratchPath(prefix);
    auto const baselineName = baseline.filename().string();

    // The name is "<prefix>-<pid>-<N>"; the next call increments N.
    auto const lastDash = baselineName.rfind('-');
    REQUIRE(lastDash != std::string::npos);
    auto const counter = std::stoll(baselineName.substr(lastDash + 1));
    auto const nextName = baselineName.substr(0, lastDash + 1) + std::to_string(counter + 1);
    return baseline.parent_path() / nextName;
}

/// Removes `path` (recursively) when this object goes out of scope, including when a `REQUIRE`
/// above it unwinds -- unlike `CHECK`, which does not -- so a case that goes red still cleans up
/// what it planted rather than leaking one directory into the system temp location per red run.
struct RemoveAtExit
{
    std::filesystem::path path;

    explicit RemoveAtExit(std::filesystem::path removedPath):
        path { std::move(removedPath) }
    {
    }

    RemoveAtExit(RemoveAtExit const&) = delete;
    RemoveAtExit(RemoveAtExit&&) = delete;
    RemoveAtExit& operator=(RemoveAtExit const&) = delete;
    RemoveAtExit& operator=(RemoveAtExit&&) = delete;

    ~RemoveAtExit()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

TEST_CASE("UniqueScratchPath clears a leftover at the name it is about to hand out", "[tests][scratch]")
{
    auto const nextPath = PredictNextScratchPath("scratchpath-selftest");
    RemoveAtExit const cleanup { nextPath };

    // Plant a leftover exactly where the next call will land -- the shape a dead
    // process's directory takes when its pid gets reused, with a file inside it so a
    // shallow `remove()` (which refuses a non-empty directory) could not pass this case
    // by accident.
    std::filesystem::create_directories(nextPath);
    {
        std::ofstream stale { nextPath / "stale.txt", std::ios::binary };
        stale << "leftover from a dead process";
    }
    REQUIRE(std::filesystem::exists(nextPath / "stale.txt"));

    auto const returned = FastCache::Testing::UniqueScratchPath("scratchpath-selftest");
    REQUIRE(returned == nextPath);
    CHECK_FALSE(std::filesystem::exists(returned));
}

TEST_CASE("UniqueScratchPath throws naming the path when the leftover at its next name cannot be cleared",
          "[tests][scratch]")
{
    auto const nextPath = PredictNextScratchPath("scratchpath-throws");

    UncleanableDirectory const leftover { nextPath };
    if (!leftover.Pinned())
        SKIP("nothing refuses a removal here (root on POSIX ignores permission bits)");

    CHECK_THROWS_WITH(FastCache::Testing::UniqueScratchPath("scratchpath-throws"), ContainsSubstring(nextPath.string()));
}

TEST_CASE("UniqueScratchPath never hands two threads drawing at once the same name", "[tests][scratch]")
{
    // The pid separates processes; the counter beside it separates the names ONE process draws,
    // and it is atomic because a case may draw from more than one thread. Each thread only records
    // what it was handed -- and any throw, which would otherwise end the process from a thread --
    // and the assertions run here, after both have joined.
    constexpr auto DrawsPerThread = 50;
    struct Draws
    {
        std::vector<std::filesystem::path> names;
        std::string error;
    };
    auto const draw = [](int count) {
        Draws drawn;
        try
        {
            for ([[maybe_unused]] auto const index: std::views::iota(0, count))
                drawn.names.push_back(UniqueScratchPath("scratchpath-threads"));
        }
        catch (std::exception const& error)
        {
            drawn.error = error.what();
        }
        return drawn;
    };
    Draws first;
    Draws second;
    {
        std::jthread const one { [&] { first = draw(DrawsPerThread); } };
        std::jthread const two { [&] { second = draw(DrawsPerThread); } };
    }
    INFO(first.error << second.error);
    REQUIRE(first.error.empty());
    REQUIRE(second.error.empty());

    auto all = first.names;
    all.insert(all.end(), second.names.begin(), second.names.end());
    REQUIRE(all.size() == std::size_t { 2 * DrawsPerThread });
    std::ranges::sort(all);
    CHECK(std::ranges::adjacent_find(all) == all.end());
}
