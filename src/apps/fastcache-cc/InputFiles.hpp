// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>

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

/// How far a FINE clock read is moved back, where no coarse clock answers, and how far `Fence`'s
/// fail-closed answer lies below its last coarse reading. It bounds the lag of a kernel's
/// tick-granular file stamping behind a fine clock: a change made just after a fine read can be
/// stamped up to one tick earlier than that read. 10 ms is one tick at 100 Hz, the coarsest tick in
/// common use.
inline constexpr FileInstant FineClockSafetyTick = 10'000'000;

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
/// What moves which field, as measured (#1650):
/// - A rename over the path changes the file id.
/// - An in-place write moves the change time. So does `utimensat`, which sets the last-write time
///   back -- the change time, NOT the last-write time, is why a backdated save is still seen.
/// - `CopyFileW` over an existing file on NTFS does NOT stamp a new change time: it copies the
///   SOURCE's change time (and last-write time) onto the destination. Measured: a source written,
///   200 ms waited, then copied -- the destination's change time equalled the source's, 200 ms
///   before an instant read just before the copy.
///
/// So an older file copied over a newer one is seen by its IDENTITY changing -- in every comparison
/// after T1', and in the TU's T0/T1' comparison -- and never by the clock. The T0 clock rule
/// (`ChangedAtOrAfter` against T0) cannot see such a copy of a HEADER made during the key's
/// preprocess, between T0 and T1': a documented limit of the witness.
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

    /// An instant meant to ORDER every change against it: one made before this call stamped
    /// strictly below the result, one made after it returns at or above it. Reads the FINE clock
    /// at the call, then blocks until the coarse clock reads strictly above both that and its own
    /// reading at the call -- typically one to two ticks -- and returns that coarse reading.
    ///
    /// Why the fine clock bounds the past and the coarse one does not: the coarse clock reads the
    /// last tick boundary the timekeeper ACCUMULATED, which trails the moment it is published and
    /// then holds until the next one, so it lags real time by up to about TWO ticks -- measured
    /// 7.45 ms behind `CLOCK_REALTIME` on WSL's 4 ms tick. Its first new reading can therefore still
    /// lie at or below a fine-clock instant read before this call: with a fence that waited only for
    /// the coarse clock to change, that happened in 25 to 100 of 100 runs on WSL (Linux 6.6). A
    /// FILE STAMP reaches that region only where the kernel stamps with the fine clock -- Linux
    /// 6.13+ (multigrain timestamps, on ext4, xfs, btrfs and tmpfs) stamps a change to a file
    /// stat'ed since its last change with a FINE time. MEASURED in CI: under that waiting rule, 57
    /// of 60 changes made just before the fence read as at-or-after it on a
    /// `Linux-gcc-release-arm64` runner (#1650). That multigrain stamping is what carried the clock
    /// lag into the STAMPS there is INFERRED; no local kernel stamps that way, so the stamping half
    /// is SCRIPTED through `IRealtimeClocks` (`FenceOn`'s cases), while the clock half is asked of
    /// this machine's own clocks.
    ///
    /// The ordering is a property of the clocks, so it holds only where they behave as assumed --
    /// a later stamp is never below the coarse clock, an earlier one never above the fine clock --
    /// and under the condition `ChangedAtOrAfter` states: a filesystem stamping from THIS
    /// machine's clock on a decimal or 2 s grid. Outside that (another machine's clock, a
    /// non-decimal grid, a clock stepped backwards between the reads) it is not a guarantee, and
    /// the fail-closed fallback below gives none either, though it can only call MORE changes
    /// recent.
    /// The wait is `DrainWithin`'s, so its 100 ms bound is MEASURED on the injected wait, never
    /// counted. Where the coarse clock has not passed the floor within that bound, or stops answering, the
    /// result is the last coarse reading moved back by a safety tick; where no coarse clock
    /// exists, the fine clock after waiting that tick out, moved back by it.
    ///
    /// Why not `Now`: an instant read INSIDE the current coarse tick shares that tick with every
    /// change stamped in it, before the read or after, so a change saved a moment before an
    /// invocation started cannot be told from one saved after -- `ChangedAtOrAfter` must call it
    /// "at or after", and the invocation refuses its store. Measured on WSL (4 ms ticks): 5 of 30
    /// compiles launched right after a write refused to store (#1650). Waiting for the next tick
    /// removes the ambiguity rather than failing closed on it, at the cost of one to two ticks.
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

/// The realtime clocks `IInputFiles::Now` and `IInputFiles::Fence` read: a seam so the fence's
/// ORDERING decision can be driven against readings a test host does not produce -- a FILE STAMP
/// above the coarse clock's next tick is what a multigrain kernel writes and no local kernel here
/// does (#1650). The wait between two reads is not part of it: that is `IDrainWait`'s.
class IRealtimeClocks
{
  public:
    virtual ~IRealtimeClocks() = default;

    /// @return The coarse realtime clock -- the one a filesystem stamps changes with -- or nullopt
    ///         where the platform has none or it does not answer.
    [[nodiscard]] virtual std::optional<FileInstant> Coarse() const = 0;

    /// @return The fine realtime clock, or nullopt where it does not answer.
    [[nodiscard]] virtual std::optional<FileInstant> Fine() const = 0;

  protected:
    /// Protected rather than deleted, for `IInputFiles`' reason.
    IRealtimeClocks() = default;
    IRealtimeClocks(IRealtimeClocks const&) = default;
    IRealtimeClocks& operator=(IRealtimeClocks const&) = default;
    IRealtimeClocks(IRealtimeClocks&&) = default;
    IRealtimeClocks& operator=(IRealtimeClocks&&) = default;
};

/// `IInputFiles::Now` over @p clocks: the coarse clock, else the fine one moved back by
/// `FineClockSafetyTick`, else 0 -- every input then reads as changed.
/// @param clocks The clocks to read.
/// @return The instant.
[[nodiscard]] FileInstant NowOn(IRealtimeClocks const& clocks);

/// `IInputFiles::Fence` over @p clocks, as that function states it.
/// @param clocks The clocks to read.
/// @param wait Where the wait between two reads blocks, and the monotonic clock its bound is
///             measured on.
/// @return The fenced instant.
[[nodiscard]] FileInstant FenceOn(IRealtimeClocks const& clocks, IDrainWait& wait);

/// @return This machine's realtime clocks: `GetSystemTimeAsFileTime` (coarse) and
///         `GetSystemTimePreciseAsFileTime` (fine) on Windows; `CLOCK_REALTIME_COARSE` where it
///         exists and `CLOCK_REALTIME` on POSIX.
[[nodiscard]] std::unique_ptr<IRealtimeClocks> MakeSystemRealtimeClocks();

/// This machine's files.
///
/// - Windows: `GetFileInformationByHandleEx`, on a handle opened for attributes only with full
///   sharing, so it never contends with a compiler holding the file open. The clock is
///   `GetSystemTimeAsFileTime`, with `GetSystemTimePreciseAsFileTime` as `Fence`'s floor.
/// - POSIX: `stat`. The clock is `CLOCK_REALTIME_COARSE` where it exists; elsewhere (macOS), or
///   when that read fails, `CLOCK_REALTIME` moved back by a safety tick; and 0 -- every input
///   reads as changed -- when no clock answers. `Fence` reads `CLOCK_REALTIME` for its floor.
///
/// @return The implementation.
[[nodiscard]] std::unique_ptr<IInputFiles> MakeDiskInputFiles();

} // namespace FastCache::Cc
