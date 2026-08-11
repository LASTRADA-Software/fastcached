// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace FastCache
{

/// Substitute environment variable references in a configuration value.
///
/// Grammar:
///   - `$NAME`    — `NAME` is `[A-Za-z_][A-Za-z0-9_]*`, ending at the first
///                  character outside that set, so `$ProgramData/fastcached`
///                  expands the variable and keeps `/fastcached` literal.
///   - `${NAME}`  — `NAME` is everything up to the closing brace. Needed for
///                  names the bare form cannot spell, notably Windows'
///                  `${ProgramFiles(x86)}`.
///   - `$$`       — a literal `$`.
///
/// Only `$`-notation is recognised. Windows `%NAME%` is left alone, because a
/// bare `%` is valid in a path and silently rewriting it would be worse than
/// not supporting it.
///
/// A reference to an unset variable is an error rather than an empty string:
/// silently expanding `$ProgramData/fastcached/cache` to `/fastcached/cache`
/// would point the store at a path that is merely wrong instead of reporting
/// the missing variable. A variable that is *set but empty* expands to empty,
/// which is a deliberate value rather than a mistake.
///
/// This is applied to path-valued keys only. Secrets are deliberately not
/// expanded: a `$` in a password would otherwise change meaning.
///
/// @param input Raw configuration value, expanded in place of references.
/// @param field Field name carried in the returned ConfigError for diagnostics
///              (e.g. `"storage_path"`).
/// @return The expanded string, or a ConfigError. `UndefinedVariable` when a
///         referenced variable is unset; `ParseError` for a malformed
///         reference (unterminated `${`, empty `${}`, or a `$` that begins
///         neither a name nor an escape).
[[nodiscard]] std::expected<std::string, ConfigError> ExpandEnvironmentVariables(std::string_view input,
                                                                                 std::string_view field);

} // namespace FastCache
