// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/CpuFeatures.hpp>

#if defined(_M_X64) || defined(__x86_64__)
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
    #endif
#elif defined(__APPLE__) && defined(__aarch64__)
    #include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace FastCache
{

#if defined(_M_X64) || defined(__x86_64__)
namespace
{
    /// The four registers one CPUID query answers in, in that order.
    struct CpuidRegisters
    {
        std::uint32_t eax = 0;
        std::uint32_t ebx = 0;
        std::uint32_t ecx = 0;
        std::uint32_t edx = 0;
    };

    /// One feature, as a bit of one register of one CPUID leaf, subleaf 0.
    struct CpuidFeatureBit
    {
        std::uint32_t leaf;
        std::uint32_t CpuidRegisters::* reg;
        std::uint32_t bit;
        bool CpuFeatures::* feature;
    };

    /// Every x86 feature this tree asks about, grouped by leaf so each leaf is queried once.
    constexpr std::array<CpuidFeatureBit, 3> CpuidFeatureBits { {
        { .leaf = 1, .reg = &CpuidRegisters::ecx, .bit = 9, .feature = &CpuFeatures::x86Ssse3 },
        { .leaf = 1, .reg = &CpuidRegisters::ecx, .bit = 19, .feature = &CpuFeatures::x86Sse41 },
        { .leaf = 7, .reg = &CpuidRegisters::ebx, .bit = 29, .feature = &CpuFeatures::x86Sha },
    } };
    static_assert(std::ranges::is_sorted(CpuidFeatureBits, {}, &CpuidFeatureBit::leaf));

    /// Query one CPUID leaf, subleaf 0, unchecked: the caller compares the leaf
    /// with the highest one the CPU supports first.
    /// @param leaf The leaf.
    /// @return Its registers.
    [[nodiscard]] CpuidRegisters Cpuid(std::uint32_t leaf) noexcept
    {
    #if defined(_MSC_VER)
        std::array<int, 4> registers {};
        __cpuidex(registers.data(), static_cast<int>(leaf), 0);
        return std::bit_cast<CpuidRegisters>(registers);
    #else
        CpuidRegisters registers;
        __cpuid_count(leaf, 0, registers.eax, registers.ebx, registers.ecx, registers.edx);
        return registers;
    #endif
    }
} // namespace
#endif

CpuFeatures DetectCpuFeatures() noexcept
{
    CpuFeatures features;

#if defined(_M_X64) || defined(__x86_64__)
    // A leaf above the highest one the CPU supports answers with the highest
    // leaf's data on Intel, which would read an unrelated register as a feature.
    auto const highestLeaf = Cpuid(0).eax;
    auto queriedLeaf = std::uint32_t { 0 };
    CpuidRegisters registers;
    for (auto const& row: CpuidFeatureBits)
    {
        if (row.leaf > highestLeaf)
            continue;
        if (row.leaf != queriedLeaf)
        {
            registers = Cpuid(row.leaf);
            queriedLeaf = row.leaf;
        }
        features.*row.feature = ((registers.*row.reg >> row.bit) & 1U) != 0;
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    int present = 0;
    std::size_t size = sizeof(present);
    features.armSha2 = ::sysctlbyname("hw.optional.arm.FEAT_SHA256", &present, &size, nullptr, 0) == 0 && present != 0;
#endif
    // Linux aarch64 and Windows ARM64 detect nothing until a CI leg compiles a branch for them (#1432).

    return features;
}

} // namespace FastCache
