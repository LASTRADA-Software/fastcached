// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>

#include <string>

namespace FastCache
{

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
/// @param cfg The merged configuration in force.
/// @param cli The command-line parse, for its provenance bits.
/// @return True when a non-empty secret is in force and the command line did not
///         supply it, and a file was actually read.
[[nodiscard]] bool SecretCameFromConfigFile(Config const& cfg, CliResult const& cli);

/// What to warn about a secret whose file anyone can read, or nothing.
///
/// The whole of [#384](https://github.com/LASTRADA-Software/fastcached/issues/384)'s
/// startup half, composed from the two halves that can be tested separately:
/// `SecretCameFromConfigFile` (provenance, pure) and
/// `Platform/FileTrust`'s `SecretFileExposure` (the platform's answer about a
/// mode or an access list).
///
/// Empty when there is nothing to say -- no secret, a secret from argv, or a file
/// nothing else can read. **`Undetermined` is reported**, because a platform that
/// would not answer is not a platform that answered "safe"; it is a sentence
/// saying so, which is cheap and is the only honest option.
///
/// @param cfg The merged configuration in force.
/// @param cli The command-line parse, for its provenance bits.
/// @return A sentence naming the file, the exposure and the remedy, or empty.
[[nodiscard]] std::string SecretFileWarning(Config const& cfg, CliResult const& cli);

} // namespace FastCache
