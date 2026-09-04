// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <filesystem>
#include <string>

namespace FastCache
{

bool SecretCameFromConfigFile(Config const& cfg, CliResult const& cli)
{
    // No secret in force: nothing to protect, and a file's mode is not this
    // daemon's business.
    if (cfg.requirePass.empty())
        return false;

    // The command line supplied it, so the file is not what is protecting it --
    // and the exposure is `ps`, which is a different problem with a different
    // answer (`InlineCredentialRejection`'s).
    if (cli.requirePassExplicit)
        return false;

    // And a file has to have actually been read. `configPath` is the resolved
    // path, set by the assembly only when a file was opened -- so a run that
    // declined a discovered file leaves it empty and this answers false, rather
    // than warning about a file nothing read.
    return !cfg.configPath.empty();
}

std::string SecretFileWarning(Config const& cfg, CliResult const& cli)
{
    if (!SecretCameFromConfigFile(cfg, cli))
        return {};

    std::filesystem::path const path { cfg.configPath };
    auto const exposure = SecretFileExposure(path);
    if (exposure == SecretExposure::None)
        return {};

    return SecretExposureHint(path, exposure);
}

} // namespace FastCache
