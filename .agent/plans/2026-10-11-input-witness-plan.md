# Input Witness Implementation Plan (#1650)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An edit saved while `fastcache-cc` is compiling a translation unit must never record a manifest or store an object that does not match the key it is filed under, and must cost cross-checkout and cross-machine sharing nothing.

**Architecture:** A new seam, `IInputFiles`, reads a file's identity (change time, size, volume, file id), hashes its content, and reads a coarse clock. `InputWitness` uses it to snapshot every input at three moments: T0 before the key's preprocess, T1′ after it, and T2 after the compile. From those snapshots it computes one `InputStability` verdict. An `EnumTable` maps the verdict to the STORE and manifest actions. `BuildManifest` stops reading files itself and takes each hash from an `IManifestContent`, which in production is the witness, so a manifest always describes the keyed content.

**Tech Stack:** C++23, Catch2, CMake. Win32 `GetFileInformationByHandleEx` and POSIX `stat`/`clock_gettime`. Bash and PowerShell 7 end-to-end fixtures.

**Spec:** `.agent/plans/2026-10-11-input-witness-design.md`. Read it first; this plan argues from it.

## Global Constraints

- Branch `feature/1650-input-witness`. Every commit's author and committer are `christian@parpart.family`, and every commit ends with `Signed-off-by: Christian Parpart <christian@parpart.family>`, never the `lastrada.net` address.
- Read `.agent/rules/compile-cache.md`, `.agent/rules/testing.md` and `.agent/rules/build-and-toolchain.md` before writing code. `AGENT.md` is the index.
- No C-style `for` loops anywhere under `src/`. Use range-for, `std::views::iota`, or `std::span{argv, argc}.subspan(1)` for argv.
- No `NOLINT`, no `k`/`g_` prefixes, no abbreviations (`Index`, never `Idx`). Every new public symbol gets a Doxygen comment, and every `std::expected` return is `[[nodiscard]]`.
- Yes/no parameters are `enum class`, never `bool`.
- A table indexed by an enumerator is `EnumTable<Enum, Row>` plus `RowsInEnumeratorOrder`. A new private enum has no explicit `= N` values and ends in `Last`.
- `FileIdentity`, `FileInstant` and `InputWitness` are never encoded, transmitted, persisted or folded into a key or manifest (spec invariant 1).
- The `ComputeManifestKey` tag moves from `manifest-v7` to `manifest-v8`. `objkey-v*` does NOT move.
- The new statistics token is `INPUTS-MOVED`. Existing tokens are uppercase words; `VERIFY-MISMATCH` is the precedent.
- Format with the pinned clang-format, resolved by `bash scripts/check-clang-format-version.sh --resolve`. Never run a `clang-format -i` that is not that build.
- Windows builds: `cmake --preset cl-debug` and `cmake --build --preset cl-debug --target fastcache-cc-tests` from a VS developer shell. Unit tests: `out/build/cl-debug/src/apps/fastcache-cc/fastcache-cc-tests.exe "[tag]"`. If the build directory differs, find the binary with `Get-ChildItem out/build/cl-debug -Recurse -Filter fastcache-cc-tests.exe`.
- A Catch2 run selecting a subset asserts how many cases ran (testing rule): read the `test cases:` line of the output, and never select by a name containing a comma.

## Review Focus

1. **A UTF-8 (non-ASCII) path.** It must be identified like any other. If it were not, every TU under such a directory would silently stop caching (`Unwitnessed`). Test: Task 1, step 1, case "A non-ASCII path is identified".
2. **A header reached through a symlink that is retargeted mid-compile** (POSIX). Identity follows the link target, so the retarget is a change. Test: Task 1, step 1, case "Retargeting a symlink changes the identity read through it".
3. **A header DELETED during the compile.** This must be `MovedDuringCompile` (no store, manifest kept), not a crash and not `Stable`. Test: Task 3, step 1, case "A header deleted during the compile is MovedDuringCompile".
4. **A file rewritten with identical bytes during the compile** (a build tool re-generating a header). It fails closed to `MovedDuringCompile`, because an interleaving cannot be excluded. The build still succeeds, and the next invocation stores. Test: Task 3, step 1, case "Same bytes, new identity is still moved".
5. **A file last changed in the same coarse clock tick as T0.** Counted as moved (fail closed), never as stable. Test: Task 3, step 1, case "A change time equal to T0 counts as moved".

---

### Task 1: The `IInputFiles` seam and its disk implementation

**Files:**
- Create: `src/apps/fastcache-cc/InputFiles.hpp`
- Create: `src/apps/fastcache-cc/InputFiles.cpp`
- Create: `src/apps/fastcache-cc/InputFiles_test.cpp`
- Modify: `src/apps/fastcache-cc/CMakeLists.txt`: add `InputFiles.cpp` to `_fc_cc_sources` (after `HitVerification.cpp`) and `InputFiles_test.cpp` to `fastcache-cc-tests` (after `HitVerification_test.cpp`).

**Interfaces:**
- Consumes: `FastCache::Cc::HashFileContents(std::string_view)` (`DirectManifest.hpp`) and `FastCache::PathFromNarrowText(std::string_view)` (`FastCache/Platform/NarrowText.hpp`, already in `_fc_cc_core`).
- Produces:
  - `using FastCache::Cc::FileInstant = std::int64_t;` (nanoseconds since the Unix epoch);
  - `struct FileIdentity { FileInstant changeTime; std::uint64_t size; std::uint64_t volumeId; std::uint64_t fileIdHigh; std::uint64_t fileIdLow; }` with a defaulted `operator==`;
  - `class IInputFiles` with `Identify(std::string_view) const -> std::optional<FileIdentity>`, `Hash(std::string_view) const -> std::string` and `Now() const -> FileInstant`;
  - `MakeDiskInputFiles() -> std::unique_ptr<IInputFiles>`.

- [ ] **Step 1: Write the failing tests**

`src/apps/fastcache-cc/InputFiles_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "InputFiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <tests/ScratchPath.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

using namespace FastCache::Cc;

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
    auto const distance = files->Now() - identity->changeTime;
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
    CHECK(identity->changeTime >= before);
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
    CHECK(identity->changeTime >= before);
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
    CHECK(identity->changeTime >= before);
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
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);
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
    CHECK((first->fileIdHigh != second->fileIdHigh || first->fileIdLow != second->fileIdLow));
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
```

On Windows `scratch.Path().string()` must return UTF-8. The process code page is UTF-8 (`build-and-toolchain.md`). If `Path().string()` is not UTF-8 on this toolchain, build the path with `FastCache::Utf8FromNarrowText` or the helper `ScratchPath.hpp` uses. Do not weaken the case.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset cl-debug --target fastcache-cc-tests`.
Expected: a compile error, because `InputFiles.hpp` does not exist.

- [ ] **Step 3: Write the header**

`src/apps/fastcache-cc/InputFiles.hpp`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

`src/apps/fastcache-cc/InputFiles.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "InputFiles.hpp"

#include "DirectManifest.hpp"

#include <FastCache/Platform/NarrowText.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <time.h>
#endif

#include <cstring>

namespace FastCache::Cc
{

namespace
{
#if defined(_WIN32)
    /// 1970-01-01 in FILETIME ticks (100 ns since 1601-01-01).
    constexpr std::int64_t UnixEpochAsFileTime = 116'444'736'000'000'000;

    [[nodiscard]] FileInstant FromFileTime(std::int64_t ticks) noexcept
    {
        return (ticks - UnixEpochAsFileTime) * 100;
    }

    /// Closes a handle on every path out.
    class HandleCloser
    {
      public:
        explicit HandleCloser(HANDLE handle) noexcept: _handle { handle } {}
        HandleCloser(HandleCloser const&) = delete;
        HandleCloser& operator=(HandleCloser const&) = delete;
        ~HandleCloser() { CloseHandle(_handle); }

      private:
        HANDLE _handle;
    };
#endif

    class DiskInputFiles final: public IInputFiles
    {
      public:
        [[nodiscard]] std::optional<FileIdentity> Identify(std::string_view path) const override
        {
#if defined(_WIN32)
            auto const native = PathFromNarrowText(path);
            if (!native.has_value())
                return std::nullopt;
            HANDLE const handle = CreateFileW(native->c_str(),
                                              FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_FLAG_BACKUP_SEMANTICS,
                                              nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return std::nullopt;
            HandleCloser const closer { handle };

            FILE_BASIC_INFO basic {};
            FILE_STANDARD_INFO standard {};
            if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof basic) == 0
                || GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof standard) == 0)
                return std::nullopt;

            // FAT and exFAT report no change time; their last-write time is the best there is.
            auto const changeTicks =
                basic.ChangeTime.QuadPart != 0 ? basic.ChangeTime.QuadPart : basic.LastWriteTime.QuadPart;
            FileIdentity identity { .changeTime = FromFileTime(changeTicks),
                                    .size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) };

            FILE_ID_INFO id {};
            if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof id) != 0)
            {
                identity.volumeId = id.VolumeSerialNumber;
                std::memcpy(&identity.fileIdLow, id.FileId.Identifier, sizeof identity.fileIdLow);
                std::memcpy(&identity.fileIdHigh,
                            id.FileId.Identifier + sizeof identity.fileIdLow,
                            sizeof identity.fileIdHigh);
                return identity;
            }
            BY_HANDLE_FILE_INFORMATION legacy {};
            if (GetFileInformationByHandle(handle, &legacy) == 0)
                return std::nullopt;
            identity.volumeId = legacy.dwVolumeSerialNumber;
            identity.fileIdLow = (std::uint64_t { legacy.nFileIndexHigh } << 32U) | legacy.nFileIndexLow;
            return identity;
