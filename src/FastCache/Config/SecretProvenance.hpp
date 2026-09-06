// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace FastCache
{

/// What a start observed about where the secret in force came from.
///
/// The three facts the provenance rule needs, separated from the two
/// configuration types that can supply them. `fastcached` reads them off
/// `Config` and `CliResult`; `fastcache-compile-node` reads them off `NodeConfig`
/// and a command-line-only parse of the same argv -- and the RULE over them is
/// written once, which is the whole reason this struct exists rather than a
/// second predicate spelled against the second pair of types.
///
/// The same acquisition-versus-decision split `Platform/FileTrust` draws between
/// `SecretFileFacts` and `ClassifySecretFile`, and for the same reason: the
/// acquisition is per caller and cannot be shared, so the decision must be.
struct SecretProvenanceFacts
{
    /// A non-empty secret is in force.
    ///
    /// The VALUE decides this and not a provenance bit: an operator who typed
    /// `--requirepass=` on purpose is asking for no authentication, which is not
    /// a secret and not a file's business.
    bool secretInForce { false };

    /// The command line supplied it.
    ///
    /// A secret typed on the command line is a DIFFERENT exposure -- it is in
    /// `ps`, which is what `InlineCredentialRejection` refuses to bake into a
    /// registration -- so this rule declines it. Answering `false` about such a
    /// secret is not a claim that it is safe.
    bool namedOnCommandLine { false };

    /// A configuration file was actually opened and applied.
    ///
    /// A run that declined a discovered file read no file, so there is nothing
    /// whose mode could be protecting anything -- and a warning naming the file
    /// that was passed over would be an alarm about nothing.
    bool fileWasRead { false };
};

/// Is the secret in force only as private as a configuration file?
///
/// The rule, over facts either executable can produce.
/// @param facts What the start observed.
/// @return True when a file's mode is what protects the secret.
[[nodiscard]] constexpr bool SecretCameFromConfigFile(SecretProvenanceFacts const& facts) noexcept
{
    return facts.secretInForce && !facts.namedOnCommandLine && facts.fileWasRead;
}

/// What to warn about each of @p files that anyone else on the machine can read.
///
/// **The provenance question is the CALLER's and is deliberately absent here.**
/// #384's rule is provenance-gated because the secret it protects can also arrive
/// in argv, where the exposure is `ps` rather than a mode. A secret reached BY
/// PATH -- a cluster key, a scheduler or dashboard token, a TLS private key -- has
/// no such second route: the path is not the secret and the file is, so a
/// world-readable key is exposed whether the path was typed or read out of a
/// file, and asking whether it was typed answers a question nobody has
/// ([#752](https://github.com/LASTRADA-Software/fastcached/issues/752)). A caller
/// with a provenance gate applies it before building the list.
///
/// A file that is **not there** is skipped rather than reported `Undetermined`.
/// "The platform would not say who may read this" and "there is nothing here to
/// read" are different states, and folding them would answer a mistyped path with
/// a sentence about permissions while whoever loads the file answers with the real
/// one. A path that exists and cannot be inspected is still reported, which is the
/// state `Undetermined` is for.
///
/// Empty paths are skipped: a setting nobody named names no file.
///
/// One sentence per FILE rather than per setting that named it: a single-machine
/// deployment legitimately points two settings at one file, and the same remedy for
/// the same path twice reads as two problems. Repeats are matched on the path as
/// given -- two spellings of one file are a different question, and answering it
/// here would mean resolving paths this function only inspects.
///
/// @param files The files this process holds secrets in, in the order to report.
/// @return One sentence per exposed file, in @p files order; empty when none is.
[[nodiscard]] std::vector<std::string> SecretFileWarnings(std::span<std::filesystem::path const> files);

/// One file found unfit to hold a secret.
struct SecretFileFinding
{
    /// The file, exactly as the caller named it.
    std::filesystem::path path;

    /// Why it is unfit. Never `None` -- a fit file produces no finding at all.
    SecretExposure exposure { SecretExposure::None };
};

/// Which of @p files are unfit to hold a secret, and why.
///
/// The ANSWER rather than the sentence, published because a caller that has to
/// notice a CHANGE cannot do it on rendered text. `SecretExposureWatcher` remembers
/// what it last said and speaks only on a transition, and a mode loosening under an
/// unchanged secret has to read as one -- so it compares `(path, exposure)` pairs.
/// Comparing sentences would work today and would stop working the moment a hint
/// loses information, which is not a property worth resting a security signal on.
///
/// Every rule `SecretFileWarnings` documents is this function's: empty paths and
/// absent files are skipped, an inspectable file that will not answer is
/// `Undetermined` and IS reported, and one file named twice yields one finding.
/// `SecretFileWarnings` is this composed with `SecretExposureHint`.
///
/// @param files The files this process holds secrets in, in the order to report.
/// @return One finding per exposed file, in @p files order; empty when none is.
[[nodiscard]] std::vector<SecretFileFinding> SecretFileExposures(std::span<std::filesystem::path const> files);

/// Every file the DAEMON's secrets live in, in the order to report them.
///
/// The daemon's half of what `Node::NodeSecretFiles` is for the worker: which files
/// this binary's secrets live in, gate applied, ready for `SecretFileWarnings` or for
/// `SecretExposureWatcher`. Two callers ask it -- the start and every reload -- and a
/// subject list each of them derived for itself is the shape #396 and #726 paid for.
///
/// The whole of [#384](https://github.com/LASTRADA-Software/fastcached/issues/384)'s
/// gate, composed from the two halves that can be tested separately:
/// `SecretCameFromConfigFile` over the facts (provenance, pure) and
/// `Platform/FileTrust`'s `SecretFileExposure` (the platform's answer about a mode or
/// an access list), which whoever renders this list then asks.
///
/// Empty when there is nothing to look at -- no secret, or a secret from argv, or no
/// file read. **`Undetermined` is still reported** by the renderer, because a
/// platform that would not answer is not a platform that answered "safe".
///
/// **The provenance BIT rather than the whole `CliResult`**, which is exactly what
/// `Node::NodeSecretFiles` takes and what the rule reads: one bool out of a 584-byte
/// object holding a whole `Config`. The daemon's reload subscriber captures whatever
/// this takes for the life of the process, and `ConfigReloaderOf::Reload()` deep-copies
/// every subscriber on each SIGHUP -- measured at 7 allocations and 828 bytes per
/// reload for the `CliResult` shape against 1 allocation and 56 bytes for this one,
/// duplicating a configuration snapshot nothing reads.
///
/// @param cfg The merged configuration in force.
/// @param secretNamedOnCommandLine Whether argv supplied `--requirepass`; the parse's
///        own `CliResult::requirePassExplicit`, never a value comparison.
/// @return The configuration file when its mode is what protects the secret, else
///         nothing. Never more than one element today; a list because the daemon has
///         its own path-reached secret still to wire
///         ([#864](https://github.com/LASTRADA-Software/fastcached/issues/864)), and
///         because the shape is the worker's `NodeSecretFiles` so a reader meets one
///         answer twice.
[[nodiscard]] std::vector<std::filesystem::path> DaemonSecretFiles(Config const& cfg, bool secretNamedOnCommandLine);

} // namespace FastCache
