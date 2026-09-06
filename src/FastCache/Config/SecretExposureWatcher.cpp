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
    // Owned by the closures rather than by the caller, so no call site can hand this
    // a lifetime shorter than the reloader's. `Subscribe` has no unsubscribe, so a
    // watcher living in the caller's frame would be a hazard nobody could see.
    auto const watcher = std::make_shared<SecretExposureWatcher>();

    auto observe = [watcher, secretNamedOnCommandLine, report = std::move(report)](Config const& cfg) {
        // Through `DaemonSecretFiles` rather than deriving the subject list here:
        // the start and every reload must ask the SAME question, and two derivations
        // of one gate is the shape this whole change exists to avoid.
        auto const files = DaemonSecretFiles(cfg, secretNamedOnCommandLine);
        for (auto const& warning: watcher->Observe(files))
            report(warning);
    };

    // The startup observation, and it is what SEEDS the memory a reload compares
    // against: a subscription attached WITHOUT it would report a standing exposure a
    // second time at the first SIGHUP, because the memory would still be empty.
    //
    // The claim is about the observation HAPPENING, never about its order relative to
    // `Subscribe` -- observing second would seed the memory just as well, and no
    // reload can be observed before this call returns anyway. It is written first
    // because a rule that does not depend on the order should not be spelled as one:
    // a comment claiming a reordering breaks something is a reason that outruns the
    // fact it was drawn from, which is the failure this file's own rulebook records.
    observe(*reloader.Current());

    // Moved rather than copied: the seeding call above is the last use here, and the
    // subscriber list is where this closure lives from now on.
    reloader.Subscribe([observe = std::move(observe)](auto const& /*previous*/, auto const& current) { observe(*current); });
}

} // namespace FastCache
