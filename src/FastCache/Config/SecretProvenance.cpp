// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace FastCache
{

std::vector<SecretFileFinding> SecretFileExposures(std::span<std::filesystem::path const> files)
{
    std::vector<SecretFileFinding> findings;
    for (auto const& path: files)
    {
        if (path.empty())
            continue;

        // One finding per FILE, not per setting that named it. A single-machine
        // deployment legitimately points two settings at one file -- the cluster key
        // and the scheduler token, say -- and an operator handed the same remedy for
        // the same path twice reads the second copy as a second problem, which is the
        // alarm-fatigue failure this whole check is trying not to be.
        //
        // Over the findings themselves rather than a parallel list of paths already
        // asked about: a repeated path that is CLEAN or absent yields nothing either
        // way, so the two differ only in one extra `stat` over a list bounded at five
        // files -- and a second container a reader has to check stays in step with the
        // one that matters is the more expensive of the two.
        if (std::ranges::any_of(findings, [&path](auto const& found) { return found.path == path; }))
            continue;

        // `status` rather than `exists`, because the two answers this has to keep
        // apart are exactly the ones `exists` folds: a file that is not there
        // reports `not_found`, while one whose directory cannot be searched reports
        // `none` with the error set -- and that second state is the `Undetermined`
        // the exposure rule is required to report rather than swallow.
        std::error_code ec;
        if (std::filesystem::status(path, ec).type() == std::filesystem::file_type::not_found)
            continue;

        auto const exposure = SecretFileExposure(path);
        if (exposure == SecretExposure::None)
            continue;

        findings.push_back(SecretFileFinding { .path = path, .exposure = exposure });
    }
    return findings;
}

std::vector<std::string> SecretFileWarnings(std::span<std::filesystem::path const> files)
{
    std::vector<std::string> warnings;
    for (auto const& finding: SecretFileExposures(files))
        warnings.push_back(SecretExposureHint(finding.path, finding.exposure));
    return warnings;
}

std::vector<std::filesystem::path> DaemonSecretFiles(Config const& cfg, bool secretNamedOnCommandLine)
{
    if (!SecretCameFromConfigFile(SecretProvenanceFacts {
            // No secret in force: nothing to protect, and a file's mode is not this
            // daemon's business.
            .secretInForce = !cfg.requirePass.empty(),

            // The command line supplied it, so the file is not what is protecting it --
            // and the exposure is `ps`, which is a different problem with a different
            // answer (`InlineCredentialRejection`'s).
            .namedOnCommandLine = secretNamedOnCommandLine,

            // And a file has to have actually been read. `configPath` is the resolved
            // path, set by the assembly only when a file was opened -- so a run that
            // declined a discovered file leaves it empty and this answers false, rather
            // than warning about a file nothing read.
            .fileWasRead = !cfg.configPath.empty(),
        }))
        return {};

    return { std::filesystem::path { cfg.configPath } };
}

} // namespace FastCache
