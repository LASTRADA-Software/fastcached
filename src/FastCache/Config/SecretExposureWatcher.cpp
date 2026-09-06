// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretExposureWatcher.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace FastCache
{

std::vector<std::string> SecretExposureWatcher::Observe(std::span<std::filesystem::path const> files)
{
    return Transitions(SecretFileExposures(files));
}

std::vector<std::string> SecretExposureWatcher::Transitions(std::span<SecretFileFinding const> found)
{
    std::vector<std::string> fresh;
    for (auto const& finding: found)
    {
        auto const previous =
            std::ranges::find_if(_reported, [&finding](auto const& row) { return row.path == finding.path; });

        // Said already, and still true. Silence: repeating it at every SIGHUP is how
        // an operator learns to scroll past the one line that mattered.
        if (previous != _reported.end() && previous->exposure == finding.exposure)
            continue;

        fresh.push_back(SecretExposureHint(finding.path, finding.exposure));
    }

    // Replaced whole rather than merged, which is what forgets a path that stopped
    // being a subject or stopped being exposed. Both are states the operator's last
    // warning no longer describes, so a return to exposure is a fresh transition and
    // is worth saying again.
    _reported.assign(found.begin(), found.end());
    return fresh;
}

void WatchSecretExposure(ConfigReloader& reloader, bool secretNamedOnCommandLine, SecretExposureReport report)
{
    // `DaemonSecretFiles` rather than a list derived at the call site: the start and
    // every reload must ask the SAME question, and two derivations of one gate is the
    // shape this whole arrangement exists to avoid.
    WatchSecretExposure<Config>(
        reloader,
        [secretNamedOnCommandLine](Config const& cfg) { return DaemonSecretFiles(cfg, secretNamedOnCommandLine); },
        std::move(report));
}

} // namespace FastCache
