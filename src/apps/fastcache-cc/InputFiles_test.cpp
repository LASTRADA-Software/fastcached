// SPDX-License-Identifier: Apache-2.0
#include "InputFiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;

namespace
{
/// One minute in the seam's unit, for the sanity bound on the epoch and the scale.
constexpr FileInstant OneMinute = std::int64_t { 60 } * 1'000'000'000;
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

#if defined(_WIN32)
TEST_CASE("CopyFileW keeps the last-write time and still moves the change time", "[input-files]")
{
    // #1650's deterministic variant: an mtime-only identity cannot see this copy at all.
    FastCache::Testing::ScratchDirectory const scratch { "fc-input-files-copy" };
    scratch.Write("old.hpp", "MESSAGE_V1_OLD\n");
    scratch.Write("new.hpp", "MESSAGE_V2_NEW\n");
    auto const oldPath = scratch / "old.hpp";
    auto const newPath = scratch / "new.hpp";
    auto const anHourAgo = std::filesystem::file_time_type::clock::now() - std::chrono::hours { 1 };
    std::filesystem::last_write_time(newPath, anHourAgo);

    auto const files = MakeDiskInputFiles();
    auto const before = files->Now();
    REQUIRE(CopyFileW(newPath.c_str(), oldPath.c_str(), FALSE) != 0);

    // The trap is real on this machine: the copy carried the hour-old last-write time.
    CHECK(std::filesystem::last_write_time(oldPath) < anHourAgo + std::chrono::minutes { 1 });
    auto const identity = files->Identify(oldPath.string());
    REQUIRE(identity.has_value());
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