#else
            struct stat status {};
            if (::stat(std::string { path }.c_str(), &status) != 0)
                return std::nullopt;
    #if defined(__APPLE__)
            auto const& changed = status.st_ctimespec;
    #else
            auto const& changed = status.st_ctim;
    #endif
            return FileIdentity { .changeTime = (static_cast<FileInstant>(changed.tv_sec) * 1'000'000'000) + changed.tv_nsec,
                                  .size = static_cast<std::uint64_t>(status.st_size),
                                  .volumeId = static_cast<std::uint64_t>(status.st_dev),
                                  .fileIdHigh = 0,
                                  .fileIdLow = static_cast<std::uint64_t>(status.st_ino) };
#endif
        }

        [[nodiscard]] std::string Hash(std::string_view path) const override
        {
            return HashFileContents(path);
        }

        [[nodiscard]] FileInstant Now() const override
        {
#if defined(_WIN32)
            FILETIME now {};
            GetSystemTimeAsFileTime(&now);
            return FromFileTime(static_cast<std::int64_t>((std::uint64_t { now.dwHighDateTime } << 32U) | now.dwLowDateTime));
#else
            timespec now {};
    #if defined(CLOCK_REALTIME_COARSE)
            ::clock_gettime(CLOCK_REALTIME_COARSE, &now);
    #else
            ::clock_gettime(CLOCK_REALTIME, &now);
    #endif
            return (static_cast<FileInstant>(now.tv_sec) * 1'000'000'000) + now.tv_nsec;
#endif
        }
    };
} // namespace

std::unique_ptr<IInputFiles> MakeDiskInputFiles()
{
    return std::make_unique<DiskInputFiles>();
}

} // namespace FastCache::Cc
```

- [ ] **Step 5: Register the files in CMake**

In `src/apps/fastcache-cc/CMakeLists.txt`:
- add `InputFiles.cpp` on its own line after `HitVerification.cpp` in `set(_fc_cc_sources ...)`;
- add `InputFiles_test.cpp` on its own line after `HitVerification_test.cpp` in `add_executable(fastcache-cc-tests ...)`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset cl-debug --target fastcache-cc-tests`, then `out/build/cl-debug/src/apps/fastcache-cc/fastcache-cc-tests.exe "[input-files]"`.
Expected: PASS. On Windows, `test cases: 6 | 6 passed` (the two POSIX cases are compiled out). On Linux, 7.

- [ ] **Step 7: Commit**

```bash
git add src/apps/fastcache-cc/InputFiles.hpp src/apps/fastcache-cc/InputFiles.cpp src/apps/fastcache-cc/InputFiles_test.cpp src/apps/fastcache-cc/CMakeLists.txt
git commit -s -m "cc: an input file's identity, content and clock behind one seam (#1650)"
```

---

### Task 2: `BuildManifest` takes its hashes from a content source, and the manifest tag moves

**Files:**
- Modify: `src/apps/fastcache-cc/DirectManifest.hpp`:
  - `ManifestFault` (line ~104) and `FaultTable` (line ~160);
  - add `IManifestContent` before `ManifestInputs`;
  - add a field to `ManifestInputs`;
  - declare `ProjectDependencyPaths` after `ResolveAgainst`.
- Modify: `src/apps/fastcache-cc/DirectManifest.cpp`:
  - `BuildManifest` (`:760-858`), the `record` lambda (`:790-796`);
  - `ComputeManifestKey`'s `KeyDigest digest { "manifest-v7" }` (`:995`);
  - define `ProjectDependencyPaths`.
- Create: `src/apps/fastcache-cc/ManifestContentTestSupport.hpp`.
- Modify: `src/apps/fastcache-cc/DirectManifest_test.cpp`:
  - all 16 `BuildManifest(` call sites gain `.content`;
  - the pinned-digest case (`:1672`);
  - new cases.
- Modify: `src/apps/fastcache-cc/main.cpp:2120-2127`. A temporary `.content`, so the tree builds; Task 5 replaces it.

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `ManifestFault::ChangedDuringInvocation` (appended before `Last`);
  - `class IManifestContent` with `HashAsKeyed(std::string_view resolvedPath) const -> std::expected<std::string, ManifestFault>`;
  - a new last field of `ManifestInputs`: `IManifestContent const& content;`;
  - `ProjectDependencyPaths(std::span<std::string const> reportedPaths, std::string_view workingDirectory, PathCanon::Layout const& layout) -> std::vector<std::string>`;
  - test-only `FastCache::Cc::Test::HashedWhenAsked`.

- [ ] **Step 1: Write the failing tests**

`src/apps/fastcache-cc/ManifestContentTestSupport.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DirectManifest.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc::Test
{

/// Hashes each file at the moment `BuildManifest` asks. That is RECORD time, which is what
/// `BuildManifest` did by itself before #1650, and it is right for every case here that is about
/// CLASSIFICATION rather than timing. Test-only on purpose: production hands `BuildManifest` an
/// input witness, so nothing it records can have been read after the compile.
class HashedWhenAsked final: public IManifestContent
{
  public:
    [[nodiscard]] std::expected<std::string, ManifestFault> HashAsKeyed(std::string_view resolvedPath) const override
    {
        auto hash = HashFileContents(resolvedPath);
        if (hash.empty())
            return std::unexpected(ManifestFault::Unreadable);
        return hash;
    }
};

/// Answers from a fixed script and records every path it was asked about, so a case can assert
/// that `BuildManifest` took its hashes from here and asked about exactly the paths an input
/// witness would have covered.
class ScriptedContent final: public IManifestContent
{
  public:
    /// One scripted answer.
    struct Answer
    {
        std::string path;                                   ///< Resolved path asked about.
        std::expected<std::string, ManifestFault> response; ///< What to answer.
    };

    /// @param answers The script; a path missing from it answers `Unreadable`.
    explicit ScriptedContent(std::vector<Answer> answers): _answers { std::move(answers) } {}

    [[nodiscard]] std::expected<std::string, ManifestFault> HashAsKeyed(std::string_view resolvedPath) const override
    {
        _asked.emplace_back(resolvedPath);
        for (auto const& answer: _answers)
            if (answer.path == resolvedPath)
                return answer.response;
        return std::unexpected(ManifestFault::Unreadable);
    }

    /// @return Every path asked about, in order.
    [[nodiscard]] std::vector<std::string> const& Asked() const noexcept { return _asked; }

  private:
    std::vector<Answer> _answers;
    mutable std::vector<std::string> _asked;
};

} // namespace FastCache::Cc::Test
```

Append to `src/apps/fastcache-cc/DirectManifest_test.cpp` (and add `#include "ManifestContentTestSupport.hpp"` beside the other local includes):

```cpp
TEST_CASE("BuildManifest takes every hash from its content source, never from the disk", "[direct-manifest][1650]")
{
    // #1650: the hashes BuildManifest recorded were read AFTER the compile, so a save during it
    // paired the new content with the old object. The content source is the witness in
    // production; this proves BuildManifest no longer reads the files itself.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-content-source" };
    scratch.Write("src/header.hpp", "#pragma once\n");
    scratch.Write("src/a.cpp", "#include \"header.hpp\"\n");
    auto const& root = scratch.Path();
    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    auto const source = (root / "src" / "a.cpp").string();
    auto const header = (root / "src" / "header.hpp").string();
    auto const resolvedSource = ResolveAgainst(source, root.string(), layout);
    auto const resolvedHeader = ResolveAgainst(header, root.string(), layout);

    Test::ScriptedContent const content { { { .path = resolvedSource, .response = std::string { "tu-as-keyed" } },
                                            { .path = resolvedHeader, .response = std::string { "header-as-keyed" } } } };
    auto const built = BuildManifest({ .sourcePath = source,
                                       .reportedDependencies = ReportedDependencies::Observed({ header }),
                                       .workingDirectory = root.string(),
                                       .toolchainStamp = "stamp",
                                       .objectKey = "objkey",
                                       .content = content },
                                     layout);
    REQUIRE(built.has_value());
    std::vector<std::string> hashes;
    for (auto const& entry: built->entries)
        hashes.push_back(entry.contentHash);
    std::ranges::sort(hashes);
    CHECK(hashes == std::vector<std::string> { "header-as-keyed", "tu-as-keyed" });

    // And it asked about exactly the TU plus what ProjectDependencyPaths names, so a witness
    // taken over that list answers every lookup.
    auto expected = ProjectDependencyPaths(std::vector<std::string> { header }, root.string(), layout);
    expected.insert(expected.begin(), resolvedSource);
    CHECK(content.Asked() == expected);
}

TEST_CASE("A content source's refusal names its path and its fault", "[direct-manifest][1650]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-content-refusal" };
    scratch.Write("src/header.hpp", "#pragma once\n");
    scratch.Write("src/a.cpp", "#include \"header.hpp\"\n");
    auto const& root = scratch.Path();
    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    auto const source = (root / "src" / "a.cpp").string();
    auto const header = (root / "src" / "header.hpp").string();
    auto const resolvedHeader = ResolveAgainst(header, root.string(), layout);

    Test::ScriptedContent const content {
        { { .path = ResolveAgainst(source, root.string(), layout), .response = std::string { "tu" } },
          { .path = resolvedHeader, .response = std::unexpected(ManifestFault::ChangedDuringInvocation) } }
    };
    auto const built = BuildManifest({ .sourcePath = source,
                                       .reportedDependencies = ReportedDependencies::Observed({ header }),
                                       .workingDirectory = root.string(),
                                       .toolchainStamp = "stamp",
                                       .objectKey = "objkey",
                                       .content = content },
                                     layout);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == ManifestFailure { .fault = ManifestFault::ChangedDuringInvocation, .path = resolvedHeader });
    CHECK(DescribeManifestFailure(built.error()) == "changed during this compile: " + resolvedHeader);
}

TEST_CASE("ProjectDependencyPaths keeps project headers, resolved, and drops toolchain ones", "[direct-manifest][1650]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-project-paths" };
    auto const& root = scratch.Path();
    FastCache::PathCanon::Layout const layout { .sourceRoot = (root / "src").string(),
                                                .buildTree = (root / "out").string() };
    auto const project = (root / "src" / "inc" / ".." / "h.hpp").string();
    auto const toolchain = (root / "sdk" / "vector").string();
    auto const kept = ProjectDependencyPaths(std::vector<std::string> { project, toolchain }, root.string(), layout);
    CHECK(kept == std::vector<std::string> { ResolveAgainst(project, root.string(), layout) });
}
```

