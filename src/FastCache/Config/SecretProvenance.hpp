// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
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

/// Did a secret in force reach this process through a configuration FILE?
///
/// The question `Platform/FileTrust`'s readability check needs an answer to
/// before it is worth asking anything about a file's mode, and it is a
/// **provenance** question: `requirepass` can arrive on the command line, out of
/// a file, or not at all, and only one of those makes a file's permissions the
/// thing that protects it.
///
/// Provenance, never a value comparison -- `OptionSpec::explicitBit` records
/// whether the operator NAMED the flag, and the empty-string default cannot be
/// recovered from the value the way a numeric default sometimes can: an operator
/// who typed `--requirepass=` on purpose is asking for no authentication, which
/// is not a secret and not a file's business.
///
/// A secret typed on the command line is a different exposure -- it is in `ps`,
/// which is exactly what `InlineCredentialRejection` refuses to bake into a
/// registration -- and is deliberately NOT this function's subject. Answering
/// `false` there is not a claim that such a secret is safe.
///
/// The daemon's half of the split above: it reads the three facts off its own
/// types and hands them to the one rule.
///
/// @param cfg The merged configuration in force.
/// @param cli The command-line parse, for its provenance bits.
/// @return True when a non-empty secret is in force and the command line did not
///         supply it, and a file was actually read.
[[nodiscard]] bool SecretCameFromConfigFile(Config const& cfg, CliResult const& cli);

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
/// `SecretCameFromConfigFile` (provenance, pure) and
/// `Platform/FileTrust`'s `SecretFileExposure` (the platform's answer about a mode or
/// an access list), which whoever renders this list then asks.
///
/// Empty when there is nothing to look at -- no secret, or a secret from argv, or no
/// file read. **`Undetermined` is still reported** by the renderer, because a
/// platform that would not answer is not a platform that answered "safe".
///
/// @param cfg The merged configuration in force.
/// @param cli The command-line parse, for its provenance bits.
/// @return The configuration file when its mode is what protects the secret, else
///         nothing. Never more than one element today; a list because the daemon has
///         its own path-reached secret still to wire, and because the shape is the
///         worker's `NodeSecretFiles` so a reader meets one answer twice.
[[nodiscard]] std::vector<std::filesystem::path> DaemonSecretFiles(Config const& cfg, CliResult const& cli);

} // namespace FastCache
