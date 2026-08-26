// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostMemory.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <sys/types.h>
#else
    #include <charconv>
    #include <fstream>
    #include <string>
    #include <string_view>
    #include <system_error>
#endif

namespace FastCache
{

#if defined(_WIN32)

std::size_t QueryHostTotalMemoryBytes() noexcept
{
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    if (!::GlobalMemoryStatusEx(&status))
        return 0;
    return static_cast<std::size_t>(status.ullTotalPhys);
}

#elif defined(__APPLE__)

std::size_t QueryHostTotalMemoryBytes() noexcept
{
    auto value = std::uint64_t { 0 };
    auto size = sizeof(value);
    if (::sysctlbyname("hw.memsize", &value, &size, nullptr, 0) != 0)
        return 0;
    return static_cast<std::size_t>(value);
}

#else // Linux and other POSIX with /proc

namespace
{

    /// The host's physical RAM per /proc/meminfo, or 0 if it could not be read.
    [[nodiscard]] std::uint64_t QueryMemTotalBytes() noexcept
    {
        std::ifstream in { "/proc/meminfo" };
        if (!in.is_open())
            return 0;

        std::string line;
        while (std::getline(in, line))
        {
            constexpr std::string_view Prefix { "MemTotal:" };
            if (!std::string_view { line }.starts_with(Prefix))
                continue;

            auto rest = std::string_view { line }.substr(Prefix.size());
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                rest.remove_prefix(1);

            auto digits = rest;
            auto end = std::size_t { 0 };
            while (end < digits.size() && digits[end] >= '0' && digits[end] <= '9')
                ++end;
            digits = digits.substr(0, end);

            auto kib = std::uint64_t { 0 };
            auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), kib);
            if (ec != std::errc {} || ptr != digits.data() + digits.size())
                return 0;
            return kib * 1024U;
        }
        return 0;
    }

    /// Read a cgroup pseudo-file holding a single unsigned decimal.
    ///
    /// Returns 0 for anything that is not a plain number, which is also how cgroup
    /// v2's literal "max" for an unconstrained controller lands here.
    [[nodiscard]] std::uint64_t ReadUnsignedFile(char const* path) noexcept
    {
        std::ifstream in { path };
        if (!in.is_open())
            return 0;

        std::string text;
        if (!(in >> text))
            return 0;

        auto value = std::uint64_t { 0 };
        auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc {} || ptr != text.data() + text.size())
            return 0;
        return value;
    }

    /// The memory ceiling this process is actually held to, or 0 when unconstrained.
    ///
    /// Inside a container /proc/meminfo still reports the HOST's RAM, so a cache
    /// sized off MemTotal alone is sized for a budget the process will never be
    /// allowed to use and gets OOM-killed on the way there.
    ///
    /// cgroup v1 spells "unlimited" as a sentinel close to the pointer width rather
    /// than omitting the value, and the exact constant varies with page size, hence
    /// a sanity ceiling rather than an equality test against one kernel's number.
    [[nodiscard]] std::uint64_t QueryCgroupMemoryLimitBytes() noexcept
    {
        constexpr auto Unlimited = std::uint64_t { 1 } << 62U;

        for (auto const* path: { "/sys/fs/cgroup/memory.max",                     // cgroup v2
                                 "/sys/fs/cgroup/memory/memory.limit_in_bytes" }) // cgroup v1
            if (auto const limit = ReadUnsignedFile(path); limit != 0 && limit < Unlimited)
                return limit;

        return 0;
    }

} // anonymous namespace

std::size_t QueryHostTotalMemoryBytes() noexcept
{
    auto const memTotal = QueryMemTotalBytes();
    auto const cgroupLimit = QueryCgroupMemoryLimitBytes();

    if (memTotal == 0)
        return static_cast<std::size_t>(cgroupLimit);
    if (cgroupLimit == 0)
        return static_cast<std::size_t>(memTotal);
    return static_cast<std::size_t>(std::min(memTotal, cgroupLimit));
}

#endif

std::size_t DefaultMaxMemoryBytes() noexcept
{
    // Memoised: Config is default-constructed freely -- in tests, on every
    // SIGHUP reload, and for the defaults instance ServiceControl diffs the
    // live config against -- and each call would otherwise re-read /proc or
    // re-enter a syscall.
    //
    // The result always fits a std::size_t without a further clamp: it is
    // either Floor, or total/4 for a total that came from a std::size_t.
    static auto const budget = [] {
        constexpr auto HostShareDivisor = std::uint64_t { 4 };             // a quarter
        constexpr auto Floor = std::uint64_t { 512 } * 1024 * 1024;        // 512 MiB
        constexpr auto Ceiling = std::uint64_t { 8 } * 1024 * 1024 * 1024; // 8 GiB

        auto const total = static_cast<std::uint64_t>(QueryHostTotalMemoryBytes());
        if (total == 0)
            return static_cast<std::size_t>(Floor);

        // Rounded DOWN to a whole MiB, which is not cosmetics. A quarter of a
        // machine's RAM lands on an arbitrary byte count, and two things follow from
        // printing one: `FormatByteSize` is exact by design -- so that what it prints
        // can be handed straight back as a flag value -- and an exact quarter has no
        // whole unit, so the line an operator reads at startup to see how big their
        // cache is becomes `4115271K`, or worse. Rounding down also cannot cross the
        // share: it only ever asks for less. Both bounds are already MiB-aligned, so
        // the clamp is untouched by this.
        constexpr auto Mib = std::uint64_t { 1024 } * 1024;
        auto const share = (total / HostShareDivisor / Mib) * Mib;
        return static_cast<std::size_t>(std::clamp(share, Floor, Ceiling));
    }();

    return budget;
}

} // namespace FastCache
