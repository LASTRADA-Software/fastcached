// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// An instant on the clock a filesystem stamps change times with: nanoseconds since the Unix
/// epoch.
///
/// Read COARSELY on purpose (`IInputFiles::Now`). A filesystem stamps a change with the kernel's
/// tick-granular time, so an instant read with a finer clock can lie AFTER the stamp of a change
/// that happened after it, and the T0 rule (#1650) would call that change older than the
/// invocation. A coarse read is never later than the stamp of anything that happens after it.
using FileInstant = std::int64_t;

/// What a file is right now, for telling whether it changed between two looks within ONE
/// invocation.
///
/// Never stored, transmitted or folded into a key or a manifest: every field is local to this
/// machine and this checkout, and a manifest carrying one would stop being shared (#1650). It is
/// compared only with another identity of the same path taken by the same process.
///
/// The change time, NOT the last-write time, is the point: `copy`, `CopyFileW` and robocopy keep
/// a file's last-write time, and so does `utimensat`, while every one of them moves the change
/// time. The file id catches a rename over the path.
struct FileIdentity
{
    FileInstant changeTime {};   ///< NTFS `ChangeTime` or POSIX `st_ctim`; the last-write time
                                 ///< where the filesystem keeps no change time (FAT, exFAT).
    std::uint64_t size {};       ///< Bytes.
    std::uint64_t volumeId {};   ///< Volume serial number, or `st_dev`.
    std::uint64_t fileIdHigh {}; ///< High half of the 128-bit file id; zero on POSIX.
    std::uint64_t fileIdLow {};  ///< Low half of the file id, or `st_ino`.

    [[nodiscard]] friend bool operator==(FileIdentity const&, FileIdentity const&) = default;
};

/// The launcher's view of its input files during one invocation: identity, content and the
/// clock their change times are on. One seam for the three because the witness only ever
/// compares their answers with each other.
class IInputFiles
{
  public:
    IInputFiles() = default;
    IInputFiles(IInputFiles const&) = delete;
    IInputFiles& operator=(IInputFiles const&) = delete;
    IInputFiles(IInputFiles&&) = delete;
    IInputFiles& operator=(IInputFiles&&) = delete;
    virtual ~IInputFiles() = default;

    /// @param path An absolute path, UTF-8.
    /// @return The file's identity, following symlinks, or nullopt when it cannot be read.
    [[nodiscard]] virtual std::optional<FileIdentity> Identify(std::string_view path) const = 0;

    /// @param path An absolute path, UTF-8.
    /// @return The content hash `HashFileContents` computes; empty when unreadable.
    [[nodiscard]] virtual std::string Hash(std::string_view path) const = 0;

    /// @return Now, on the coarse clock change times are stamped with (see `FileInstant`).
    [[nodiscard]] virtual FileInstant Now() const = 0;
};

/// This machine's files.
///
/// - Windows: `GetFileInformationByHandleEx`, on a handle opened for attributes only with full
///   sharing, so it never contends with a compiler holding the file open. The clock is
///   `GetSystemTimeAsFileTime`.
/// - POSIX: `stat`. The clock is `CLOCK_REALTIME_COARSE` where it exists, `CLOCK_REALTIME`
///   elsewhere.
///
/// @return The implementation.
[[nodiscard]] std::unique_ptr<IInputFiles> MakeDiskInputFiles();

} // namespace FastCache::Cc
