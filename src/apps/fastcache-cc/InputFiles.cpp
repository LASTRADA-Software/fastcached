// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "InputFiles.hpp"

#include <FastCache/Platform/NarrowText.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

#include <cstring>
#include <ctime>

namespace FastCache::Cc
{

namespace
{
#if !defined(_WIN32)
    /// How far a FINE clock read is moved back, where no coarse clock answers. It bounds the lag of
    /// a kernel's tick-granular file stamping behind a fine clock: a change made just after a fine
    /// read can be stamped up to one tick earlier than that read. 10 ms is one tick at 100 Hz, the
    /// coarsest tick in common use.
    constexpr FileInstant FineClockSafetyTick = 10'000'000;

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
#if defined(_WIN32)
            FILETIME now {};
            GetSystemTimeAsFileTime(&now);
            return FromFileTime(
                static_cast<std::int64_t>((std::uint64_t { now.dwHighDateTime } << 32U) | now.dwLowDateTime));
#else
            timespec now {};
    #if defined(CLOCK_REALTIME_COARSE)
            if (::clock_gettime(CLOCK_REALTIME_COARSE, &now) == 0)
                return FromTimespec(now);
    #endif
            if (::clock_gettime(CLOCK_REALTIME, &now) == 0)
                return FromTimespec(now) - FineClockSafetyTick;
            // No clock answered. Zero precedes every change time, so every input reads as changed:
            // the invocation fails closed rather than trusting a witness it cannot time.
            return 0;
#endif
        }
    };
} // namespace

std::unique_ptr<IInputFiles> MakeDiskInputFiles()
{
    return std::make_unique<DiskInputFiles>();
}

} // namespace FastCache::Cc
