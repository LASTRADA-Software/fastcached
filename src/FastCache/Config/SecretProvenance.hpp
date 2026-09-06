// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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

/// One path-valued flag whose file holds a secret.
///
/// A row rather than a branch, so a second secret-bearing flag is a second row and
/// the loop that asks about them is written once.
///
/// **One row type for both executables**, because the two copies differed only in a
/// type: `NodeConfig` spells a path as `std::filesystem::path` and `Config` spells it
/// as `std::string`. Two structs of two fields each, differing by a type, is exactly
/// the repetition a table exists to delete -- and it is what lets ONE coverage guard
/// be written against both
/// ([#864](https://github.com/LASTRADA-Software/fastcached/issues/864)).
///
/// @tparam ConfigT The configuration a flag's value lands in.
/// @tparam PathT How that configuration spells a filesystem path.
template <typename ConfigT, typename PathT = std::filesystem::path>
struct SecretFileRow
{
    std::string_view flag; ///< The `--flag` spelling, and the key the coverage guard joins on.
    PathT ConfigT::* path; ///< Where the operator's answer lands.
};

/// One path-valued flag whose file is deliberately NOT a secret.
///
/// **Mandatory classification with a named opt-out, rather than an opt-in list.**
/// An opt-in list is exact about the flags it knows and silent about the ones it
/// does not, and silence reads identically to complete coverage -- which is #492,
/// and which is precisely how #752 describes the narrow answer that "looks
/// complete". Each binary's coverage case requires every `=<path>` row of its own
/// option table to appear in exactly one of its two tables, so a further
/// path-valued flag does not compile into silence: its author has to say which kind
/// it is.
struct PublicPathFlag
{
    std::string_view flag; ///< The `--flag` spelling.
    std::string_view why;  ///< Why its file holds no secret. A forcing function, not a dead field.
};

/// Every path-valued DAEMON flag whose file holds a secret.
///
/// One row, and the measurement is the point rather than the count: of the six
/// `=<path>` rows of `CliOptions()`, `--tls-key` is the only one naming a
/// credential. It is the private key terminating TLS on the CACHE port, so a
/// world-readable one lets any local account impersonate this daemon to its clients
/// or decrypt a captured session -- and `--requirepass` travels over that same
/// connection, so the exposure composes with the one #384 was written about
/// ([#864](https://github.com/LASTRADA-Software/fastcached/issues/864)).
///
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<SecretFileRow<Config, std::string> const> DaemonSecretFileTable() noexcept;

/// Every path-valued DAEMON flag whose file holds no secret, and why.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<PublicPathFlag const> DaemonPublicPathFlags() noexcept;

/// Every file the DAEMON's secrets live in, in the order to report them.
///
/// The daemon's half of what `Node::NodeSecretFiles` is for the worker: which files
/// this binary's secrets live in, gate applied, ready for `SecretFileWarnings` or for
/// `SecretExposureWatcher`. Two callers ask it -- the start and every reload -- and a
/// subject list each of them derived for itself is the shape #396 and #726 paid for.
///
/// **Two rules, not one, and only the first is #384's.** The configuration file is
/// provenance-gated: `--requirepass` can arrive in argv instead, where the exposure is
/// `ps` rather than a mode, and that is a different problem with a different owner.
/// Every file `DaemonSecretFileTable()` names is not gated at all -- the path is not
/// the secret and the file is, so a world-readable TLS private key is exposed however
/// its path was named
/// ([#864](https://github.com/LASTRADA-Software/fastcached/issues/864)). That is #752's
/// rule, and it was never specific to the worker.
///
/// **A named file is reported whether or not a tier reads it.** `--tls-key` with no
/// `--tls` is a key sitting on disk, and whether a surface exists is not a fact about
/// the configuration -- a rule whose premise is "somebody will read this" cannot state
/// its premise without guessing.
///
/// The provenance half is the whole of
/// [#384](https://github.com/LASTRADA-Software/fastcached/issues/384)'s gate, composed
/// from the two halves that can be tested separately: `SecretCameFromConfigFile` over
/// the facts (provenance, pure) and `Platform/FileTrust`'s `SecretFileExposure` (the
/// platform's answer about a mode or an access list), which whoever renders this list
/// then asks.
///
/// Empty when there is nothing to look at -- no secret named anywhere. **`Undetermined`
/// is still reported** by the renderer, because a platform that would not answer is not
/// a platform that answered "safe".
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
/// @return The paths, configuration file first when it qualifies; empties are kept
///         out. The shape is the worker's `NodeSecretFiles`, so a reader meets one
///         answer twice.
[[nodiscard]] std::vector<std::filesystem::path> DaemonSecretFiles(Config const& cfg, bool secretNamedOnCommandLine);

} // namespace FastCache
