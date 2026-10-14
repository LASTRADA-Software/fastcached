// SPDX-License-Identifier: Apache-2.0
#include "InputFiles.hpp"

#include <FastCache/Core/BoundedDrain.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <tests/OverrunningDrainWait.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SteppedDrainWait.hpp>
#include <tests/Unwrap.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <sys/utsname.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;

namespace
{
/// One minute in the seam's unit, for the sanity bound on the epoch and the scale.
constexpr FileInstant OneMinute = std::int64_t { 60 } * 1'000'000'000;

/// @return The running kernel's name and release (`uname -sr`), for a failure to name the kernel
///         it ran on: whether a kernel stamps multigrain times decides what the fence case meets.
std::string KernelRelease()
{
#if defined(_WIN32)
    return "Windows";
#else
    utsname name {};
    if (::uname(&name) != 0)
        return "unknown (uname failed)";
    return std::format("{} {}", static_cast<char const*>(name.sysname), static_cast<char const*>(name.release));
#endif
}

/// Realtime clocks whose readings are a script, so the fence's ordering decision can be driven
/// against readings no local kernel produces. Each `Coarse()` answers the next scripted reading
/// and then repeats the last one, so a clock scripted to stop moving stays stopped; it counts its
/// reads, so a case can bound how often a wait looked.
class ScriptedClocks final: public IRealtimeClocks
{
  public:
    /// @param fine What every `Fine()` answers.
    /// @param coarse What successive `Coarse()` calls answer; must not be empty.
    ScriptedClocks(std::optional<FileInstant> fine, std::vector<std::optional<FileInstant>> coarse):
        _fine { fine },
        _coarse { std::move(coarse) }
    {
    }

    [[nodiscard]] std::optional<FileInstant> Coarse() const override
    {
        ++_reads;
        auto const reading = _coarse.at(_next);
        if (_next + 1 < _coarse.size())
            ++_next;
        return reading;
    }

    [[nodiscard]] std::optional<FileInstant> Fine() const override
    {
        return _fine;
    }

    /// @return How many times `Coarse()` was read.
    [[nodiscard]] int CoarseReads() const noexcept
    {
        return _reads;
    }

  private:
    std::optional<FileInstant> _fine;
    std::vector<std::optional<FileInstant>> _coarse;
    mutable std::size_t _next = 0;
    mutable int _reads = 0;
};

/// An instant in 2025, for scripted clocks to read near.
constexpr FileInstant ScriptBase = std::int64_t { 1'760'000'000 } * OneSecondOfFileTime;

/// One 4 ms tick (250 Hz), the tick the scripted coarse clock advances by.
constexpr FileInstant ScriptTick = 4'000'000;
} // namespace

TEST_CASE("A change time is on the seam's own clock, in its own unit", "[input-files]")
{
    // Catches a wrong epoch (1601 vs 1970) or a wrong scale (100 ns vs 1 ns), both of which
    // would put every change time decades away from Now() and make the T0 rule vacuous.
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-clock" };
    scratch.Write("a.hpp", "one\n");
    auto const files = MakeDiskInputFiles();
    auto const identity = files->Identify((scratch / "a.hpp").string());
    REQUIRE(identity.has_value());
    auto const distance = files->Now() - Unwrap(identity).changeTime;
    CHECK(distance > -OneMinute);
    CHECK(distance < OneMinute);
}

TEST_CASE("A file written after an instant has a change time at or after it", "[input-files]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-after" };
    scratch.Write("a.hpp", "one\n");
    auto const files = MakeDiskInputFiles();
    auto const before = files->Now();
    scratch.Write("a.hpp", "two\n");
    auto const identity = files->Identify((scratch / "a.hpp").string());
    REQUIRE(identity.has_value());
    CHECK(Unwrap(identity).changeTime >= before);
}

