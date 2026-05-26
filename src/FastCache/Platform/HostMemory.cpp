// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostMemory.hpp>

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

std::size_t QueryHostTotalMemoryBytes() noexcept
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
        return static_cast<std::size_t>(kib) * 1024U;
    }
    return 0;
}

#endif

} // namespace FastCache
