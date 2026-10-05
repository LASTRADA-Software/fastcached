// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace FastCache::Testing
{

/// A path that is THERE and whose contents this process cannot read, for as long as it is alive.
///
/// The I/O arm of a reader, as distinct from its "absent" arm and its verdict on bytes it read.
/// On POSIX it is a file holding @p contents with every permission bit cleared -- EACCES, the case
/// measured against a node whose kept roster was mode 000. On Windows it is a directory where the
/// file should be, which every reader here fails to open as a file; permission bits do nothing
/// there, since `std::filesystem::permissions` only toggles the read-only attribute.
///
/// Neither is guaranteed -- root reads a mode-000 file -- so `Held()` ASKS rather than assumes: a
/// case that quietly degraded to "the file was readable after all" would assert nothing while
/// looking green, and a case that finds it not held says so and skips.
class UnreadablePath
{
  public:
    /// @param path Where the unreadable file is to be.
    /// @param contents What a readable file there would have held.
    UnreadablePath(std::filesystem::path path, std::string_view contents):
        _path { std::move(path) }
    {
#if defined(_WIN32)
        (void) contents;
        std::filesystem::create_directories(_path);
#else
        std::ofstream { _path, std::ios::binary | std::ios::trunc } << contents;
        std::error_code ignored;
        std::filesystem::permissions(_path, std::filesystem::perms::none, ignored);
#endif
    }

    ~UnreadablePath()
    {
#if !defined(_WIN32)
        // Readable again, so the scratch directory can be cleared.
        std::error_code ignored;
        std::filesystem::permissions(_path, std::filesystem::perms::owner_all, ignored);
#endif
    }

    UnreadablePath(UnreadablePath const&) = delete;
    UnreadablePath& operator=(UnreadablePath const&) = delete;
    UnreadablePath(UnreadablePath&&) = delete;
    UnreadablePath& operator=(UnreadablePath&&) = delete;

    /// @return True when the path is there and its contents really cannot be read now.
    [[nodiscard]] bool Held() const
    {
        std::error_code ignored;
        if (!std::filesystem::exists(_path, ignored))
            return false;
        std::ifstream probe { _path, std::ios::binary };
        char byte {};
        return !probe.is_open() || !probe.read(&byte, 1);
    }

  private:
    std::filesystem::path _path;
};

} // namespace FastCache::Testing
