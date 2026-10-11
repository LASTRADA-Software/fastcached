// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// An instant on the clock a filesystem stamps change times with: nanoseconds since the Unix
/// epoch.
///
/// Read by `IInputFiles::Now` on a COARSE realtime clock where the platform has one, and
/// otherwise on the fine clock moved back by `FineClockSafetyTick`. A filesystem stamps a change
/// with the kernel's tick-granular time, so an instant read with a finer clock can lie AFTER the
/// stamp of a change that happened after it, and the T0 rule (#1650) would call that change older
/// than the invocation. Many filesystems also stamp on a coarser grid of their own -- FAT every
/// 2 s, HFS+ and ext4 with 128-byte inodes every second, Linux's exFAT and vfat every 10 ms, NTFS
/// every 100 ns -- so a change made after the instant can carry a stamp BEFORE it. A caller
/// therefore never compares a change time with an instant by a raw `>=`: `ChangedAtOrAfter` is
/// the comparison, and its comment says where it is exact.
using FileInstant = std::int64_t;

/// One second in `FileInstant` units.
inline constexpr FileInstant OneSecondOfFileTime = 1'000'000'000;

/// The stamp grids `ChangedAtOrAfter` recognises, COARSEST FIRST: FAT's 2 s, then every decimal
/// unit from 1 s down to 10 ns -- among them HFS+'s and 128-byte-inode ext4's 1 s, Linux exFAT's
/// and vfat's 10 ms, and NTFS's and ReFS's 100 ns. A stamp is read as lying on the first grid that
/// divides it exactly; one that none divides is on the 1 ns grid, and nothing is floored.
///
/// The 1 s row is subsumed for comparisons: for a stamp on an odd second, flooring the instant to
/// 1 s or to 2 s gives the same answer, so no input tells it from the 2 s row. It stays because it
/// is a real grid (HFS+, ext4 with 128-byte inodes), and without it an odd whole second would be
/// read as the 100 ms grid instead.
inline constexpr std::array StampGrids { 2 * OneSecondOfFileTime,    OneSecondOfFileTime,       FileInstant { 100'000'000 },
                                         FileInstant { 10'000'000 }, FileInstant { 1'000'000 }, FileInstant { 100'000 },
                                         FileInstant { 10'000 },     FileInstant { 1'000 },     FileInstant { 100 },
                                         FileInstant { 10 } };

/// Every row is positive, so no row divides by zero.
static_assert(std::ranges::all_of(StampGrids, [](FileInstant grid) { return grid > 0; }));

/// The rows are strictly decreasing, so the FIRST grid that divides a stamp is its coarsest.
static_assert(std::ranges::adjacent_find(StampGrids, std::less_equal {}) == StampGrids.end());

/// Whether a change stamped @p changeTime may have happened at or after @p instant.
///
/// A filesystem stamps a change made at real time `t` with `floor_g(t)`, where `g` is its stamp
/// grid, so a change at `t >= instant` carries a stamp `>= floor_g(instant)`. Comparing against
/// @p instant floored to that grid is therefore the weakest comparison that still never calls
/// such a change older than @p instant. A blanket floor -- ccache's "include file too new", which
/// distrusts everything inside a fixed window -- would stop caching every consumer of a header
/// generated in the same build second, which #1650's design rules out.
///
/// The grid is inferred from the STAMP ITSELF (`StampGrids`): its trailing decimal zeros are the
/// only evidence of the grid it lies on, since nothing else on hand knows the filesystem. The floor
/// rounds toward negative infinity, so a pre-epoch instant is floored correctly as well.
///
/// **Exact where** the filesystem's grid is a decimal unit or 2 s, AND the stamp comes from THIS
/// machine's clock. A non-decimal grid, or a stamp another machine's clock wrote -- a network
/// share, or a WSL `/mnt` drive stamped by the Windows host -- is outside it, and there a change
/// after @p instant can still read as before it.
///
/// Residual, failing CLOSED: a fine-grained stamp that happens to end in zeros is read as lying on
/// a coarser grid, so @p instant is floored by at most that unit and a change at most that long
/// BEFORE it can read as at-or-after it. That costs one invocation its cache store and never
/// serves a wrong object.
/// @param changeTime A file's change time (`FileIdentity::changeTime`).
/// @param instant An instant read from `IInputFiles::Now` or `IInputFiles::Fence`.
/// @return False when the change is before @p instant, which within the conditions above means
///         certainly before it; true when it may be at or after it.
[[nodiscard]] constexpr bool ChangedAtOrAfter(FileInstant changeTime, FileInstant instant) noexcept
{
    auto granularity = FileInstant { 1 };
    for (auto const grid: StampGrids)
    {
        if (changeTime % grid == 0)
        {
            granularity = grid;
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

    /// @return Now, on the clock change times are stamped with: coarse where the platform has a
    ///         coarse realtime clock, otherwise the fine clock minus `FineClockSafetyTick` (see
    ///         `FileInstant`).
    [[nodiscard]] virtual FileInstant Now() const = 0;

    /// An instant that ORDERS every change against it: one made before this call is stamped
    /// strictly below the result, and one made after it returns is stamped at or above it. Blocks
    /// until the clock change times are stamped with has crossed one tick to get there.
    ///
    /// Why not `Now`: an instant read INSIDE the current coarse tick shares that tick with every
    /// change stamped in it, before the read or after, so a change saved a moment before an
    /// invocation started cannot be told from one saved after -- `ChangedAtOrAfter` must call it
    /// "at or after", and the invocation refuses its store. Measured on WSL (4 ms ticks): 5 of 30
    /// compiles launched right after a write refused to store (#1650). Waiting for the next tick
    /// removes the ambiguity rather than failing closed on it, at the cost of at most one tick.
    ///
    /// Holds under the condition `ChangedAtOrAfter` states: a filesystem stamping from THIS
    /// machine's clock on a decimal or 2 s grid.
    /// @return The fenced instant, on the clock `Now` reads.
    [[nodiscard]] virtual FileInstant Fence() const = 0;

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
