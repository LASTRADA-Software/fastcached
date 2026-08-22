// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Cc::Test
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
/// reintroduced the same bug -- which is why it is a shared header now rather
/// than a fourth copy. The most recent occurrence was
/// "The worker names the source and the object, not the client" failing one run
/// in a `ctest -j4` sweep and passing the next.
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

/// Write a stub object wherever an argv asks for one, for a fake compiler.
///
/// Honours BOTH families' output spellings, fused: `-o<path>` and `/Fo<path>`.
///
/// Shared rather than written per fake, because three of them modelled only the
/// GNU convention and that is precisely what hid a real bug: the compile worker
/// hard-coded `-o`, which `cl` does not accept, so MSVC wrote its object
/// elsewhere and the worker refused the job -- and every fake passed happily,
/// because each one only knew the flag the worker was already emitting. A fake
/// that models one family cannot fail for the other.
///
/// @param argv The command line the worker built.
/// @param contents Bytes to write, so a case can tell one object from another.
inline void WriteStubObject(std::span<std::string const> argv, std::string_view contents = "OBJECT")
{
    for (auto const& arg: argv)
    {
        for (auto const prefix: { std::string_view { "-o" }, std::string_view { "/Fo" } })
        {
            if (!arg.starts_with(prefix) || arg.size() == prefix.size())
                continue;
            std::ofstream { arg.substr(prefix.size()), std::ios::binary } << contents;
            break;
        }
    }
}

} // namespace FastCache::Cc::Test
