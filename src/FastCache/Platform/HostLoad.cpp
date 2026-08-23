// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach/mach_host.h>
#else
    #include <charconv>
    #include <cstdio>
    #include <iterator>
    #include <string>
    #include <string_view>
#endif

namespace FastCache
{

namespace
{
    /// Read this machine's cumulative CPU counters.
    /// @return The counters, or nullopt when the platform would not say.
    [[nodiscard]] std::optional<CpuTicks> ReadCpuTicks() noexcept;

    /// Read memory a new process could actually obtain.
    /// @return The byte count, or nullopt when the platform would not say.
    [[nodiscard]] std::optional<std::uint64_t> ReadAvailableMemory() noexcept;

#if defined(_WIN32)

    /// A Windows FILETIME as one 64-bit count of 100 ns units.
    /// @param value The FILETIME.
    /// @return Its value as an integer.
    [[nodiscard]] std::uint64_t AsTicks(FILETIME const& value) noexcept
    {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    }

    std::optional<CpuTicks> ReadCpuTicks() noexcept
    {
        FILETIME idle {};
        FILETIME kernel {};
        FILETIME user {};
        if (GetSystemTimes(&idle, &kernel, &user) == 0)
            return std::nullopt;

        // `kernel` INCLUDES idle -- documented, and the mistake this comment exists
        // to prevent: treating the three as disjoint reports a busy machine as idle
        // and an idle one as half-loaded, with no symptom but bad scheduling.
        auto const total = AsTicks(kernel) + AsTicks(user);
        auto const idleTicks = AsTicks(idle);
        return CpuTicks { .busy = idleTicks >= total ? 0 : total - idleTicks, .total = total };
    }

    std::optional<std::uint64_t> ReadAvailableMemory() noexcept
    {
        MEMORYSTATUSEX status {};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) == 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(status.ullAvailPhys);
    }

#elif defined(__APPLE__)

    std::optional<CpuTicks> ReadCpuTicks() noexcept
    {
        host_cpu_load_info_data_t info {};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count)
            != KERN_SUCCESS)
            return std::nullopt;

        auto const user = static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_USER]);
        auto const system = static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_SYSTEM]);
        auto const nice = static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_NICE]);
        auto const idle = static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_IDLE]);
        return CpuTicks { .busy = user + system + nice, .total = user + system + nice + idle };
    }

    std::optional<std::uint64_t> ReadAvailableMemory() noexcept
    {
        vm_size_t pageSize = 0;
        if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS)
            return std::nullopt;

        vm_statistics64_data_t stats {};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count)
            != KERN_SUCCESS)
            return std::nullopt;

        // Free plus what the kernel would reclaim on demand. Free alone is what a
        // healthy macOS reports almost none of, so a consumer reading that would
        // conclude every machine that has been up for a day is out of memory.
        auto const reclaimable = static_cast<std::uint64_t>(stats.free_count) + stats.inactive_count + stats.purgeable_count;
        return reclaimable * static_cast<std::uint64_t>(pageSize);
    }

#else

    /// The leading whitespace-separated integer fields of a line, in order.
    /// @param line The text to read.
    /// @param out Where to put them.
    /// @return How many were read.
    [[nodiscard]] std::size_t ReadFields(std::span<char const> line, std::span<std::uint64_t> out) noexcept
    {
        std::size_t found = 0;
        std::size_t at = 0;
        while (found < out.size() && at < line.size())
        {
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
                ++at;
            auto const* const begin = std::next(line.data(), static_cast<std::ptrdiff_t>(at));
            auto const* const end = std::next(line.data(), static_cast<std::ptrdiff_t>(line.size()));
            std::uint64_t value = 0;
            auto const [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc {} || ptr == begin)
                break;
            out[found++] = value;
            at = static_cast<std::size_t>(std::distance(line.data(), ptr));
        }
        return found;
    }

    /// The bytes of `line` after `prefix`, as a bounded range.
    ///
    /// A span rather than a `std::string_view` because `from_chars` needs both
    /// bounds, and a `string_view::data()` handed to any callee is a pointer whose
    /// termination nothing guarantees -- which is the whole of what
    /// `bugprone-suspicious-stringview-data-usage` reports. Carrying the size in the
    /// type removes the question rather than answering it at each call.
    /// @param line The whole line.
    /// @param prefix What to skip.
    /// @return Everything after the prefix.
    [[nodiscard]] std::span<char const> After(std::string const& line, std::string_view prefix) noexcept
    {
        return std::span<char const> { line }.subspan(prefix.size());
    }

    /// Read the first line of a procfs file that begins with `prefix`.
    ///
    /// `FILE*` rather than `std::ifstream`, which is the idiomatic spelling and is
    /// avoided for the reason procfs always forces: these files report a size of
    /// zero and are generated on read.
    /// @param path The file.
    /// @param prefix Only a line starting with this is returned.
    /// @return The line, or nullopt when neither the file nor such a line is there.
    [[nodiscard]] std::optional<std::string> ReadProcLine(char const* path, std::string_view prefix) noexcept
    {
        // "e" is close-on-exec. A compile worker spawns a compiler per job, and a
        // descriptor leaked into one outlives the read that opened it.
        //
        // Owned by a `unique_ptr` rather than closed at each exit: RAII is what the
        // project asks for even where -- especially where -- the resource is a C
        // handle, and it is what `cppcoreguidelines-owning-memory` is checking for.
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> const file { std::fopen(path, "re"), &std::fclose };
        if (file == nullptr)
            return std::nullopt;

        std::array<char, 512> buffer {};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file.get()) != nullptr)
        {
            std::string_view const line { buffer.data() };
            if (line.starts_with(prefix))
                return std::string { line };
        }
        return std::nullopt;
    }

    std::optional<CpuTicks> ReadCpuTicks() noexcept
    {
        constexpr std::string_view Key = "cpu ";
        auto const line = ReadProcLine("/proc/stat", Key);
        if (!line.has_value())
            return std::nullopt;

        // user, nice, system, idle, iowait, irq, softirq, steal. `guest` and
        // `guest_nice` follow and are deliberately NOT summed: the kernel already
        // counts them inside user and nice, so adding them inflates the total and
        // reports a busy machine as idler than it is.
        std::array<std::uint64_t, 8> fields {};
        auto const count = ReadFields(After(*line, Key), fields);
        if (count < 4)
            return std::nullopt;

        std::uint64_t total = 0;
        for (auto const field: std::span { fields }.first(count))
            total += field;

        // iowait counts as idle: the CPU is available to anything that wants it.
        // Counting it as busy would take a machine out of rotation for having a slow
        // disk, which is the opposite of what a compile fleet wants.
        auto const idle = fields[3] + (count > 4 ? fields[4] : 0);
        return CpuTicks { .busy = idle >= total ? 0 : total - idle, .total = total };
    }

    std::optional<std::uint64_t> ReadAvailableMemory() noexcept
    {
        constexpr std::string_view Key = "MemAvailable:";
        auto const line = ReadProcLine("/proc/meminfo", Key);
        if (!line.has_value())
            return std::nullopt;

        std::array<std::uint64_t, 1> fields {};
        if (ReadFields(After(*line, Key), fields) != 1)
            return std::nullopt;
        return fields[0] * 1024; // procfs reports kB
    }