TEST_CASE("A fence orders a change made just before it below it and one made after it at or above", "[input-files]")
{
    // #1650, Ruling 11. A T0 read INSIDE the current coarse tick shares that tick with a change
    // saved a moment before the invocation started, so `ChangedAtOrAfter` had to call that change
    // "at or after" and the compile refused its store -- about 1 in 6 launches right after a write
    // on WSL's 4 ms tick. One pass can pass by luck, so the property is asked many times in a row:
    // `Iterations` writes each landing as close before the fence as this process can put them.
    //
    // Each write lands on a file the previous iteration stat'ed, which is what makes Linux 6.13+
    // (multigrain timestamps) stamp it with a FINE time; a fence that waited only for the COARSE
    // clock to change returned an instant below such a stamp in 57 of 60 iterations on a CI arm64
    // runner. No kernel available locally stamps multigrain times, so a local pass cannot tell
    // the fine-clock floor from that wait through FILE STAMPS -- the kernel is printed on every run
    // for that reason. The CLOCK half of it is asked of this machine's clocks by "A fence lands
    // strictly above a fine-clock instant read before it at every tick phase", and the stamping
    // half is scripted by "A fence passes a fine stamp the coarse clock's next tick still lags".
    constexpr auto Iterations = 60;
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-fence" };
    auto const path = (scratch / "a.hpp").string();
    auto const files = MakeDiskInputFiles();

    auto beforeReadAsAfter = 0;
    auto afterReadAsBefore = 0;
    auto unreadable = 0;
    for (auto const iteration: std::views::iota(0, Iterations))
    {
        scratch.Write("a.hpp", std::format("before {}\n", iteration));
        auto const fence = files->Fence();
        auto const before = files->Identify(path);
        scratch.Write("a.hpp", std::format("after {}\n", iteration));
        auto const after = files->Identify(path);
        if (!before || !after)
        {
            ++unreadable;
            continue;
        }
        if (ChangedAtOrAfter(before->changeTime, fence))
            ++beforeReadAsAfter;
        if (!ChangedAtOrAfter(after->changeTime, fence))
            ++afterReadAsBefore;
    }
    // WARN rather than INFO, so a GREEN run records the kernel too: which kernel this passed on is
    // the half of the evidence a failure message alone never carries.
    WARN("of " << Iterations << " iterations, kernel " << KernelRelease());
    CHECK(unreadable == 0);
    CHECK(beforeReadAsAfter == 0);
    CHECK(afterReadAsBefore == 0);
}

TEST_CASE("A fence lands strictly above a fine-clock instant read before it at every tick phase", "[input-files]")
{
    // The CLOCK half of #1650's CI failure, asked of this machine's own clocks: the coarse clock
    // reads the last tick boundary the timekeeper accumulated, which trails its publication and
    // holds until the next one, so its first NEW reading can lie at or below a fine-clock instant
    // read before the fence. A fence that returned the first coarse reading that merely CHANGED
    // landed there in many of these iterations on WSL (Linux 6.6, 4 ms tick) -- a kernel that does
    // not stamp files with the fine clock, so no file-stamp case there can show it.
    //
    // Each iteration first sleeps a different fraction of a tick, so the fence starts at a sweep
    // of phases across it rather than wherever the previous fence left off: 163 us steps cover
    // 16.3 ms, four of Linux's 4 ms ticks.
    //
    // On Windows the case checks the same property but cannot tell the old fence from the new: the
    // old rule was measured at 0 of 200 there. And at the default timer resolution (15.6 ms) a
    // sub-tick `sleep_for` rounds up to whole ticks, so the sweep does not really sweep phases.
    constexpr auto Iterations = 100;
    constexpr auto PhaseStep = std::chrono::microseconds { 163 };
    auto const clocks = MakeSystemRealtimeClocks();

    auto atOrBelow = 0;
    auto unanswered = 0;
    for (auto const iteration: std::views::iota(0, Iterations))
    {
        std::this_thread::sleep_for(PhaseStep * iteration);
        auto const before = clocks->Fine();
        auto const fence = FenceOn(*clocks, FastCache::DefaultDrainWait());
        if (!before)
            ++unanswered;
        else if (*before >= fence)
            ++atOrBelow;
    }
    WARN("of " << Iterations << " iterations, kernel " << KernelRelease());
    CHECK(unanswered == 0);
    CHECK(atOrBelow == 0);
}

