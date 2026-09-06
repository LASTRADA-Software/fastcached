// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
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

std::span<SecretFileRow<Config, std::string> const> DaemonSecretFileTable() noexcept
{
    static constexpr auto table = std::to_array<SecretFileRow<Config, std::string>>({
        // The private key terminating TLS on the CACHE port. World-readable, any
        // local account can impersonate this daemon to its clients or decrypt a
        // captured session -- and `--requirepass` travels over that same connection,
        // so this exposure composes with the one #384 was written about.
        { .flag = "--tls-key", .path = &Config::tlsKeyPath },
    });
    return table;
}

std::span<PublicPathFlag const> DaemonPublicPathFlags() noexcept
{
    static constexpr auto table = std::to_array<PublicPathFlag>({
        { .flag = "--config",
          .why = "the file itself holds no secret by construction; whether the secret IN it is exposed is the "
                 "provenance-gated question DaemonSecretFiles asks separately" },
        // Explicitly classified rather than left off, because it is the one an author
        // would reach for by symmetry with `--tls-key`. A certificate is handed to
        // every client during the handshake, so it is public BY CONSTRUCTION -- and
        // warning about the mode of a file that is MEANT to be readable is the alarm
        // that teaches operators to ignore the one that matters.
        { .flag = "--tls-cert", .why = "a certificate is presented to every client during the handshake" },
        // Not a credential: nothing in the store is a key or a token, and the check
        // is about files that hold a SECRET rather than files that hold data. Its
        // mode is the operator's to choose -- `--requirepass` gates the network
        // surface, never the filesystem, and warning here would tell an operator to
        // tighten a file this daemon has no rule about.
        { .flag = "--storage", .why = "a store of cached values; data rather than a credential" },
        { .flag = "--seed-config",
          .why = "a template copied to the machine-wide location; --seed-config is what SECURES that "
                 "destination, and the source it copies from carries no secret" },
        { .flag = "--pidfile", .why = "a process id, which every process list already publishes" },
    });
    return table;
}

std::vector<std::filesystem::path> DaemonSecretFiles(Config const& cfg, bool secretNamedOnCommandLine)
{
    std::vector<std::filesystem::path> files;

    // First, because it is the one an operator most often has open. This half alone
    // is provenance-gated: `--requirepass` typed in argv is a `ps` exposure, which is
    // a different problem with a different owner.
    if (SecretCameFromConfigFile(SecretProvenanceFacts {
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
        files.emplace_back(cfg.configPath);

    // Not gated at all, and that is #752's rule rather than an omission of #384's:
    // the path is not the secret and the file is, so a world-readable private key is
    // exposed whether its path was typed or read out of a configuration file.
    for (auto const& row: DaemonSecretFileTable())
        if (auto const& path = cfg.*row.path; !path.empty())
            files.emplace_back(path);

    return files;
}

} // namespace FastCache
