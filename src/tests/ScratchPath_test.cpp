// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

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
/// says whether the refusal is actually in force and a case asks it before believing anything.
class UncleanableDirectory
{
  public:
    UncleanableDirectory():
        _path { UniqueScratchPath("scratch-uncleanable") },
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
        _handle = ::CreateFileW((_path / "held.txt").c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        return _handle != INVALID_HANDLE_VALUE;
#else
        return ::chmod(_path.c_str(), S_IRUSR | S_IXUSR) == 0 && ::geteuid() != 0;
#endif
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