#endif

    /// The real machine, behind the counter seam.
    class SystemCounterSource final: public IHostCounterSource
    {
      public:
        /// @param scratchRoot Filesystem to report free space for; empty for none.
        explicit SystemCounterSource(std::filesystem::path scratchRoot) noexcept:
            _scratchRoot { std::move(scratchRoot) }
        {
        }

        [[nodiscard]] std::optional<CpuTicks> Cpu() override
        {
            return ReadCpuTicks();
        }

        [[nodiscard]] std::optional<std::uint64_t> AvailableMemoryBytes() override
        {
            return ReadAvailableMemory();
        }

        [[nodiscard]] std::optional<std::uint64_t> FreeScratchBytes() override
        {
            if (_scratchRoot.empty())
                return std::nullopt;

            // `QueryDiskSpace` answers zeroes on failure rather than an error, so the
            // zero has to be turned back into "did not say" here. Absent is not zero:
            // a machine whose disk could not be read must be scheduled on its other
            // properties, where one reporting zero cannot compile at all.
            auto const space = QueryDiskSpace(_scratchRoot);
            if (space.freeBytes == 0)
                return std::nullopt;
            return static_cast<std::uint64_t>(space.freeBytes);
        }

      private:
        /// Where this process writes its working files; empty for none.
        std::filesystem::path _scratchRoot;
    };

    /// A sampler over any counter source, differencing consecutive CPU readings.
    class DifferencingSampler final: public IHostLoadSampler
    {
      public:
        /// @param counters Where the readings come from; must not be null.
        explicit DifferencingSampler(std::unique_ptr<IHostCounterSource> counters) noexcept:
            _counters { std::move(counters) }
        {
        }

        HostLoad Sample() override
        {
            HostLoad out { .cpuBusyPermille = std::nullopt,
                           .availableMemoryBytes = _counters->AvailableMemoryBytes(),
                           .freeScratchBytes = _counters->FreeScratchBytes() };

            auto const now = _counters->Cpu();
            if (!now.has_value())
                return out;

            // The arithmetic is `CpuBusyPermille`, which is pure and lives in the
            // header: everything that could be *wrong* about differencing two counters
            // is asserted there without a kernel, a clock or a wait.
            if (_previous.has_value())
                out.cpuBusyPermille = CpuBusyPermille(*_previous, *now);

            // Stored whatever the outcome. A reading that produced no figure -- the
            // first, or one across a counter that went backwards -- is still the right
            // baseline for the next interval; keeping the older one would difference
            // across the very discontinuity that was refused.
            _previous = now;
            return out;
        }

      private:
        std::unique_ptr<IHostCounterSource> _counters;

        /// The previous reading, absent until the first `Sample`.
        ///
        /// Which is why the first call reports no CPU figure: there is nothing to
        /// difference against, and answering zero would say "idle" about a machine
        /// nobody has looked at yet.
        std::optional<CpuTicks> _previous;
    };
} // namespace

std::unique_ptr<IHostCounterSource> MakeSystemCounterSource(std::filesystem::path scratchRoot)
{
    return std::make_unique<SystemCounterSource>(std::move(scratchRoot));
}

std::unique_ptr<IHostLoadSampler> MakeHostLoadSampler(std::unique_ptr<IHostCounterSource> counters)
{
    return std::make_unique<DifferencingSampler>(std::move(counters));
}

} // namespace FastCache
