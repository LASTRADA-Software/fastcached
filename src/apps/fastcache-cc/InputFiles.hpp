// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
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
/// invocation. A coarse read is never later than the stamp of anything that happens after it
/// **only where the filesystem stamps at least as finely as the kernel tick**. HFS+ and ext4 with
/// 128-byte inodes stamp whole seconds and FAT every two seconds, so there a change made after the
/// instant can carry a stamp BEFORE it. A caller therefore never compares a change time with an
/// instant by a raw `>=`: `ChangedAtOrAfter` is the comparison.
using FileInstant = std::int64_t;

/// One second in `FileInstant` units.
inline constexpr FileInstant OneSecondOfFileTime = 1'000'000'000;

/// The coarse stamp granularities `ChangedAtOrAfter` recognises, COARSEST FIRST: FAT's two
/// seconds, then the whole second of HFS+ and of ext4 with 128-byte inodes. A stamp is read as
/// having the first granularity it is a whole multiple of.
inline constexpr std::array<FileInstant, 2> CoarseStampGranularities { 2 * OneSecondOfFileTime, OneSecondOfFileTime };

/// Whether a change stamped @p changeTime may have happened at or after @p instant.
///
/// A filesystem stamps a change made at real time `t` with `floor_g(t)`, where `g` is its stamp
/// granularity, so a change at `t >= instant` carries a stamp `>= floor_g(instant)`. Comparing
/// against @p instant floored to that granularity is therefore the weakest comparison that still
/// never calls such a change older than @p instant. A blanket floor -- ccache's "include file too
/// new", which distrusts everything inside a fixed window -- would stop caching every consumer of a
/// header generated in the same build second, which #1650's design rules out.
///
/// The granularity is inferred from the STAMP ITSELF, since nothing else on hand knows the
/// filesystem: a whole multiple of two seconds reads as FAT's 2 s, any other whole second as 1 s,
/// and anything finer as a filesystem stamping below a second, where `IInputFiles::Now` is
/// already coarse and nothing is floored. The floor rounds toward negative infinity, so a
/// pre-epoch instant is floored correctly as well.
///
/// Residual, failing CLOSED: a fine-grained stamp that happens to land on a whole second is
/// floored as though its filesystem were coarse, so a change shortly BEFORE @p instant can read as
/// at-or-after it. That costs one invocation its cache store and never serves a wrong object. A
/// 1 s filesystem's even-second stamp is judged against a 2 s window for the same reason.
/// @param changeTime A file's change time (`FileIdentity::changeTime`).
/// @param instant An instant read from `IInputFiles::Now`.
/// @return False only when the change is certainly before @p instant.
[[nodiscard]] constexpr bool ChangedAtOrAfter(FileInstant changeTime, FileInstant instant) noexcept
{
    auto granularity = FileInstant { 1 };
    for (auto const candidate: CoarseStampGranularities)
    {
        if (changeTime % candidate == 0)
        {
            granularity = candidate;
            break;
        }
    }
    auto const intoWindow = ((instant % granularity) + granularity) % granularity;
    return changeTime >= instant - intoWindow;
}

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
    virtual ~IInputFiles() = default;

    /// @param path An absolute path, UTF-8.
    /// @return The file's identity, following symlinks, or nullopt when it cannot be read.
    [[nodiscard]] virtual std::optional<FileIdentity> Identify(std::string_view path) const = 0;

    /// @param path An absolute path, UTF-8.
    /// @return The content hash `HashFileContents` computes; empty when unreadable.
    [[nodiscard]] virtual std::string Hash(std::string_view path) const = 0;

    /// @return Now, on the coarse clock change times are stamped with (see `FileInstant`).
    [[nodiscard]] virtual FileInstant Now() const = 0;

  protected:
    /// Protected rather than deleted, so a test fake deriving from this can be returned by value
    /// while nothing slice-copies through the interface.
    IInputFiles() = default;
    IInputFiles(IInputFiles const&) = default;
    IInputFiles& operator=(IInputFiles const&) = default;
    IInputFiles(IInputFiles&&) = default;
    IInputFiles& operator=(IInputFiles&&) = default;
};

/// This machine's files.
///
/// - Windows: `GetFileInformationByHandleEx`, on a handle opened for attributes only with full
///   sharing, so it never contends with a compiler holding the file open. The clock is
///   `GetSystemTimeAsFileTime`.
/// - POSIX: `stat`. The clock is `CLOCK_REALTIME_COARSE` where it exists; elsewhere (macOS), or
///   when that read fails, `CLOCK_REALTIME` moved back by a safety tick; and 0 -- every input
///   reads as changed -- when no clock answers.
///
/// @return The implementation.
[[nodiscard]] std::unique_ptr<IInputFiles> MakeDiskInputFiles();

} // namespace FastCache::Cc
