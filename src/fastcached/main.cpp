// SPDX-License-Identifier: Apache-2.0
//
// fastcached — Fast Cache Daemon entry point.
//
// MVP scaffold. Subsequent passes wire in:
//   - Config (CLI + YAML) parsing
//   - ConfigReloader subscribed to ISignalSource
//   - Reactor + Server bring-up
//   - DaemonHost (POSIX double-fork / Windows SCM)
//
// For now we just print version+build info and exit so the build artefact is
// known-good before the rest of the layers land.

#include <cstddef>
#include <cstdlib>
#include <print>
#include <span>
#include <string_view>

namespace
{

constexpr std::string_view kProgramName = "fastcached";
constexpr std::string_view kVersion = "0.0.1";

int PrintVersion()
{
    std::println("{} {}", kProgramName, kVersion);
    return EXIT_SUCCESS;
}

int PrintUsage()
{
    std::println("usage: {} [--version] [--help]", kProgramName);
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc <= 1)
        return PrintUsage();

    std::span<char* const> const args { argv + 1, static_cast<std::size_t>(argc - 1) };
    for (auto const* raw : args)
    {
        std::string_view const arg { raw };
        if (arg == "--version" || arg == "-V")
            return PrintVersion();
        if (arg == "--help" || arg == "-h")
            return PrintUsage();
    }
    return PrintUsage();
}
