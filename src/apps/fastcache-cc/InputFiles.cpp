// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "InputFiles.hpp"

#include <FastCache/Platform/NarrowText.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <optional>
#include <thread>

namespace FastCache::Cc
{

namespace
{
    /// How long `Fence` waits for the coarse clock to move before giving up on it. Ten ticks at
    /// 100 Hz: a clock that has not moved in that long is not ticking the way the fence assumes.
    constexpr auto FenceBound = std::chrono::milliseconds { 100 };

    /// How long `Fence` sleeps between two reads of the coarse clock: short against any tick in
    /// common use (1 ms to 15.6 ms), so the fence overshoots the tick by little.
    constexpr auto FencePoll = std::chrono::microseconds { 500 };

#if !defined(_WIN32)

    /// @param value An instant on the realtime clock.
    /// @return The same instant in `FileInstant` units.
    [[nodiscard]] FileInstant FromTimespec(timespec const& value) noexcept
    {
        return (static_cast<FileInstant>(value.tv_sec) * OneSecondOfFileTime) + value.tv_nsec;
    }
#endif

#if defined(_WIN32)
    /// 1970-01-01 in FILETIME ticks (100 ns since 1601-01-01).
    constexpr std::int64_t UnixEpochAsFileTime = 116'444'736'000'000'000;

    [[nodiscard]] FileInstant FromFileTime(std::int64_t ticks) noexcept
    {
        return (ticks - UnixEpochAsFileTime) * 100;
    }

    /// @param value A clock reading in the `FILETIME` layout.
    /// @return The same instant in `FileInstant` units.
    [[nodiscard]] FileInstant FromFileTime(FILETIME const& value) noexcept
    {
        return FromFileTime(
            static_cast<std::int64_t>((std::uint64_t { value.dwHighDateTime } << 32U) | value.dwLowDateTime));
    }

    /// Closes a handle on every path out.
    class HandleCloser
    {
      public:
        explicit HandleCloser(HANDLE handle) noexcept:
            _handle { handle }
        {
        }
        HandleCloser(HandleCloser const&) = delete;
        HandleCloser& operator=(HandleCloser const&) = delete;
        HandleCloser(HandleCloser&&) = delete;
        HandleCloser& operator=(HandleCloser&&) = delete;
        ~HandleCloser()
        {
            CloseHandle(_handle);
        }

      private:
        HANDLE _handle;
    };
#endif

    class SystemRealtimeClocks final: public IRealtimeClocks
    {
      public:
        [[nodiscard]] std::optional<FileInstant> Coarse() const override
        {
#if defined(_WIN32)
            FILETIME now {};
            GetSystemTimeAsFileTime(&now);
            return FromFileTime(now);
#else
    #if defined(CLOCK_REALTIME_COARSE)
            timespec now {};
            if (::clock_gettime(CLOCK_REALTIME_COARSE, &now) == 0)
                return FromTimespec(now);
    #endif
            return std::nullopt;
#endif
        }

        [[nodiscard]] std::optional<FileInstant> Fine() const override
        {
#if defined(_WIN32)
            FILETIME now {};
            GetSystemTimePreciseAsFileTime(&now);
            return FromFileTime(now);
#else
            timespec now {};
            if (::clock_gettime(CLOCK_REALTIME, &now) == 0)
                return FromTimespec(now);
            return std::nullopt;
#endif
        }

        void Pause(std::chrono::nanoseconds duration) const override
        {
            std::this_thread::sleep_for(duration);
        }

        [[nodiscard]] std::chrono::nanoseconds Monotonic() const override
        {
            return std::chrono::steady_clock::now().time_since_epoch();
        }
    };

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
                std::memcpy(
                    &identity.fileIdHigh, id.FileId.Identifier + sizeof identity.fileIdLow, sizeof identity.fileIdHigh);
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
            return FileIdentity { .changeTime = FromTimespec(changed),
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
            return NowOn(*_clocks);
        }

        [[nodiscard]] FileInstant Fence() const override
        {
            return FenceOn(*_clocks);
        }

      private:
        std::unique_ptr<IRealtimeClocks> _clocks = MakeSystemRealtimeClocks();
    };
} // namespace

FileInstant NowOn(IRealtimeClocks const& clocks)
{
    if (auto const coarse = clocks.Coarse())
        return *coarse;
    if (auto const fine = clocks.Fine())
        return *fine - FineClockSafetyTick;
    // No clock answered. Zero precedes every change time, so every input reads as changed: the
    // invocation fails closed rather than trusting a witness it cannot time.
    return 0;
}

FileInstant FenceOn(IRealtimeClocks const& clocks)
{
    // The FINE clock is read at the call because it, not the coarse one, bounds every stamp a
    // change made before this call can carry -- coarse, fine, or a multigrain stamp: Linux 6.13+
    // stamps a file stat'ed since its last change with a FINE time, and the coarse clock can still
    // read BELOW that stamp for up to a tick (#1650). Waiting only for the coarse clock to change
    // returned such an instant, below a change made before it.
    auto const fineAtCall = clocks.Fine();
    if (auto const coarseAtCall = clocks.Coarse())
    {
        // Every earlier stamp is at or below `floor`. The first coarse reading strictly above it
        // is therefore an instant no earlier change carries, while every later change is stamped
        // at or above the coarse clock, so at or above that reading.
        auto const floor = std::max(*coarseAtCall, fineAtCall.value_or(*coarseAtCall));
        auto last = *coarseAtCall;
        auto const deadline = clocks.Monotonic() + FenceBound;
        while (clocks.Monotonic() < deadline)
        {
            clocks.Pause(FencePoll);
            auto const reading = clocks.Coarse();
            if (!reading)
                break;
            if (*reading > floor)
                return *reading;
            last = *reading;
        }
        // The clock did not pass the floor within the bound, or stopped answering. Fail CLOSED: an
        // instant a safety tick below the last reading can only make more changes read as
        // at-or-after it, which costs a store and never serves a wrong object.
        return last - FineClockSafetyTick;
    }
    if (fineAtCall)
    {
        // No coarse clock: a fine read can be up to one tick AHEAD of the stamp a change made just
        // after it receives. So wait that tick out and move the reading back by it: every change
        // before this call is then at or below the result, and every change after it at or above.
        clocks.Pause(std::chrono::nanoseconds { FineClockSafetyTick });
        if (auto const fine = clocks.Fine())
            return *fine - FineClockSafetyTick;
    }
    // No clock answered; `NowOn`'s answer, for `NowOn`'s reason.
    return 0;
}

std::unique_ptr<IRealtimeClocks> MakeSystemRealtimeClocks()
{
    return std::make_unique<SystemRealtimeClocks>();
}

std::unique_ptr<IInputFiles> MakeDiskInputFiles()
{
    return std::make_unique<DiskInputFiles>();
}

} // namespace FastCache::Cc
