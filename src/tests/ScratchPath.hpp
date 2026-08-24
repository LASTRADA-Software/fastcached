// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Testing
{

/// A scratch directory name no other test process can collide with.
///
/// **This exists because a per-process counter is not unique.**
/// `catch_discover_tests` registers every TEST_CASE as its own ctest test, so
/// cases run as separate PROCESSES -- and under `ctest -j` several run at once.
/// A `static int counter` starts at zero in every one of them, so the first
/// scratch directory in each process is `<prefix>-1`. Two of them then share a
/// path, and whichever constructs second wipes the files the first is still
/// using: a fixture that fails only under parallelism, only sometimes.
///
/// That was found and fixed once already, in Stats_test.cpp, whose comment
/// records it as "a real, reproduced flake ... not a hypothetical". The fix
/// lived as a private helper in that one file, so three later test files
/// reintroduced the same bug -- which is why it became a shared header rather
/// than a fourth copy.
///
/// It then happened a FIFTH time, in `RaftStorage_test.cpp`, and that occurrence
/// is why this header lives here rather than beside the launcher's tests. The
/// shared version sat under `src/apps/fastcache-cc/`, which the library test
/// binary does not have on its include path -- so the one test suite that could
/// not reach it re-derived it, counter and all, and ten of its cases failed
/// under `ctest -j 8`. A helper is only shared if it is somewhere everything
/// that needs it can include from; `src` is on all three test targets' include
/// paths, which is the same reason `tests/Unwrap.hpp` lives beside this file.
///
/// @param prefix Short, human-recognisable tag so a leaked directory can be
///        traced back to the test that made it.
/// @return An absolute path under the system temp directory. NOT created.
[[nodiscard]] inline std::filesystem::path UniqueScratchPath(std::string_view prefix)
{
    // Process id AND a counter. The pid separates concurrent test processes; the
    // counter separates several scratch directories within one case. Either alone
    // is insufficient, which is exactly how the original bug survived review.
    static int counter = 0;
#if defined(_WIN32)
    auto const pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    auto const pid = static_cast<unsigned long>(::getpid());
#endif
    return std::filesystem::temp_directory_path() / std::format("{}-{}-{}", prefix, pid, ++counter);
}

/// A scratch directory that exists for as long as the object does.
///
/// RAII rather than a teardown statement, because a teardown written after the
/// assertions does not run when one of them fails -- and the failing run is
/// exactly the one whose leftovers confuse the next.
///
/// One definition rather than the four near-identical copies this replaced
/// (`RaftStorage_test::ScratchDirectory`, `CompileJob_test::ScratchDir`,
/// `Stats_test::ScopedStateDir`, `WorkerProtocol_test::Fixture`). They differed
/// only in their prefix, and one of them differed in the way that mattered: it
/// derived its name from a counter alone. A shape copied four times is a shape
/// that will be copied wrong once.
class ScratchDirectory
{
  public:
    /// Create a directory under the system temp location.
    /// @param prefix Human-recognisable tag; see `UniqueScratchPath`.
    explicit ScratchDirectory(std::string_view prefix):
        _path { UniqueScratchPath(prefix) }
    {
        // Clearing first is safe ONLY because the name carries this process's
        // pid: what it can reach is this process's own earlier leftovers, or a
        // dead process whose pid was reused. It can never reach a live peer's
        // directory -- which is precisely what it did do while the name was a
        // bare counter, turning a name collision into deleted data.
        auto error = std::error_code {};
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
    }

    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    ~ScratchDirectory()
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(_path, error);
    }

    /// @return The directory, which exists.
    [[nodiscard]] std::filesystem::path const& Path() const noexcept
    {
        return _path;
    }

    /// Name something inside this directory.
    /// @param relative Path relative to this directory. Not created.
    /// @return The joined path.
    [[nodiscard]] std::filesystem::path operator/(std::string_view relative) const
    {
        return _path / std::filesystem::path { relative };
    }

  private:
    std::filesystem::path _path;
};

} // namespace FastCache::Testing
