// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/CpuFeatures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace FastCache;

namespace
{
/// The kernel's own reading of the CPU: the tokens of the first x86 `flags` line
/// of `/proc/cpuinfo`. Empty when the file or the line is not there, which is
/// also what an arm64 kernel gives, since it names that line `Features`.
/// @return The flag tokens.
[[nodiscard]] std::set<std::string> KernelX86CpuFlags()
{
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line))
    {
        if (!line.starts_with("flags"))
            continue;
        auto const colon = line.find(':');
        if (colon == std::string::npos)
            return {};
        std::istringstream tokens(line.substr(colon + 1));
        std::set<std::string> flags;
        std::string token;
        while (tokens >> token)
            flags.insert(token);
        return flags;
    }
    return {};
}
} // namespace

// Compared with a reader that is not CPUID, and never with the architecture this
// test was COMPILED for: an expectation taken from the build agrees with an
// implementation taken from the build on every machine CI owns, whatever either
// says about the host (scripts/check-architecture-conditioned-tests.cmake).
TEST_CASE("DetectCpuFeatures agrees with the kernel's reading of the x86 CPU", "[core][cpufeatures]")
{
    auto const flags = KernelX86CpuFlags();

    // The positive control: every x86-64 CPU has SSE2, so a flag set without it is
    // not an x86 kernel's reading, and every comparison below would agree with
    // nothing. That is what a non-Linux host and an arm64 kernel both look like.
    if (!flags.contains("sse2"))
        SKIP("no x86 flags line in /proc/cpuinfo: not Linux on x86, so there is no second reader to compare with");

    auto const features = DetectCpuFeatures();
    CHECK(features.x86Sha == flags.contains("sha_ni"));
    CHECK(features.x86Ssse3 == flags.contains("ssse3"));
    CHECK(features.x86Sse41 == flags.contains("sse4_1"));
    // An x86 kernel is running this process, so there is no ARM instruction set to report.
    CHECK_FALSE(features.armSha2);
}