TEST_CASE("A fence passes a fine stamp the coarse clock's next tick still lags", "[input-files]")
{
    // The STAMPING half of #1650's CI failure, which no local kernel produces: a multigrain kernel
    // stamped the save just before the fence with a FINE time, and the coarse clock -- which lags
    // real time by up to about two ticks -- moved once and still read BELOW that stamp. A fence
    // returning the first coarse reading that merely DIFFERS from its first reads the save as
    // at-or-after itself.
    auto const fineStamp = ScriptBase + ScriptTick + 1'234'567;
    auto const fineAtCall = fineStamp + 65'432;
    auto const laggingTick = ScriptBase + ScriptTick;
    auto const nextTick = ScriptBase + (2 * ScriptTick);
    ScriptedClocks const clocks { fineAtCall, { ScriptBase, ScriptBase, laggingTick, laggingTick, nextTick } };
    FastCache::Testing::SteppedDrainWait wait;

    auto const fence = FenceOn(clocks, wait);

    CHECK(fence == nextTick);
    CHECK_FALSE(ChangedAtOrAfter(fineStamp, fence));
    // The control: a change after the fence is stamped at or above the coarse clock, which read
    // `fence`, and must read as at-or-after it.
    CHECK(ChangedAtOrAfter(nextTick, fence));
}

TEST_CASE("A fence whose coarse clock never passes the floor answers a safety tick below it", "[input-files]")
{
    // Every sleep costs three times its request, so the bound can tell a MEASURED wait from a
    // COUNTED one: measured, it stops at the first look at or past 100 ms of what the sleeps COST,
    // which is under one charged sleep past it; counted (`waited += poll`), it would run 100
    // requested sleeps -- 300 ms by this clock -- and read the coarse clock about 100 times.
    constexpr auto Overrun = 3;
    ScriptedClocks const clocks { ScriptBase + 9, { ScriptBase } };
    FastCache::Testing::OverrunningDrainWait wait { Overrun };
    constexpr auto Bound = std::chrono::milliseconds { 100 };
    constexpr auto ChargedPoll = std::chrono::milliseconds { 1 } * Overrun;

    CHECK(FenceOn(clocks, wait) == ScriptBase - FineClockSafetyTick);
    CHECK(wait.Elapsed() >= Bound);
    CHECK(wait.Elapsed() < Bound + ChargedPoll);
    // One read at the call, then one per look: a look before each charged sleep and one after the
    // last.
    CHECK(clocks.CoarseReads() <= 1 + ((Bound / ChargedPoll) + 2));
}

TEST_CASE("A fence whose coarse clock stops answering answers a safety tick below its last reading", "[input-files]")
{
    ScriptedClocks const clocks { ScriptBase + 9, { ScriptBase, ScriptBase + 1, std::nullopt } };
    FastCache::Testing::SteppedDrainWait wait;

    CHECK(FenceOn(clocks, wait) == ScriptBase + 1 - FineClockSafetyTick);
}

TEST_CASE("A fence with no coarse clock waits a safety tick and answers the fine clock moved back by it", "[input-files]")
{
    ScriptedClocks const clocks { ScriptBase, { std::nullopt } };
    FastCache::Testing::SteppedDrainWait wait;

    CHECK(FenceOn(clocks, wait) == ScriptBase - FineClockSafetyTick);
    CHECK(wait.Elapsed() == std::chrono::nanoseconds { FineClockSafetyTick });
}

