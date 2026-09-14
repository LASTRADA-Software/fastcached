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

#include <array>
#include <cstddef>
#include <cstdint>

namespace FastCache
{

#if defined(_M_X64) || defined(__x86_64__)
namespace
{
    /// The four registers one CPUID query answers in.
    struct CpuidRegisters
    {
        std::uint32_t eax = 0;
        std::uint32_t ebx = 0;
        std::uint32_t ecx = 0;
        std::uint32_t edx = 0;
    };

    /// Which register a feature bit is read from.
    enum class CpuidRegister : std::uint8_t
    {
        Ebx,
        Ecx,
    };

    /// One feature, as a bit of one CPUID leaf.
    struct CpuidFeatureBit
    {
        std::uint32_t leaf;
        CpuidRegister reg;
        std::uint32_t bit;
        bool CpuFeatures::* feature;
    };

    /// Every x86 feature this tree asks about. Subleaf 0 throughout.
    constexpr std::array<CpuidFeatureBit, 3> CpuidFeatureBits { {
        { .leaf = 1, .reg = CpuidRegister::Ecx, .bit = 9, .feature = &CpuFeatures::x86Ssse3 },
        { .leaf = 1, .reg = CpuidRegister::Ecx, .bit = 19, .feature = &CpuFeatures::x86Sse41 },
        { .leaf = 7, .reg = CpuidRegister::Ebx, .bit = 29, .feature = &CpuFeatures::x86Sha },
    } };

    /// Query one CPUID leaf, subleaf 0.
    /// @param leaf The leaf.
    /// @return Its registers; all zero where the query is refused.
    [[nodiscard]] CpuidRegisters Cpuid(std::uint32_t leaf) noexcept
    {
    #if defined(_MSC_VER)
        std::array<int, 4> registers {};
        __cpuidex(registers.data(), static_cast<int>(leaf), 0);
        return { .eax = static_cast<std::uint32_t>(registers[0]),
                 .ebx = static_cast<std::uint32_t>(registers[1]),
                 .ecx = static_cast<std::uint32_t>(registers[2]),
                 .edx = static_cast<std::uint32_t>(registers[3]) };
    #else
        CpuidRegisters registers;
        if (__get_cpuid_count(leaf, 0, &registers.eax, &registers.ebx, &registers.ecx, &registers.edx) == 0)
            return {};
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
    for (auto const& row: CpuidFeatureBits)
    {
        if (row.leaf > highestLeaf)
            continue;
        auto const registers = Cpuid(row.leaf);
        auto const value = row.reg == CpuidRegister::Ebx ? registers.ebx : registers.ecx;
        features.*row.feature = ((value >> row.bit) & 1U) != 0;
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    int present = 0;
    std::size_t size = sizeof(present);
    features.armSha2 = ::sysctlbyname("hw.optional.arm.FEAT_SHA256", &present, &size, nullptr, 0) == 0 && present != 0;
#endif
    // Anything else, Linux aarch64 and Windows ARM64 included, detects nothing: no CI leg
    // compiles a detection branch there (#ISSUE), and the scalar paths are correct.

    return features;
}

} // namespace FastCache
