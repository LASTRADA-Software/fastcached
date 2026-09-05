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
    auto found = SecretFileExposures(files);

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
    _reported = std::move(found);
    return fresh;
}

void WatchSecretExposure(ConfigReloader& reloader, CliResult cli, SecretExposureReport report)
{
    // Owned by the closures rather than by the caller, so no call site can hand this
    // a lifetime shorter than the reloader's. `Subscribe` has no unsubscribe, so a
    // watcher living in the caller's frame would be a hazard nobody could see.
    auto const watcher = std::make_shared<SecretExposureWatcher>();

    auto const observe = [watcher, cli = std::move(cli), report = std::move(report)](Config const& cfg) {
        // Through `DaemonSecretFiles` rather than deriving the subject list here:
        // the start and every reload must ask the SAME question, and two derivations
        // of one gate is the shape this whole change exists to avoid.
        auto const files = DaemonSecretFiles(cfg, cli);
        for (auto const& warning: watcher->Observe(files))
            report(warning);
    };

    // BEFORE the subscription, so the memory a reload compares against holds what an
    // operator was already told. Subscribing first would report a standing exposure a
    // second time at the first SIGHUP.
    observe(*reloader.Current());

    reloader.Subscribe([observe](auto const& /*previous*/, auto const& current) { observe(*current); });
}

} // namespace FastCache