TEST_CASE("A fence with no clock at all answers zero so every input reads as changed", "[input-files]")
{
    ScriptedClocks const clocks { std::nullopt, { std::nullopt } };
    FastCache::Testing::SteppedDrainWait wait;

    CHECK(FenceOn(clocks, wait) == 0);
}

TEST_CASE("Now reads the coarse clock and falls back to the fine clock moved back and then to zero", "[input-files]")
{
    CHECK(NowOn(ScriptedClocks { ScriptBase + 7, { ScriptBase } }) == ScriptBase);
    CHECK(NowOn(ScriptedClocks { ScriptBase, { std::nullopt } }) == ScriptBase - FineClockSafetyTick);
    CHECK(NowOn(ScriptedClocks { std::nullopt, { std::nullopt } }) == 0);
}

#if defined(_WIN32)
TEST_CASE("CopyFileW over a file changes its identity while carrying the source's last-write time", "[input-files]")
{
    // #1650's deterministic variant: an mtime-only identity cannot see this copy at all.
    //
    // What this does NOT assert is that the change time moved. It does not: measured on NTFS,
    // `CopyFileW` copies the SOURCE's change time onto the destination rather than stamping a new
    // one, so an older file copied over a newer one carries an older change time than the file it
    // replaced. Nor does it assert the copied value equals the source's, which may differ between
    // Windows builds. What catches the copy is the IDENTITY, not the clock: it differs from the one
    // taken before the copy, which is the comparison the witness makes after T1'.
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-copy" };
    scratch.Write("new.hpp", "MESSAGE_V2_NEW\n");
    auto const oldPath = scratch / "old.hpp";
    auto const newPath = scratch / "new.hpp";
    auto const anHourAgo = std::filesystem::file_time_type::clock::now() - std::chrono::hours { 1 };
    std::filesystem::last_write_time(newPath, anHourAgo);

    // The fence puts the next write in a later tick than every change to `new.hpp`, so the
    // destination's own change time cannot coincide with the one the copy brings.
    auto const files = MakeDiskInputFiles();
    static_cast<void>(files->Fence());
    scratch.Write("old.hpp", "MESSAGE_V1_OLD\n"); // Same length as new.hpp's content.
    auto const before = files->Identify(oldPath.string());
    REQUIRE(before.has_value());
    REQUIRE(CopyFileW(newPath.c_str(), oldPath.c_str(), FALSE) != 0);
    auto const after = files->Identify(oldPath.string());
    REQUIRE(after.has_value());

    // The trap is real on this machine: the copy carried the hour-old last-write time.
    CHECK(std::filesystem::last_write_time(oldPath) < anHourAgo + std::chrono::minutes { 1 });
    CHECK(Unwrap(before) != Unwrap(after));
}

TEST_CASE("Setting a file's last-write time back still moves its NTFS change time", "[input-files]")
{
    // The Windows twin of the utimensat case: what the seam reads is `ChangeTime`, which a backdated
    // `LastWriteTime` does not hide. A seam reading `LastWriteTime` instead would fail the last check.
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-backdate" };
    scratch.Write("a.hpp", "MESSAGE_V1_OLD\n");
    auto const path = scratch / "a.hpp";
    auto const files = MakeDiskInputFiles();
    scratch.Write("a.hpp", "MESSAGE_V2_NEW\n");
    // Fenced AFTER the write, so the write alone cannot satisfy the last check: only the backdate
    // below can move the change time past this instant.
    auto const before = files->Fence();
    auto const anHourAgo = std::filesystem::file_time_type::clock::now() - std::chrono::hours { 1 };
    std::filesystem::last_write_time(path, anHourAgo);
    auto const identity = files->Identify(path.string());
    REQUIRE(identity.has_value());

    // The trap is real: the last-write time now reads an hour old.
    CHECK(std::filesystem::last_write_time(path) < anHourAgo + std::chrono::minutes { 1 });
    CHECK(Unwrap(identity).changeTime >= before);
}
#else
TEST_CASE("Setting a file's modification time back still moves its change time", "[input-files]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-utimens" };
    scratch.Write("a.hpp", "MESSAGE_V1_OLD\n");
    auto const path = (scratch / "a.hpp").string();
    auto const files = MakeDiskInputFiles();
    auto const before = files->Now();
    scratch.Write("a.hpp", "MESSAGE_V2_NEW\n");
    std::array<timespec, 2> const anHourAgo { timespec { .tv_sec = ::time(nullptr) - 3600, .tv_nsec = 0 },
                                              timespec { .tv_sec = ::time(nullptr) - 3600, .tv_nsec = 0 } };
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), anHourAgo.data(), 0) == 0);
    auto const identity = files->Identify(path);
    REQUIRE(identity.has_value());
    CHECK(Unwrap(identity).changeTime >= before);
}