Update the pinned-digest case at `DirectManifest_test.cpp:1672`:
- Add a retired row, after the `manifest-v6` row, with the comment `// Retired by #1650: a v7 manifest's hashes were read after the compile, so an edit saved during it paired the new content with the old object. Only the tag moved.` The row is `{ .key = "manifest-v7", .digest = "c9ccdb2f69e66935265a024bef7b0c5e" },`. Only the tag changes, so the outgoing vector IS the v7 digest; the case's own comment explains this.
- Replace `CHECK(manifestKey == "c9ccdb2f69e66935265a024bef7b0c5e");` with the value the test prints after Step 3. Run it once, copy the `manifestKey` it reports as actual, and paste it.

For every existing `BuildManifest(` call in `DirectManifest_test.cpp` and every `ManifestInputs {` construction:
- add `.content = hashedWhenAsked` as the LAST designated initializer;
- declare `Test::HashedWhenAsked const hashedWhenAsked;` at the top of each such case, or once in the file's anonymous namespace as `Test::HashedWhenAsked const hashedWhenAsked;`.

These cases are about classification, so record-time hashing is right for them. Run `grep -c "BuildManifest(" src/apps/fastcache-cc/DirectManifest_test.cpp` before and after. The count of calls grows only by the new cases, and every one carries `.content`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset cl-debug --target fastcache-cc-tests`.
Expected: a compile error, because `IManifestContent`, `ProjectDependencyPaths`, `ChangedDuringInvocation` and the `content` field do not exist.

- [ ] **Step 3: Implement**

In `DirectManifest.hpp`, append the fault before `Last`:

```cpp
    /// The file changed after this invocation began, so its content now is not the content the
    /// key was taken from (#1650). Reached only by the one path the input witness did not
    /// snapshot: the build's own depfile, read after the compile when the probe reported nothing.
    ChangedDuringInvocation,
```

Append its row to `FaultTable`:

```cpp
    { .fault = ManifestFault::ChangedDuringInvocation, .label = "changed during this compile" },
```

Before `struct ManifestInputs`, add:

```cpp
/// Where `BuildManifest` takes each entry's content hash from.
///
/// A manifest asserts "these hashes reach that object", so its hashes must be the content the
/// object's KEY was derived from. Hashing at record time, after the compile, asserted whatever a
/// save during the compile had left on disk instead (#1650). In production this is the input
/// witness, which hashed every file right after the key's preprocess.
class IManifestContent
{
  public:
    virtual ~IManifestContent() = default;

    /// @param resolvedPath A path as `ResolveAgainst` resolved it.
    /// @return Its content hash as of the key, or why that cannot be known.
    [[nodiscard]] virtual std::expected<std::string, ManifestFault> HashAsKeyed(std::string_view resolvedPath) const = 0;
};
```

Add, as the LAST member of `ManifestInputs`:

```cpp
    /// Where each entry's hash comes from. A reference with no default, so a caller cannot
    /// leave the question unanswered and fall back to reading the disk at record time (#1650).
    IManifestContent const& content;
```

After `ResolveAgainst`'s declaration, declare:

```cpp
/// The reported dependencies a manifest would hash, resolved exactly as `BuildManifest` resolves
/// them and kept only where `BuildManifest` keeps them (project paths).
///
/// One function for both sides, so an input witness taken over this list answers every lookup
/// `BuildManifest` makes. Paths `BuildManifest` would REFUSE (unanchored, a near-miss root) are
/// dropped here rather than refused: the refusal is still `BuildManifest`'s to make, by name.
/// @param reportedPaths    Dependencies as the compiler spelled them.
/// @param workingDirectory The compile's working directory.
/// @param layout           This build's roots.
/// @return The resolved project paths, in reported order, duplicates removed.
[[nodiscard]] std::vector<std::string> ProjectDependencyPaths(std::span<std::string const> reportedPaths,
                                                              std::string_view workingDirectory,
                                                              PathCanon::Layout const& layout);
```

Add `#include <span>` to the header.

In `DirectManifest.cpp`, define it beside `BuildManifest`, reusing the file-local `ClassifyResolved`:

```cpp
std::vector<std::string> ProjectDependencyPaths(std::span<std::string const> reportedPaths,
                                                std::string_view workingDirectory,
                                                PathCanon::Layout const& layout)
{
    std::vector<std::string> kept;
    for (auto const& rawPath: reportedPaths)
    {
        auto resolved = ResolveAgainst(rawPath, workingDirectory, layout);
        if (ClassifyResolved(resolved, layout) == PathRole::Project && !std::ranges::contains(kept, resolved))
            kept.push_back(std::move(resolved));
    }
    return kept;
}
```

If `std::ranges::contains` is not available on one of the CI standard libraries, use `std::ranges::find(kept, resolved) == kept.end()`.

In `BuildManifest`, replace the `record` lambda's body:

```cpp
    auto const record = [&manifest, &inputs](std::string token, std::string const& resolved) -> std::expected<void, ManifestFailure> {
        auto hash = inputs.content.HashAsKeyed(resolved);
        if (!hash.has_value())
            return std::unexpected(ManifestFailure { .fault = hash.error(), .path = resolved });
        manifest.entries.push_back({ .canonicalPath = std::move(token), .contentHash = *std::move(hash) });
        return {};
    };
```

