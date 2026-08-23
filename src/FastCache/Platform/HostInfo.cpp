// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/CpuAffinity.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/utsname.h>
#endif

namespace FastCache
{

namespace
{
    /// The architecture this binary was compiled for.
    ///
    /// A compile-time answer on purpose; see `HostFacts::architecture` for why the
    /// OS's answer would be the wrong one under emulation.
    /// @return The architecture name, or "unknown" for one nothing here names.
    [[nodiscard]] constexpr std::string_view CompiledArchitecture() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
        return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
        return "x86";
#elif defined(__arm__) || defined(_M_ARM)
        return "arm";
#elif defined(__riscv) && __riscv_xlen == 64
        return "riscv64";
#elif defined(__powerpc64__)
        return "ppc64";
#else
        // Named rather than left empty: "unknown" tells an operator comparing two
        // nodes that this build does not recognise its own target, which is a
        // different problem from a field nothing filled in.
        return "unknown";
#endif
    }

#if defined(_WIN32)
    /// The OS version, read from the kernel rather than from `GetVersionEx`.
    ///
    /// `GetVersionEx` is deprecated and, worse, *lies* by design: without an
    /// application manifest declaring compatibility it reports 6.2 on every
    /// Windows since 8, so two nodes on different builds would advertise the same
    /// version. `RtlGetVersion` is the documented way to get the real one and is
    /// not manifest-gated.
    /// @return "major.minor.build", or empty when the call fails.
    [[nodiscard]] std::string QueryWindowsVersion()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

        auto* const ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return {};

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- GetProcAddress
        // returns FARPROC and there is no other way to call through it.
        auto* const entry =
            reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        if (entry == nullptr)
            return {};

        RTL_OSVERSIONINFOW info {};
        info.dwOSVersionInfoSize = sizeof(info);
        if (entry(&info) != 0)
            return {};

        return std::to_string(info.dwMajorVersion) + '.' + std::to_string(info.dwMinorVersion) + '.'
               + std::to_string(info.dwBuildNumber);
    }
#endif

    /// Build the facts once.
    /// @return The facts for this machine.
    [[nodiscard]] HostFacts BuildHostFacts()
    {
        auto facts = HostFacts {};
        facts.architecture = std::string { CompiledArchitecture() };

#if defined(_WIN32)
        facts.osName = "Windows";
        facts.osVersion = QueryWindowsVersion();
#else
        utsname info {};
        if (::uname(&info) == 0)
        {
            // `sysname` is "Darwin" on macOS, which is accurate and is not what an
            // operator reading a fleet listing expects to compare against. The
            // release is left exactly as the kernel reports it -- on macOS that is
            // the Darwin version rather than the product version, and translating
            // it would mean carrying a table that goes stale every autumn.
            facts.osName = std::string { static_cast<char const*>(info.sysname) };
            if (facts.osName == "Darwin")
                facts.osName = "macOS";
            facts.osVersion = std::string { static_cast<char const*>(info.release) };
        }
#endif

        return facts;
    }
} // namespace

HostFacts const& QueryHostFacts()
{
    // Function-local static: initialised on first use, thread-safe since C++11,
    // and nothing here changes while the process lives.
    static HostFacts const facts = BuildHostFacts();
    return facts;
}

DiskSpace QueryDiskSpace(std::filesystem::path const& path) noexcept
{
    auto error = std::error_code {};
    auto const space = std::filesystem::space(path, error);
    if (error)
        return {};

    // `available` rather than `free`: the difference is the reserve only root may
    // use, and a compile worker is not root. Reporting `free` would promise space
    // an unprivileged process cannot write into.
    return DiskSpace { .capacityBytes = space.capacity, .freeBytes = space.available };
}

namespace
{
    /// The real machine, behind the facts seam.
    class SystemHostFacts final: public IHostFactsSource
    {
      public:
        [[nodiscard]] HostFacts const& Facts() const override
        {
            return QueryHostFacts();
        }

        [[nodiscard]] std::uint32_t LogicalCores() const override
        {
            // Clamped here rather than at each caller, which is the point of the
            // seam answering instead of the free function: `OnlineCpuCount` can
            // report a machine it could not read as zero, and a caller that divided
            // by it or subtracted a reserve from it would be wrong in a different
            // way each time somebody forgot.
            return std::max(std::uint32_t { 1 }, static_cast<std::uint32_t>(OnlineCpuCount()));
        }

        [[nodiscard]] std::uint64_t TotalMemoryBytes() const override
        {
            return static_cast<std::uint64_t>(QueryHostTotalMemoryBytes());
        }

        [[nodiscard]] DiskSpace SpaceOn(std::filesystem::path const& path) const override
        {
            return QueryDiskSpace(path);
        }
    };
} // namespace

std::unique_ptr<IHostFactsSource> MakeSystemHostFacts()
{
    return std::make_unique<SystemHostFacts>();
}

} // namespace FastCache