TEST_CASE("Retargeting a symlink changes the identity read through it", "[input-files]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-symlink" };
    scratch.Write("a.hpp", "same\n");
    scratch.Write("b.hpp", "same\n");
    auto const link = scratch / "h.hpp";
    std::filesystem::create_symlink(scratch / "a.hpp", link);
    auto const files = MakeDiskInputFiles();
    auto const first = files->Identify(link.string());
    std::filesystem::remove(link);
    std::filesystem::create_symlink(scratch / "b.hpp", link);
    auto const second = files->Identify(link.string());
    auto const target = files->Identify((scratch / "b.hpp").string());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(target.has_value());
    CHECK(Unwrap(first) != Unwrap(second));
    // The link is FOLLOWED: what is read through it is its target's identity, which no inode
    // reuse can produce by accident.
    CHECK(Unwrap(second) == Unwrap(target));
}
#endif

TEST_CASE("A rename over a file changes its identity, same bytes and size", "[input-files]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-rename" };
    scratch.Write("h.hpp", "same\n");
    scratch.Write("h.tmp", "same\n");
    auto const files = MakeDiskInputFiles();
    auto const first = files->Identify((scratch / "h.hpp").string());
    std::filesystem::rename(scratch / "h.tmp", scratch / "h.hpp");
    auto const second = files->Identify((scratch / "h.hpp").string());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto const before = Unwrap(first);
    auto const after = Unwrap(second);
    CHECK((before.fileIdHigh != after.fileIdHigh || before.fileIdLow != after.fileIdLow));
}

TEST_CASE("A rename over a file after a fence is stamped at or after it", "[input-files]")
{
    // #1650: a header replaced by a rename during the key's preprocess is caught by the T0 clock
    // rule only if the RENAME moves the change time read through the path -- the replacing file's
    // content was written BEFORE the fence, so only the rename itself happens after it.
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-rename-fence" };
    scratch.Write("h.hpp", "old\n");
    scratch.Write("h.tmp", "new\n");
    auto const files = MakeDiskInputFiles();
    auto const fence = files->Fence();
    std::filesystem::rename(scratch / "h.tmp", scratch / "h.hpp");
    auto const identity = files->Identify((scratch / "h.hpp").string());
    REQUIRE(identity.has_value());
    CHECK(ChangedAtOrAfter(Unwrap(identity).changeTime, fence));
}

TEST_CASE("A missing file has no identity and no hash", "[input-files]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-missing" };
    auto const files = MakeDiskInputFiles();
    auto const absent = (scratch / "absent.hpp").string();
    CHECK_FALSE(files->Identify(absent).has_value());
    CHECK(files->Hash(absent).empty());
}

TEST_CASE("A non-ASCII path is identified", "[input-files]")
{
    // Review focus 1: a path the seam could not open would make every TU under it Unwitnessed,
    // which silently stops caching it.
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-utf8" };
    scratch.Write("header-\xC3\xBC.hpp", "one\n");
    auto const files = MakeDiskInputFiles();
    auto const path = (scratch.Path().string() + "/header-\xC3\xBC.hpp");
    CHECK(files->Identify(path).has_value());
    CHECK_FALSE(files->Hash(path).empty());
}