Rewrite the comment above it to say the hash is the content as of the key (#1650), not as of now.

In `ComputeManifestKey`, change `KeyDigest digest { "manifest-v7" };` to `KeyDigest digest { "manifest-v8" };`. Add one sentence to its comment: v8 retires every manifest recorded before #1650, whose hashes were read after the compile; `objkey` did not move, because the objects under their keys were never the defect.

In `main.cpp:2120`, so the tree builds until Task 5, add `.content = recordTimeContent` to the `Cc::BuildManifest({...})` call. Declare it just above as `Cc::Test::HashedWhenAsked const recordTimeContent; // Replaced by the input witness in Task 5 (#1650).`, and include `ManifestContentTestSupport.hpp` in `main.cpp`. Do NOT add a record-time class to production code. Task 5 removes both the include and the object.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `out/build/cl-debug/src/apps/fastcache-cc/fastcache-cc-tests.exe "[direct-manifest]"`, then the whole binary with no filter.
Expected: everything PASSES. The pinned-digest case fails once with the new v8 digest; paste that value as described in Step 1, then it passes.

- [ ] **Step 5: Commit**

```bash
git add src/apps/fastcache-cc/DirectManifest.hpp src/apps/fastcache-cc/DirectManifest.cpp src/apps/fastcache-cc/DirectManifest_test.cpp src/apps/fastcache-cc/ManifestContentTestSupport.hpp src/apps/fastcache-cc/main.cpp
git commit -s -m "cc: a manifest's hashes come from a content source, and manifest-v8 retires every v7 manifest (#1650)"
```

---

### Task 3: `InputWitness`, the verdict and its table

**Files:**
- Create: `src/apps/fastcache-cc/InputWitness.hpp`
- Create: `src/apps/fastcache-cc/InputWitness.cpp`
- Create: `src/apps/fastcache-cc/InputWitness_test.cpp`
- Modify: `src/apps/fastcache-cc/CMakeLists.txt`: add `InputWitness.cpp` to `_fc_cc_sources` after `InputFiles.cpp`, and `InputWitness_test.cpp` to the tests after `InputFiles_test.cpp`.

**Interfaces:**
- Consumes: Task 1's `IInputFiles`, `FileIdentity` and `FileInstant`. Task 2's `IManifestContent`, `ManifestFault::ChangedDuringInvocation`, `ManifestInputs::content`, `ProjectDependencyPaths`, `BuildManifest` and `EncodeManifest`. Also `ComputeManifestKey` and `CanonicalSourceToken`.
- Produces:
  - `enum class InputStability : std::uint8_t { Stable, MovedDuringCompile, MovedBeforeKey, Unwitnessed, Last };`
  - `enum class StoreObject : std::uint8_t { Yes, No };`
  - `enum class KeepManifest : std::uint8_t { Yes, No };`
  - `enum class ContentHashing : std::uint8_t { Yes, No };`
  - `struct StabilityRow { InputStability stability; StoreObject store; KeepManifest manifest; std::string_view sentence; };`
  - `inline constexpr EnumTable<InputStability, StabilityRow> StabilityTable`
  - `RowOf(InputStability) -> StabilityRow const&`
  - `struct StabilityFinding { InputStability stability; std::string path; };`
  - `DescribeInstability(StabilityFinding const&) -> std::string`
  - `class InputWitness`:
    - `static Begin(std::string resolvedSource, IInputFiles const&, ContentHashing) -> InputWitness`
    - `Snapshot(std::span<std::string const> resolvedProjectPaths, IInputFiles const&) -> void`
    - `AtKey() const -> StabilityFinding`
    - `AfterCompile(std::span<std::string const> lateResolvedPaths, IInputFiles const&) const -> StabilityFinding`
    - `KeyedHash(std::string_view) const -> std::optional<std::string>`
    - `Started() const -> FileInstant`
  - `class WitnessedManifestContent final : public IManifestContent`, constructed as `WitnessedManifestContent(InputWitness const&, IInputFiles const&)`.

**The rules, which `InputWitness.cpp` implements exactly:**

- **`Begin`** (T0): `started = files.Now()`. Then for the TU: `atStart = Identify(source)`, and `hashAtStart = Hash(source)` if hashing is on.
- **`Snapshot`** (T1′): the entries are the TU followed by each project path not equal to the TU, in order.
  - For each entry: `before = Identify(p)`, then `hash = Hash(p)` if hashing is on, then `after = Identify(p)`. Store `after` and `hash`.
  - The first finding in entry order (TU first) becomes `atKey`. Within one entry, the first matching rule wins:
    1. `before` or `after` is missing, or hashing is on and `hash` is empty: `Unwitnessed`.
    2. `before != after`: `MovedBeforeKey`.
    3. `ChangedAtOrAfter(after->changeTime, started)` (Task 1's comparison, never a raw `>=`): `MovedBeforeKey`.
    4. TU only: `atStart` is missing gives `Unwitnessed`; `*atStart != *after` gives `MovedBeforeKey`; hashing is on and `hashAtStart != hash` gives `MovedBeforeKey`.
  - If nothing matches, `atKey` is `Stable`.
- **`AfterCompile`** (T2):
  - If `atKey` is not `Stable`, return it.
  - For each entry: `now = Identify(p)`. If `now` is missing, or `*now != stored`: `MovedDuringCompile`.
  - No clock comparison is needed here. Every entry's T1′ stamp lies below T0 floored to that stamp's own granularity (rule 3 above), and any rewrite after T0 is stamped at or above that floor, so the identity always differs, on a whole-second filesystem as well. A clock check at T2 would be redundant.
  - Then for each late path that is not an entry: `now = Identify(p)`. Missing gives `Unwitnessed`; `ChangedAtOrAfter(now->changeTime, started)` gives `MovedDuringCompile`.
  - Otherwise `Stable`.
- **`KeyedHash(p)`**: the stored hash of the entry whose path equals `p`, if it is non-empty; otherwise nullopt.
- **`WitnessedManifestContent::HashAsKeyed(p)`**:
  - if the witness has a `KeyedHash(p)`, return it;
  - otherwise `id = Identify(p)`; missing gives `Unreadable`, and `ChangedAtOrAfter(id->changeTime, Started())` gives `ChangedDuringInvocation`;
  - then `hash = Hash(p)`; empty gives `Unreadable`;
  - `Identify(p) != id` gives `ChangedDuringInvocation`;
  - otherwise return `hash`.

The table rows, in enumerator order:

<!-- table-total: none -->
| stability | store | manifest | sentence |
|---|---|---|---|
| `Stable` | `Yes` | `Yes` | `""` |
| `MovedDuringCompile` | `No` | `Yes` | `"an input changed during the compile"` |
| `MovedBeforeKey` | `No` | `No` | `"an input changed while the key was taken"` |
| `Unwitnessed` | `No` | `No` | `"an input could not be observed"` |

`DescribeInstability(f)` returns `std::format("{}: {}", RowOf(f.stability).sentence, f.path)`, or `""` for `Stable`.

- [ ] **Step 1: Write the failing tests**

`src/apps/fastcache-cc/InputWitness_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "InputWitness.hpp"

#include "DirectManifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache::Cc;

namespace
{
/// Identities and contents a case sets by hand, and a clock it moves by hand.
class FakeInputFiles final: public IInputFiles
{
  public:
    std::map<std::string, FileIdentity, std::less<>> identities;
    std::map<std::string, std::string, std::less<>> hashes;
    FileInstant now { 1'000 };

    [[nodiscard]] std::optional<FileIdentity> Identify(std::string_view path) const override
    {
        if (!identities.contains(path))
            return std::nullopt;
        return identities.at(std::string { path });
    }
    [[nodiscard]] std::string Hash(std::string_view path) const override
    {
        return hashes.contains(path) ? hashes.at(std::string { path }) : std::string {};
    }
    [[nodiscard]] FileInstant Now() const override { return now; }

    /// Put @p path at content @p hash, changed at @p changeTime, with id @p id.
    void Set(std::string const& path, std::string hash, FileInstant changeTime, std::uint64_t id)
    {
        identities[path] = FileIdentity { .changeTime = changeTime, .size = hash.size(), .volumeId = 1, .fileIdHigh = 0, .fileIdLow = id };
        hashes[path] = std::move(hash);
    }
};

constexpr std::string_view Tu = "/r/src/a.cpp";
constexpr std::string_view Header = "/r/src/h.hpp";

/// A quiet TU and header, both last changed long before T0 (= 1000).
FakeInputFiles QuietFiles()
{
    FakeInputFiles files;
    files.Set(std::string { Tu }, "tu-v1", 10, 1);
    files.Set(std::string { Header }, "h-v1", 10, 2);
    return files;
}

std::vector<std::string> const HeaderOnly { std::string { Header } };
} // namespace

TEST_CASE("Nothing moved is Stable, at the key and after the compile", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Stable, .path = "" });
    files.now = 3'000;
    CHECK(witness.AfterCompile({}, files) == StabilityFinding { .stability = InputStability::Stable, .path = "" });
    CHECK(witness.KeyedHash(Tu) == std::optional<std::string> { "tu-v1" });
    CHECK(witness.KeyedHash(Header) == std::optional<std::string> { "h-v1" });
}

TEST_CASE("A TU saved during the compile is MovedDuringCompile, and names the TU", "[input-witness][1650]")
{
    // The incident: the save landed after the key and before the launcher returned.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.Set(std::string { Tu }, "tu-v2", 2'500, 1);
    CHECK(witness.AfterCompile({}, files)
          == StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = std::string { Tu } });
    // The manifest still gets the keyed content, not the saved one.
    CHECK(witness.KeyedHash(Tu) == std::optional<std::string> { "tu-v1" });
}

TEST_CASE("Same bytes, new identity is still moved", "[input-witness][1650]")
{
    // Review focus 4. A content-only bracket would call this stable; an interleaving (V2 read by
    // the compiler, then V1 restored) cannot be excluded, so it is not.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.Set(std::string { Header }, "h-v1", 10, 99); // same bytes, same old change time, new id
    CHECK(witness.AfterCompile({}, files).stability == InputStability::MovedDuringCompile);
}

TEST_CASE("A header deleted during the compile is MovedDuringCompile", "[input-witness][1650]")
{
    // Review focus 3.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.identities.erase(std::string { Header });
    CHECK(witness.AfterCompile({}, files)
          == StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = std::string { Header } });
}

TEST_CASE("A same-size, same-id rewrite after the snapshot is seen through its change time", "[input-witness][1650]")
{
    // Size and file id unchanged, so only the change time can tell. It must: every entry's
    // T1' change time is before T0, and any rewrite after T1' is stamped at or after T0.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.identities[std::string { Header }].changeTime = 2'000;
    CHECK(witness.AfterCompile({}, files).stability == InputStability::MovedDuringCompile);
}

TEST_CASE("A header changed while the key was taken is MovedBeforeKey", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.Set(std::string { Header }, "h-v2", 1'500, 2); // after T0 = 1000, before the snapshot
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey()
          == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Header } });
    CHECK(witness.AfterCompile({}, files).stability == InputStability::MovedBeforeKey);
}

TEST_CASE("A TU whose content moved between T0 and the snapshot is MovedBeforeKey, without a clock", "[input-witness][1650]")
{
    // A filesystem whose clock lags this machine's (a network share) stamps the change BEFORE T0;
    // the TU's own T0/T1' comparison still sees it.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.Set(std::string { Tu }, "tu-v2", 10, 1); // same size, change time and id: only the hash differs
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Tu } });
}

TEST_CASE("A change time equal to T0 counts as moved, one tick earlier does not", "[input-witness][1650]")
{
    // Review focus 5: the boundary is >=, so a change in T0's own coarse tick fails closed.
    for (auto const [changeTime, expected]: { std::pair { FileInstant { 1'000 }, InputStability::MovedBeforeKey },
                                              std::pair { FileInstant { 999 }, InputStability::Stable } })
    {
        auto files = QuietFiles();
        files.Set(std::string { Header }, "h-v1", changeTime, 2);
        auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
        files.now = 2'000;
        witness.Snapshot(HeaderOnly, files);
        CHECK(witness.AtKey().stability == expected);
    }
}

TEST_CASE("A file that cannot be read at the snapshot is Unwitnessed", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.hashes.erase(std::string { Header });
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Unwitnessed, .path = std::string { Header } });
}

TEST_CASE("With hashing off nothing is hashed and an unreadable hash is not Unwitnessed", "[input-witness][1650]")
{
    // Direct mode off: no manifest, so the witness guards the STORE by identity alone.
    auto files = QuietFiles();
    files.hashes.clear();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::No);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey().stability == InputStability::Stable);
    CHECK_FALSE(witness.KeyedHash(Header).has_value());
}

TEST_CASE("A late dependency changed after T0 is MovedDuringCompile; one unreadable is Unwitnessed", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot({}, files);
    files.Set("/r/src/late.hpp", "late", 1'500, 7);
    std::vector<std::string> const late { "/r/src/late.hpp" };
    CHECK(witness.AfterCompile(late, files)
          == StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = "/r/src/late.hpp" });
    std::vector<std::string> const missing { "/r/src/missing.hpp" };
    CHECK(witness.AfterCompile(missing, files).stability == InputStability::Unwitnessed);
}

TEST_CASE("A whole-second stamp in T0's own second counts as moved", "[input-witness][1650]")
{
    // A filesystem that stamps whole seconds (HFS+, ext4 with 128-byte inodes) stamps an edit made
    // at 5.9 s as 5.0 s; with T0 = 5.7 s a raw >= would call it older than the invocation. The
    // comparison is ChangedAtOrAfter, which floors T0 to the stamp's own granularity (Ruling 5).
    FakeInputFiles files;
    files.now = 5'700'000'000;
    files.Set(std::string { Tu }, "tu-v1", 10, 1);
    files.Set(std::string { Header }, "h-v2", 5'000'000'000, 2); // an odd whole second: 1 s granular
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 6'000'000'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey()
          == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Header } });
}

TEST_CASE("Every verdict decides the STORE and the manifest exactly", "[input-witness][1650]")
{
    CHECK(RowOf(InputStability::Stable).store == StoreObject::Yes);
    CHECK(RowOf(InputStability::Stable).manifest == KeepManifest::Yes);
    CHECK(RowOf(InputStability::MovedDuringCompile).store == StoreObject::No);
    CHECK(RowOf(InputStability::MovedDuringCompile).manifest == KeepManifest::Yes);
    CHECK(RowOf(InputStability::MovedBeforeKey).store == StoreObject::No);
    CHECK(RowOf(InputStability::MovedBeforeKey).manifest == KeepManifest::No);
    CHECK(RowOf(InputStability::Unwitnessed).store == StoreObject::No);
    CHECK(RowOf(InputStability::Unwitnessed).manifest == KeepManifest::No);
    CHECK(DescribeInstability({ .stability = InputStability::MovedDuringCompile, .path = "/r/a.cpp" })
          == "an input changed during the compile: /r/a.cpp");
    CHECK(DescribeInstability({ .stability = InputStability::Stable, .path = "" }).empty());
}

TEST_CASE("The witnessed content answers keyed hashes, and guards a late path by the clock", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.Set(std::string { Header }, "h-v2", 2'500, 2); // saved after the snapshot
    files.Set("/r/src/old.hpp", "old", 10, 8);           // late, untouched
    files.Set("/r/src/new.hpp", "new", 1'500, 9);        // late, changed after T0
    WitnessedManifestContent const content { witness, files };
    CHECK(content.HashAsKeyed(Header) == std::expected<std::string, ManifestFault> { "h-v1" });
    CHECK(content.HashAsKeyed("/r/src/old.hpp") == std::expected<std::string, ManifestFault> { "old" });
    CHECK(content.HashAsKeyed("/r/src/new.hpp")
          == std::expected<std::string, ManifestFault> { std::unexpected(ManifestFault::ChangedDuringInvocation) });
    CHECK(content.HashAsKeyed("/r/src/none.hpp")
          == std::expected<std::string, ManifestFault> { std::unexpected(ManifestFault::Unreadable) });
}

TEST_CASE("Two checkouts with equal content record byte-identical manifests and keys", "[input-witness][1650]")
{
    // Spec invariant 1, at the seam: identities differ in every field between the two
    // checkouts (and between two machines), and nothing of them may reach the record.
    FastCache::Testing::ScratchDirectory const scratchA { "fc-witness-share-a" };
    FastCache::Testing::ScratchDirectory const scratchB { "fc-witness-share-b-deeper" };
    std::string encoded[2];
    std::string manifestKeys[2];
    for (auto const index: { 0, 1 })
    {
        auto const& scratch = index == 0 ? scratchA : scratchB;
        scratch.Write("src/h.hpp", "#pragma once\nint h();\n");
        scratch.Write("src/a.cpp", "#include \"h.hpp\"\nint a() { return h(); }\n");
        auto const& root = scratch.Path();
        FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
        auto const source = (root / "src" / "a.cpp").string();
        auto const header = (root / "src" / "h.hpp").string();
        auto const resolvedSource = ResolveAgainst(source, root.string(), layout);
        auto const resolvedHeader = ResolveAgainst(header, root.string(), layout);

        FakeInputFiles files;
        auto const disk = MakeDiskInputFiles();
        files.identities[resolvedSource] = FileIdentity { .changeTime = 10 + index, .size = 1, .volumeId = 100U + index, .fileIdHigh = 0, .fileIdLow = 1U + index };
        files.identities[resolvedHeader] = FileIdentity { .changeTime = 20 + index, .size = 2, .volumeId = 100U + index, .fileIdHigh = 0, .fileIdLow = 3U + index };
        files.hashes[resolvedSource] = disk->Hash(resolvedSource);
        files.hashes[resolvedHeader] = disk->Hash(resolvedHeader);

        auto witness = InputWitness::Begin(resolvedSource, files, ContentHashing::Yes);
        files.now = 2'000;
        witness.Snapshot(ProjectDependencyPaths(std::vector<std::string> { header }, root.string(), layout), files);
        REQUIRE(witness.AtKey().stability == InputStability::Stable);
        WitnessedManifestContent const content { witness, files };
        auto const manifest = BuildManifest({ .sourcePath = source,
                                              .reportedDependencies = ReportedDependencies::Observed({ header }),
                                              .workingDirectory = root.string(),
                                              .toolchainStamp = "stamp",
                                              .objectKey = "objkey",
                                              .content = content },
                                            layout);
        REQUIRE(manifest.has_value());
        encoded[index] = EncodeManifest(*manifest);
        auto const token = CanonicalSourceToken(source, layout, root.string());
        REQUIRE(token.has_value());
        manifestKeys[index] = ComputeManifestKey(*token, { "-c", *token }, "stamp");
    }
    CHECK(encoded[0] == encoded[1]);
    CHECK(manifestKeys[0] == manifestKeys[1]);
}
```

If clang-tidy rejects the C array `std::string encoded[2]`, use `std::array<std::string, 2>`. If it rejects `index` in the ternary, select the scratch with `std::array<FastCache::Testing::ScratchDirectory const*, 2>`. Keep the assertions.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset cl-debug --target fastcache-cc-tests`.
Expected: a compile error, because `InputWitness.hpp` does not exist.

- [ ] **Step 3: Write `InputWitness.hpp`**

Declare everything listed under **Produces** above, with a Doxygen comment on each. The enums are private: there is no `= N`, and the comment says "Private: never transmitted or persisted". The table is `inline constexpr EnumTable<InputStability, StabilityRow> StabilityTable { { ... } };` with the four rows from the table above, followed by:

```cpp
static_assert(RowsInEnumeratorOrder(StabilityTable, &StabilityRow::stability),
              "StabilityTable must hold one row per InputStability, in enumerator order");
```

Take `EnumTable` and `RowsInEnumeratorOrder` from `<FastCache/Core/EnumTable.hpp>`, as `DirectManifest.hpp` does. `StabilityFinding` has a defaulted `operator==`. `InputWitness`'s private members are:

```cpp
    struct Entry
    {
        std::string path;
        std::optional<FileIdentity> identity;
        std::string hash;
    };
    FileInstant _started {};
    std::string _source;
    std::optional<FileIdentity> _sourceAtStart;
    std::string _sourceHashAtStart;
    ContentHashing _hashing { ContentHashing::Yes };
    std::vector<Entry> _entries;
    StabilityFinding _atKey { .stability = InputStability::Stable, .path = {} };
```

The class comment states invariant 1: never encoded, never transmitted, compared only within one process. It also says why there are THREE moments, citing the spec.

- [ ] **Step 4: Write `InputWitness.cpp`**

Implement exactly the rules listed above this task's Step 1. `Snapshot` loops `for (auto const& path: paths)` over a local vector built as `{ _source }` plus every project path `!= _source`. It records the first finding only:

```cpp
    auto const note = [this](InputStability stability, std::string const& path) {
        if (_atKey.stability == InputStability::Stable)
            _atKey = StabilityFinding { .stability = stability, .path = path };
    };
```

`RowOf` returns `StabilityTable[stability]`. Use whatever accessor `EnumTable` provides, as `DescribeManifestFailure` does in `DirectManifest.cpp`.

- [ ] **Step 5: Register in CMake, build, run the tests**

Run: `cmake --build --preset cl-debug --target fastcache-cc-tests`, then `out/build/cl-debug/src/apps/fastcache-cc/fastcache-cc-tests.exe "[input-witness]"`.
Expected: `test cases: 15 | 15 passed`.

- [ ] **Step 6: Commit**

```bash
git add src/apps/fastcache-cc/InputWitness.hpp src/apps/fastcache-cc/InputWitness.cpp src/apps/fastcache-cc/InputWitness_test.cpp src/apps/fastcache-cc/CMakeLists.txt
git commit -s -m "cc: an input witness decides what an edit during a compile leaves sound (#1650)"
```

---

### Task 4: The `INPUTS-MOVED` outcome in the statistics

**Files:**
- Modify: `src/apps/fastcache-cc/Stats.hpp:26-41`, which is `enum class Outcome`.
- Modify: `src/apps/fastcache-cc/Stats.cpp`:
  - `ParseOutcome` (`:589-600`) and `ToStringView` (`:1073-1089`);
  - the tally struct (`:455-515`): a field, `Total()` and the lookup total;
  - the text report (`:1020-1040`) and the tally switch (`:1300-1315`);
  - the source-listing condition (`:1337`), the miss-latency condition (`:1538`) and the HTML cards (`:1830-1840`).
- Modify: `src/apps/fastcache-cc/Stats_test.cpp`.
- Modify: `docs/tools/fastcache-cc.md`: the `## Statistics` section near line 672, and the sample `--show-stats` output.

**Interfaces:**
- Consumes: nothing.
- Produces: `Cc::Outcome::InputsMoved` and the token `"INPUTS-MOVED"`.

- [ ] **Step 1: Write the failing tests**

Read `Stats_test.cpp` for how an existing case round-trips `VerifyMismatch` through a log line and the report, with `grep -n "VERIFY-MISMATCH\|VerifyMismatch" src/apps/fastcache-cc/Stats_test.cpp`. Add the same two cases for `InputsMoved`:
- `ToStringView(Outcome::InputsMoved) == "INPUTS-MOVED"`, and a log line with that token reads back as `Outcome::InputsMoved`, not `Unavailable`;
- a report over one `HIT` and one `INPUTS-MOVED` record says `inputs moved : 1`, counts 2 compiles, and lists the moved record's source among the named files.

Tag both `[stats][1650]`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `out/build/cl-debug/src/apps/fastcache-cc/fastcache-cc-tests.exe "[stats]"` after building.
Expected: a compile error, because `Outcome::InputsMoved` does not exist.

- [ ] **Step 3: Implement**

Append to `Outcome`, after `VerifyMismatch`:

```cpp
    /// Compiled, and an input changed while it was compiling, so nothing was stored (#1650).
    ///
    /// Its own outcome because both neighbours would be false: `Miss` claims compiled AND
    /// stored, and `Uncacheable` claims a permanent property of the translation unit, where
    /// this is one invocation's race with an editor. The detail names the path and the moment.
    InputsMoved,
```

Every `switch` over `Outcome` without a `default` now fails to compile. Fix each one:
- `ToStringView`: `case Outcome::InputsMoved: return "INPUTS-MOVED";`
- `ParseOutcome`: `if (token == "INPUTS-MOVED") return Outcome::InputsMoved;`
- tally switch: `case Outcome::InputsMoved: ++tally->inputsMoved; break;`
- tally struct: `/// Compiles an input changed during; see Outcome::InputsMoved.` and `std::uint64_t inputsMoved {};`. Add it to the all-compiles total (`:503`) and to the lookups total (`:513`), since it was a lookup that did not hit.
- text report, after the `uncacheable` line, in the same shape: `if (tally.inputsMoved > 0) out << "  inputs moved : " << Colorize(std::to_string(tally.inputsMoved), palette.neutral, palette.reset) ...`
- source listing (`:1337`): add `|| record.outcome == Outcome::InputsMoved`, so the file is named.
- miss latency (`:1538`): add `|| record.outcome == Outcome::InputsMoved`, since it was a real compile.
- HTML cards (`:1833`): `AppendTallyCard(out, "inputs moved", overall.inputsMoved, "uncache");`

In `docs/tools/fastcache-cc.md` `## Statistics`, add one sentence after the paragraph naming the columns:

> `INPUTS-MOVED` is a compile during which one of its inputs changed — an editor's save, a `git checkout` in another terminal — so the launcher kept nothing from it; the `detail` column names the file and whether it moved while the key was taken or during the compile, and the next build caches as usual.

In the sample output, add no line; the sample shows a run with none.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `out/build/cl-debug/src/apps/fastcache-cc/fastcache-cc-tests.exe "[stats]"`, then the whole binary.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/apps/fastcache-cc/Stats.hpp src/apps/fastcache-cc/Stats.cpp src/apps/fastcache-cc/Stats_test.cpp docs/tools/fastcache-cc.md
git commit -s -m "cc: a compile an edit raced is recorded INPUTS-MOVED, never a miss (#1650)"
```

---

### Task 5: Wire the witness into `RunCached`

**Files:**
- Modify: `src/apps/fastcache-cc/main.cpp`:
  - `RecordManifest` (`:2024-2195`);
  - `RunCached`: before `:2845` (`preprocessStarted`), the probe block (`:2858-2972`), the backfill (`:3113-3130`), and after the `!CacheIsServing` return (`:3225-3234`) up to the final `RecordManifest` (`:3393-3394`);
  - remove Task 2's temporary `HashedWhenAsked` and its include.

**Interfaces:**
- Consumes: Task 1's `MakeDiskInputFiles`, Task 2's `ProjectDependencyPaths`, Task 3's `InputWitness`, `ContentHashing`, `RowOf`, `StoreObject`, `KeepManifest`, `DescribeInstability` and `WitnessedManifestContent`, and Task 4's `Outcome::InputsMoved`.
- Produces: no new symbols. `main.cpp` is in no test target, so Task 6's e2e is this task's test.

- [ ] **Step 1: Extract the dependency list `RecordManifest` builds**

Move the block in `RecordManifest` that computes `includes` (the probe's set, else the build's own depfile when the probe returned nothing and was not `unreadable`) into a file-local function. It must run identically before the STORE and in `RecordManifest`:

```cpp
/// The dependency paths a manifest is built from: the probe's, or -- when the probe came back
/// with nothing and the reason was not unreadable notes -- the build's own depfile, which the
/// compile has just written. Reconciled either way.
/// @return The paths, as the compiler spelled them.
[[nodiscard]] std::vector<std::string> ManifestDependencies(ProbedDependencies const& probed,
                                                            Cc::ParsedCommand const& cmd,
                                                            Cc::RootReconciler& reconciler)
{
    auto includes = probed.paths;
    if (includes.empty() && !probed.unreadable)
        if (auto const depText = ReadDepFile(cmd))
        {
            includes = Cc::ParseDepFilePaths(*depText);
            reconciler.DependencyList(includes);
        }
    return includes;
}
```

Keep the existing comments with the code they describe, in the function. `RecordManifest` calls `auto includes = ManifestDependencies(probed, cmd, reconciler);`.

- [ ] **Step 2: Give `RecordManifest` its content source**

Add a parameter `Cc::IManifestContent const& content` after `reconciler`, and pass `.content = content` to `Cc::BuildManifest`. Delete the temporary `HashedWhenAsked` object and the `ManifestContentTestSupport.hpp` include.

- [ ] **Step 3: T0, just before the key's preprocess**

Immediately before `auto const preprocessStarted = std::chrono::steady_clock::now();`, insert:

```cpp
    // T0 (#1650). The key, the object and the manifest are each taken from the filesystem at a
    // different moment, and a save between them filed one content's object or manifest under
    // another content's key -- served as a HIT, exit 0, to every checkout and machine whose
    // files match the saved version. The witness snapshots every input here, after the key's
    // preprocess and after the compile, and `Cc::StabilityTable` decides what of this invocation
    // is still sound. Hashing only when a manifest may be recorded: with direct mode off the
    // STORE is guarded by identity alone.
    auto const inputFiles = Cc::MakeDiskInputFiles();
    auto const workingDirectoryText = workingDirectory.string();
    auto witness = Cc::InputWitness::Begin(Cc::ResolveAgainst(reconciler.Directory(cmd.source), workingDirectoryText, layout),
                                           *inputFiles,
                                           cfg.direct ? Cc::ContentHashing::Yes : Cc::ContentHashing::No);
    Cc::WitnessedManifestContent const keyedContent { witness, *inputFiles };
```

If `reconciler.Directory` returns a type other than `std::string`, match `RecordManifest`'s `resolvedSource` exactly. The point is that this path equals the one `BuildManifest` resolves.

- [ ] **Step 4: T1′, right after the probe**

Inside the probe block, directly after `probed = { .paths = ..., .unreadable = ... };`, insert:

```cpp
        // T1': what the key was just taken from. These hashes are what a manifest records.
        witness.Snapshot(Cc::ProjectDependencyPaths(probed.paths, workingDirectoryText, layout), *inputFiles);
```

- [ ] **Step 5: The backfill on a preprocessed HIT**

Replace the backfill's `if (cfg.direct) RecordManifest(...);` with:

```cpp
                    if (cfg.direct)
                    {
                        // The hit is served whatever moved: its object was compiled from exactly
                        // the text this key hashes. Only a manifest built from hashes that may not
                        // describe that text is refused (#1650).
                        auto const atKey = witness.AtKey();
                        if (Cc::RowOf(atKey.stability).manifest == Cc::KeepManifest::Yes)
                            RecordManifest(record, cfg, cmd, layout, workingDirectory, relativizedArgs,
                                           toolchainStamp, probed, key, reconciler, keyedContent);
                        else
                            Note(record.verbose,
                                 std::format("{}; served the hit, recorded no manifest", Cc::DescribeInstability(atKey)));
                    }
```

- [ ] **Step 6: T2, before the STORE**

Directly after the `if (!Cc::CacheIsServing(fetchKind)) { ... return code; }` block that follows the compile, insert:

```cpp
    // T2 (#1650). The object belongs to the key only if nothing the key was taken from moved
    // while the compile -- local or dispatched, whose second preprocess is inside this window --
    // was reading it.
    auto const moved = witness.AfterCompile(
        Cc::ProjectDependencyPaths(ManifestDependencies(probed, cmd, reconciler), workingDirectoryText, layout),
        *inputFiles);
    if (auto const& row = Cc::RowOf(moved.stability); row.store == Cc::StoreObject::No)
    {
        auto const keepsManifest = cfg.direct && row.manifest == Cc::KeepManifest::Yes;
        record.outcome = Cc::Outcome::InputsMoved;
        record.outcomeDetail = Cc::DescribeInstability(moved);
        Note(record.verbose,
             std::format("{}; compiled, not stored{}",
                         Cc::DescribeInstability(moved),
                         keepsManifest ? "; manifest recorded from the keyed content" : ""));
        if (keepsManifest)
            RecordManifest(record, cfg, cmd, layout, workingDirectory, relativizedArgs, toolchainStamp,
                           probed, key, reconciler, keyedContent);
        return code;
    }
```

Pass `keyedContent` to the final `RecordManifest` call at the end of `RunCached` as well.

- [ ] **Step 7: Build and run the whole unit suite and the existing e2e**

Run: `cmake --build --preset cl-debug`, then `ctest --preset cl-debug -R "compile-cache-e2e" --output-on-failure` (Windows) and the `fastcache-cc-tests` binary.
Expected: every case PASSES. The existing e2e legs never edit mid-compile, so they show no `INPUTS-MOVED`. A spurious one here is a defect in this task, not a flake.

- [ ] **Step 8: Commit**

```bash
git add src/apps/fastcache-cc/main.cpp
git commit -s -m "cc: the witness guards the STORE, the manifest and the backfill (#1650)"
```

---

### Task 6: The mid-compile edit stub and the launcher end-to-end legs

**Files:**
- Create: `src/tests/MidCompileEditStub.cpp`
- Modify: `src/tests/CMakeLists.txt`:
  - define the target before the `compile-cache-e2e` block (`:4976`);
  - pass `--edit-stub` / `-EditStub` in both branches when `TARGET mid-compile-edit-stub`.
- Modify: `scripts/compile-cache-e2e.sh`:
  - accept `--edit-stub <path>`, in the usage text and the parse loop;
  - a new case `11` after case 10, listed in the header comment.
- Modify: `src/apps/fastcache-cc/run-launcher-e2e.ps1`:
  - a `-EditStub` parameter;
  - a new per-compiler section after `=== a stale manifest is refused ($cc) ===`.

**Interfaces:**
- Consumes: Task 5's behaviour. The trace lines `fastcache-cc: an input changed during the compile: <path>; compiled, not stored; manifest recorded from the keyed content`, `fastcache-cc: MISS key=`, `fastcache-cc: HIT key=`, `STORED key=` and `MANIFEST stored key=`, and the statistics token `INPUTS-MOVED`.
- Produces: the `mid-compile-edit-stub` executable, driven by environment variables:

<!-- table-total: none -->
| variable | meaning |
|---|---|
| `FC_EDIT_STUB_REAL` | absolute path of the real compiler to forward to |
| `FC_EDIT_STUB_AT` | `after-compile`: copy after a real compile exits 0; `before-dispatch-preprocess`: copy before forwarding a line-marker preprocess (`-E` without `-P`, or `/E`) |
| `FC_EDIT_STUB_TRIGGER` | a file; the copy happens only while it exists, and the stub deletes it (one shot) |
| `FC_EDIT_STUB_FROM`, `FC_EDIT_STUB_TO` | the copy: FROM's bytes overwrite TO in place |

The stub classifies its own argv, from argv[1] on:
- **A real compile:** `-c` or `/c` present, and none of `-E`, `/E`, `-EP`, `/EP`, `-P`, `/P` or `-###`.
- **A dispatch preprocess:** `-E` or `/E` present, and none of `-P`, `/P`, `-EP`, `/EP`, `-v` or `-###`.
- Anything else is forwarded untouched. That includes the key's preprocess (`-E -P`, `/EP`), the banner probe, `-###` and the include probe `-E -v`.

- [ ] **Step 1: Write the stub**

`src/tests/MidCompileEditStub.cpp`. Forward through `CreateProcessW` with the verbatim command-line tail on Windows (the issue's `wrapcl.cpp` shows the tail extraction), and through `posix_spawn` plus `waitpid` on POSIX. Exit with the child's code. Classify by walking `std::span { argv, static_cast<std::size_t>(argc) }.subspan(1)` with range-for. Perform the copy with `std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing)`, then remove the trigger.

Doxygen-comment the file's purpose: #1650's reproduction, the editor's save landing where the launcher cannot see it. It needs no FastCache library and no Catch2.

- [ ] **Step 2: Register it**

In `src/tests/CMakeLists.txt`, before the `compile-cache-e2e` block:

```cmake
# The editor's save, landing inside one launcher invocation (#1650): a stub compiler that
# forwards to the real one and, while a trigger file exists, overwrites a source either after
# the compile or before a dispatch preprocess. Built for the e2e fixtures below and never
# installed. It is copied there under the real compiler's own file name, because the launcher
# picks its driver grammar by that name.
if(TARGET fastcache-cc)
    add_executable(mid-compile-edit-stub MidCompileEditStub.cpp)
endif()
```

In both `compile-cache-e2e` branches, add `--edit-stub "$<TARGET_FILE:mid-compile-edit-stub>"` (POSIX) or `-EditStub "$<TARGET_FILE:mid-compile-edit-stub>"` (Windows). Guard each with `if(TARGET mid-compile-edit-stub)` into a list variable, the way `_fc_e2e_testclient` is built.

- [ ] **Step 3: Write the POSIX legs**

In `scripts/compile-cache-e2e.sh`:
- parse `--edit-stub` into `edit_stub=""`;
- add case `11` to the header list: "A save DURING a compile records nothing unsound (#1650)";
- after case 10, add the block below. If `edit_stub` is empty, `fail` rather than skip: the registration always passes it, so its absence is a broken registration.

```bash
# --- 11: a save DURING a compile records nothing unsound (#1650) -------------
# The launcher used to hash its manifest AFTER the compile, so a save landing during it
# paired the new content with the old object: every later build of the new content was a
# direct HIT on the old object, exit 0, in every checkout whose files matched.
[[ -n "$edit_stub" ]] || fail "case 11 needs --edit-stub; the ctest registration passes it whenever the stub is built"
real_compiler="$(command -v "$compiler")" || fail "case 11 cannot resolve the compiler '${compiler}'"
moved="${workdir}/moved"
mkdir -p "${moved}/stub"
stub="${moved}/stub/$(basename "$real_compiler")"
cp "$edit_stub" "$stub"
chmod +x "$stub"

# One tree per leg so no leg reads another's cache entries.
write_moved_tree() {
    local root="$1" message="$2" header_message="$3"
    mkdir -p "${root}/src" "${root}/build"
    printf '#pragma once\n#define HEADER_MESSAGE "%s"\n' "$header_message" > "${root}/src/m.hpp"
    printf '#include "m.hpp"\nchar const* Message() { return "%s"; }\nchar const* HeaderMessage() { return HEADER_MESSAGE; }\n' \
        "$message" > "${root}/src/m.cpp"
}
moved_compile() {
    local root="$1" log="$2"
    shift 2
    env "$@" FC_EDIT_STUB_REAL="$real_compiler" FASTCACHE_SOURCE_DIR="$root" FASTCACHE_BINARY_DIR="${root}/build" \
        "$launcher" "$stub" -std=c++20 -MD -MF "${root}/build/m.d" -c "${root}/src/m.cpp" -o "${root}/build/m.o" 2> "$log"
}
object_says() { LC_ALL=C grep -aq "$2" "$1"; }

for leg in tu header; do
    root="${moved}/${leg}"
    write_moved_tree "$root" MESSAGE_V1_OLD HEADER_V1_OLD
    if [[ "$leg" == tu ]]; then
        write_moved_tree "${moved}/${leg}-v2" MESSAGE_V2_NEW HEADER_V1_OLD
        from="${moved}/${leg}-v2/src/m.cpp" to="${root}/src/m.cpp" want=MESSAGE_V2_NEW
    else
        write_moved_tree "${moved}/${leg}-v2" MESSAGE_V1_OLD HEADER_V2_NEW
        from="${moved}/${leg}-v2/src/m.hpp" to="${root}/src/m.hpp" want=HEADER_V2_NEW
    fi
    : > "${moved}/${leg}.trigger"
    echo "== mid-compile save (${leg}): the compile that raced it stores nothing =="
    moved_compile "$root" "${moved}/${leg}-1.log" FC_EDIT_STUB_AT=after-compile \
        FC_EDIT_STUB_TRIGGER="${moved}/${leg}.trigger" FC_EDIT_STUB_FROM="$from" FC_EDIT_STUB_TO="$to" \
        || { cat "${moved}/${leg}-1.log" >&2; fail "mid-compile ${leg}: the racing compile failed"; }
    cat "${moved}/${leg}-1.log"
    [[ ! -e "${moved}/${leg}.trigger" ]] || fail "mid-compile ${leg}: the stub never fired, so this leg tested nothing"
    grep -q "an input changed during the compile: " "${moved}/${leg}-1.log" \
        || fail "mid-compile ${leg}: the save during the compile was not seen"
    grep -q "STORED key=" "${moved}/${leg}-1.log" && fail "mid-compile ${leg}: the raced object was stored"
    logged="$(tail -n 1 "$(e2e_launcher_state_log)" | e2e_launcher_log_field outcome)"
    [[ "$logged" == "INPUTS-MOVED" ]] || fail "mid-compile ${leg}: recorded '${logged}', not INPUTS-MOVED"

    echo "== mid-compile save (${leg}): the saved content is a MISS and builds what was saved =="
    rm -f "${root}/build/m.o"
    moved_compile "$root" "${moved}/${leg}-2.log" FASTCACHE_E2E_CASE=moved \
        || { cat "${moved}/${leg}-2.log" >&2; fail "mid-compile ${leg}: the rebuild failed"; }
    cat "${moved}/${leg}-2.log"
    grep -q "fastcache-cc: MISS" "${moved}/${leg}-2.log" \
        || fail "mid-compile ${leg}: the saved content was served from the cache (the #1650 defect)"
    object_says "${root}/build/m.o" "$want" || fail "mid-compile ${leg}: the object does not say ${want}"
done

echo "== mid-compile save: another checkout holding the saved content is not served the raced object =="
other="${moved}/other"
write_moved_tree "$other" MESSAGE_V2_NEW HEADER_V1_OLD
moved_compile "$other" "${moved}/other.log" FASTCACHE_E2E_CASE=moved \
    || { cat "${moved}/other.log" >&2; fail "mid-compile: the other checkout failed"; }
object_says "${other}/build/m.o" MESSAGE_V2_NEW || fail "mid-compile: another checkout was served the raced object"

echo "== control: the same stub with no save stores, records a manifest, then direct-hits =="
control="${moved}/control"
write_moved_tree "$control" MESSAGE_CONTROL HEADER_CONTROL
moved_compile "$control" "${moved}/control-1.log" FASTCACHE_E2E_CASE=moved \
    || { cat "${moved}/control-1.log" >&2; fail "control: the compile failed"; }
grep -q "STORED key=" "${moved}/control-1.log" || fail "control: a quiet compile through the stub did not store"
grep -q "MANIFEST stored key=" "${moved}/control-1.log" || fail "control: a quiet compile did not record a manifest"
rm -f "${control}/build/m.o"
moved_compile "$control" "${moved}/control-2.log" FASTCACHE_E2E_CASE=moved \
    || { cat "${moved}/control-2.log" >&2; fail "control: the rebuild failed"; }
grep -q "direct mode: the manifest validated" "${moved}/control-2.log" || fail "control: the rebuild was not a direct hit"

echo "== control: a second checkout of the same content direct-hits the first one's manifest =="
twin="${moved}/control-twin-deeper/tree"
write_moved_tree "$twin" MESSAGE_CONTROL HEADER_CONTROL
moved_compile "$twin" "${moved}/twin.log" FASTCACHE_E2E_CASE=moved \
    || { cat "${moved}/twin.log" >&2; fail "control: the twin checkout failed"; }
grep -q "direct mode: the manifest validated" "${moved}/twin.log" \
    || fail "control: cross-checkout sharing did not survive the witness"
echo "   a save during a compile stores nothing unsound, and sharing is unchanged"
```

Run `bash scripts/check-e2e-helpers.sh` afterwards. The rule against `producer | grep -q` applies, and the pipe into `e2e_launcher_log_field` above copies the existing line `:1573`.

- [ ] **Step 4: Write the Windows legs**

In `run-launcher-e2e.ps1`:
- add `[string]$EditStub` to `param(...)`;
- add the same five legs inside the per-compiler loop, under `Write-Host "=== a save during a compile records nothing unsound ($cc) ==="`;
- copy the stub to `<MoveTemp>\stub-$cc\$cc.exe`;
- set `$env:FC_EDIT_STUB_REAL = (Get-Command $cc).Source`;
- invoke through a variant of `Invoke-Launcher` that takes the compiler path and the extra environment, following `Invoke-LauncherWithEnv`'s set-then-remove pattern;
- read the logged outcome with `Get-E2ELauncherLogField` on the last line of `$launcherState.Log`;
- check object contents with `Select-String -Path $obj -Pattern MESSAGE_V2_NEW -SimpleMatch -Quiet`.

For `cl` the stub fires after `cl` exits, which is correct: `cl` holds the source open while compiling (#1650 measured it), so the save cannot land inside the compiler run.

A missing `$EditStub` is a failure, never a skip, for the reason given in Step 3.

- [ ] **Step 5: Run the e2e on both platforms**

Run on Windows: `ctest --preset cl-debug -R "^compile-cache-e2e$" --output-on-failure`.
Run on Linux or WSL: configure a `clang-debug` or `gcc-debug` tree, then `ctest -R "^compile-cache-e2e$" --output-on-failure`. Keep the build tree on the Linux filesystem (`build-and-toolchain.md`).
Expected: PASS, with every new `==` line printed.

- [ ] **Step 6: Commit**

```bash
git add src/tests/MidCompileEditStub.cpp src/tests/CMakeLists.txt scripts/compile-cache-e2e.sh src/apps/fastcache-cc/run-launcher-e2e.ps1
git commit -s -m "tests: a save during a compile, end to end, with the controls that prove sharing survives (#1650)"
```

---

### Task 7: The dispatched leg

**Files:**
- Modify: `src/tests/CMakeLists.txt`: pass `--edit-stub` / `-EditStub` to both `dist-compile-e2e` registrations, guarded on `TARGET mid-compile-edit-stub`.
- Modify: `scripts/dist-compile-e2e.sh`: a new case after case 2, in the case list and in the `--case suite` set.
- Modify: `scripts/dist-compile-e2e.ps1`: the same case.

**Interfaces:**
- Consumes: Task 6's `mid-compile-edit-stub` and its `before-dispatch-preprocess` mode, and the existing `run_launcher`, `write_source`, `fail_if_withdrawn` and `$fingerprint`.
- Produces: nothing.

- [ ] **Step 1: Assert the stub keeps the dispatch identity**

The worker serves only a matching toolchain fingerprint. Before the case runs:

```bash
stub_fingerprint="$("$launcher" --print-toolchain-fingerprint "$stub")" \
    || fail "the launcher could not fingerprint the edit stub"
[[ "$stub_fingerprint" == "$fingerprint" ]] \
    || fail "the edit stub changes the toolchain fingerprint (${stub_fingerprint} vs ${fingerprint}), so no compile through it can be dispatched and this case would test a local compile"
```

Set `FC_EDIT_STUB_REAL` in the environment of that call too. If the fingerprints differ, STOP and report. Do not loosen the case to accept a local compile: that is the tautology `distributed-compilation.md` warns about.

- [ ] **Step 2: Write the case**

The steps, in order:
1. Write `one-moved.cpp` with `write_source` and token `movedone`. Write a V2 copy whose `// unique:` line and one literal differ.
2. Compile through `run_launcher` with the stub as the compiler and `FC_EDIT_STUB_AT=before-dispatch-preprocess` armed. The save lands after the key's snapshot and before the dispatch preprocess reads the file: #1650's T1→T2 window.
3. Assert `DISPATCHED to `. On failure, run `fail_if_withdrawn` first, as case 1 does.
4. Assert `an input changed during the compile`, no `STORED key=`, and the logged outcome `INPUTS-MOVED`.
5. Restore the V1 content and compile again with no trigger. Assert a `MISS` and an object built from V1.

Before the fix, step 5 was a HIT serving the worker's V2 object under V1's key. Put that sentence in the case's comment: it is what the case discriminates.

Mirror it in `dist-compile-e2e.ps1` with `/E` as the dispatch preprocess.

- [ ] **Step 3: Run it**

Run: `ctest -R "^dist-compile-e2e$" --output-on-failure`, on Linux or WSL and on Windows.
Expected: PASS, with `DISPATCHED to` in the case's log.

- [ ] **Step 4: Commit**

```bash
git add src/tests/CMakeLists.txt scripts/dist-compile-e2e.sh scripts/dist-compile-e2e.ps1
git commit -s -m "tests: a save between the key and a dispatched compile stores nothing under the old key (#1650)"
```

---

### Task 8: The rule, the open work, the upgrade note, and proof the tests can fail

**Files:**
- Modify: `.agent/rules/compile-cache.md`: a new `##` section before `## Accepted trade-offs` (`:2396`), and a new `## Open work` entry.
- Modify: `AGENT.md`: one tripwire bullet in the compile-cache list. `AGENT.md`'s `total-lines` budget is an equality, so the bullet's lines are reclaimed in the same list (below).
- Modify: `docs/operations/upgrading-a-fleet.md`: a subsection after `### Coming from 0.4.0: nodes follow their own address` (`:75`).

- [ ] **Step 1: File the shadowing-header issue**

```bash
gh issue create --title "cc: a header created during a compile that shadows one on the include path is in no input witness" \
  --label type/bug --label area/launcher --label area/compile-cache \
  --body "Found while designing #1650 (.agent/plans/2026-10-11-input-witness-design.md, Known limits). The input witness snapshots every file the key's preprocess REPORTED. A header created during the invocation in an include directory searched EARLIER than the one the preprocess resolved is in no snapshot, so a compile that reads it stores its object under a key taken from the shadowed header. ccache has the same gap. Not reproduced; derived from the witness's construction."
```

Record the issue number it prints.

- [ ] **Step 2: Write the rule section**

In `.agent/rules/compile-cache.md`, before `## Accepted trade-offs`:

```markdown
## The key, the object and the manifest describe ONE moment

<!-- agent-tripwire: The key, the object and the manifest are taken at THREE moments -->

The launcher reads its inputs three times: the key's preprocess (T1), the compile (T2, and a
dispatched compile's second preprocess), and the manifest. Before #1650 the manifest hashed the
files AFTER the compile, so a save during it paired the new content with the old object; every
later build of the new content was a direct HIT on the old object, exit 0, in every checkout and
on every machine whose files matched -- and the manifest key is portable, so it spread.

`Cc::InputWitness` snapshots every input at T0 (before the key), T1' (after it) and T2 (after the
compile), and `Cc::StabilityTable` decides. A manifest's hashes are the T1' hashes, never a read
after the compile; an object is stored only when nothing moved; and the hit a racing backfill
would have spoiled is still served. Three rules make it hold, and each has already been wrong:

- **Change time, never last-write time.** `copy`, `CopyFileW`, robocopy and `utimensat` keep a
  file's mtime; #1650's own reproduction used `CopyFileW`, which an mtime check cannot see.
- **The clock at T0, identity after T1'.** A file whose change time is at or after T0 moved
  while the key was taken, compared through `Cc::ChangedAtOrAfter`, never a raw `>=`: it floors
  T0 to the grid the stamp itself shows (the largest of 2 s down to 10 ns that divides it),
  because exFAT stamps at 10 ms and HFS+ at 1 s, and a change in T0's own slot is stamped BELOW
  T0. After T1' every entry's stamp is below that floor, so any rewrite moves its identity.
- **Nothing the witness holds travels.** No identity reaches a key or a manifest, or two
  checkouts stop sharing; `InputWitness_test`'s two-checkout case pins the bytes.

The clock comparison can fail OPEN in the preprocess window in two places: a stamp from ANOTHER
machine's clock that lags this one (a network share, a WSL `/mnt/<drive>` the Windows host
stamps), and a stamp grid that is not a decimal unit or 2 s. The TU's own T0/T1' comparison still
holds there; a header's does not. Documented, not closed: refusing remote filesystems would remove a feature.
```

Add an `## Open work` entry with the issue from Step 1 as its leading reference, in the shape of the existing entries: the shadowing header, why the witness cannot see it, and that it was derived, not reproduced.

- [ ] **Step 3: The `AGENT.md` tripwire, within budget**

Add after `- Bump \`manifest-v*\` whenever \`objkey-v*\` moves. The reverse is not required.`:

```markdown
- The key, the object and the manifest are taken at THREE moments, so a save between them files
  one content under another's key (#1650): `Cc::InputWitness` decides, by change time, never mtime.
```

`total-lines` is an equality ratchet: two lines added must be two lines reclaimed. Reclaim them by re-wrapping wrapped compile-cache bullets of `AGENT.md` to the ~100-column width the file uses elsewhere, changing no word. Then run `ctest -R "agent-md-budget|rulebook-tripwires" --output-on-failure` (from any configured build tree). Both must pass. If reclaiming two lines would mean changing words, stop and report rather than editing the budget file: its own comment forbids raising the number.

- [ ] **Step 4: The upgrade note**

In `docs/operations/upgrading-a-fleet.md`, after the `Coming from 0.4.0: nodes follow their own address` subsection:

```markdown
### Coming from 0.4.0: direct-mode manifests are recorded once more

The launcher's manifest key moved to `manifest-v8` (#1650). A 0.4.0 launcher hashed a manifest's
files after the compile, so a file saved during one could pair its new content with the old
object; every such manifest is now unreachable. The first build of each translation unit after
the upgrade preprocesses once and records a fresh manifest. Objects still hit under their
unchanged keys, so nothing recompiles. No step.
```

- [ ] **Step 5: Prove the tests can fail (testing rule: neuter the fix)**

Two neutering runs, each on a throwaway commit that is reverted, never pushed:
1. In `InputWitness::AfterCompile`, return `_atKey` immediately. Run `fastcache-cc-tests "[input-witness]"` and `compile-cache-e2e`.
   - Expected red: the unit cases "A TU saved during the compile…", "Same bytes, new identity…", "A header deleted…", "A change in the same tick…" and "A late dependency…", plus the e2e lines `the save during the compile was not seen`.
   - Expected green: every control and every `MovedBeforeKey` case.
2. In `BuildManifest`'s `record` lambda, use `HashFileContents(resolved)` instead of `inputs.content`. Run `fastcache-cc-tests "[1650]"`.
   - Expected red: "BuildManifest takes every hash from its content source…" and the content-refusal case.

Record both runs (which cases went red, and which stayed green) in a scratch note for the pull request body. Then `git revert` or `git reset --hard` the throwaway commits back to the Task 7 tip, and confirm with `git log --oneline -3`.

- [ ] **Step 6: The local gate**

Run: `bash scripts/local-gate.sh`, bare, with `bash <path>`. It must print its terminal PASS line. A run printing neither terminal line did not conclude: re-read it with `--classify=<log>`.

- [ ] **Step 7: Commit**

```bash
git add .agent/rules/compile-cache.md AGENT.md docs/operations/upgrading-a-fleet.md
git commit -s -m "docs: the key, the object and the manifest describe one moment (#1650)"
```
