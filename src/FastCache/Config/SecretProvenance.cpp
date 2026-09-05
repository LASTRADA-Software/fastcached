// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace FastCache
{

bool SecretCameFromConfigFile(Config const& cfg, CliResult const& cli)
{
    return SecretCameFromConfigFile(SecretProvenanceFacts {
        // No secret in force: nothing to protect, and a file's mode is not this
        // daemon's business.
        .secretInForce = !cfg.requirePass.empty(),

        // The command line supplied it, so the file is not what is protecting it --
        // and the exposure is `ps`, which is a different problem with a different
        // answer (`InlineCredentialRejection`'s).
        .namedOnCommandLine = cli.requirePassExplicit,

        // And a file has to have actually been read. `configPath` is the resolved
        // path, set by the assembly only when a file was opened -- so a run that
        // declined a discovered file leaves it empty and this answers false, rather
        // than warning about a file nothing read.
        .fileWasRead = !cfg.configPath.empty(),
    });
}

std::vector<std::string> SecretFileWarnings(std::span<std::filesystem::path const> files)
{
    std::vector<std::string> warnings;
    std::vector<std::filesystem::path> reported;
    for (auto const& path: files)
    {
        if (path.empty())
            continue;

        // One sentence per FILE, not per setting that named it. A single-machine
        // deployment legitimately points two settings at one file -- the cluster key
        // and the scheduler token, say -- and an operator handed the same remedy for
        // the same path twice reads the second copy as a second problem, which is the
        // alarm-fatigue failure this whole check is trying not to be.
        if (std::ranges::find(reported, path) != reported.end())
            continue;
        reported.push_back(path);

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

        warnings.push_back(SecretExposureHint(path, exposure));
    }
    return warnings;
}

std::string SecretFileWarning(Config const& cfg, CliResult const& cli)
{
    if (!SecretCameFromConfigFile(cfg, cli))
        return {};

    // Through the aggregate rather than beside it: how a path becomes a sentence is
    // one function, so the daemon's config file and the worker's four key files
    // cannot come to disagree about what an exposure reads like.
    std::array<std::filesystem::path, 1> const files { std::filesystem::path { cfg.configPath } };
    auto const warnings = SecretFileWarnings(files);
    return warnings.empty() ? std::string {} : warnings.front();
}

} // namespace FastCache