namespace
{
/// Whole seconds in the seam's unit, for stamps of a known granularity.
constexpr FileInstant Seconds(std::int64_t count)
{
    return count * OneSecondOfFileTime;
}

/// Nanoseconds since the epoch, spelled as a FileInstant.
constexpr FileInstant Nanoseconds(std::int64_t count)
{
    return count;
}

/// Milliseconds in the seam's unit.
constexpr FileInstant Milliseconds(std::int64_t count)
{
    return count * 1'000'000;
}
} // namespace

TEST_CASE("A sub-second stamp is compared with the instant as it is", "[input-files]")
{
    // Not a whole second, so no granularity is inferred and nothing is floored.
    auto const instant = Seconds(5) + Milliseconds(700) + 7;
    CHECK(ChangedAtOrAfter(instant - 1, instant) == false);
    CHECK(ChangedAtOrAfter(instant, instant) == true);
}

TEST_CASE("A stamp on a whole odd second floors the instant to at least the second", "[input-files]")
{
    // A 1 s filesystem stamps a change at 5.9 s as 5 s. Unfloored, or floored only to 100 ms,
    // 5 s < 5.7 s would say "before". (A 2 s floor gives the same answers here as a 1 s one: for an
    // odd second no input tells the two rows apart.)
    CHECK(ChangedAtOrAfter(Seconds(5), Seconds(5) + Milliseconds(700)) == true);
    // The previous second: 5 s is certainly before anything that happened at or after 6.7 s.
    CHECK(ChangedAtOrAfter(Seconds(5), Seconds(6) + Milliseconds(700)) == false);
}

TEST_CASE("A whole even second is judged against the instant's two-second window", "[input-files]")
{
    // FAT stamps a change at 7.9 s as 6 s. A 1 s floor (7 s) would say "before".
    CHECK(ChangedAtOrAfter(Seconds(6), Seconds(7) + Milliseconds(700)) == true);
    // The previous 2 s window.
    CHECK(ChangedAtOrAfter(Seconds(4), Seconds(7) + Milliseconds(700)) == false);
}

TEST_CASE("A pre-epoch instant is floored toward negative infinity", "[input-files]")
{
    // -1.3 s lies in the 2 s window starting at -2 s; truncating toward zero would put it at 0 s.
    auto const instant = -Seconds(1) - Milliseconds(300);
    CHECK(ChangedAtOrAfter(-Seconds(2), instant) == true);
    CHECK(ChangedAtOrAfter(-Seconds(4), instant) == false);
}

TEST_CASE("A stamp on the 10 ms grid is judged against the instant's own 10 ms slot", "[input-files]")
{
    // Linux exFAT and vfat stamp every 10 ms: a change at 5.1239 s is stamped 5.120 s. 5.120 s is a
    // whole 10 ms and not a whole 100 ms, so its grid is 10 ms.
    auto const instant = Seconds(5) + Milliseconds(123);
    CHECK(ChangedAtOrAfter(Seconds(5) + Milliseconds(120), instant) == true);
    // The previous 10 ms slot.
    CHECK(ChangedAtOrAfter(Seconds(5) + Milliseconds(110), instant) == false);
}

TEST_CASE("A stamp on the 100 ns grid is compared with a 100 ns instant as it is", "[input-files]")
{
    // NTFS-shaped: both on the 100 ns grid and on no coarser one, so flooring changes nothing.
    auto const instant = Seconds(5) + Nanoseconds(123'456'700);
    CHECK(ChangedAtOrAfter(instant - Nanoseconds(100), instant) == false);
    CHECK(ChangedAtOrAfter(instant, instant) == true);
}
