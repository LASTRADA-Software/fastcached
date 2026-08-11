// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// Whose config a candidate location belongs to.
///
/// The enumerator order *is* the priority order: a static_assert requires the
/// candidate table to be sorted by this, so inserting a scope in the wrong
/// position here silently reorders the lookup rather than failing.
enum class ConfigScope : std::uint8_t
{
    User,   ///< Per-account; outranks System so an operator can override the machine default.
    System, ///< Machine-wide; the file the installer writes.
};

/// One candidate location probed for a config file when no `--config` is given.
///
/// The table of these is the single source of truth for the lookup order, for
/// the list `--help` prints, and for what the documentation claims — so a new
/// location is a new row, not a new branch in three places.
struct ConfigCandidate
{
    /// Human-readable form for `--help` and the docs, e.g.
    /// `"~/.config/fastcached/fastcached.yaml"`. Spelled symbolically because
    /// the real path is only known once the environment is read.
    std::string_view display;

    /// Environment variable holding the base directory. Empty means `suffix`
    /// is already an absolute path. A variable that is unset (or set but
    /// empty) skips the row entirely, which is what gives XDG's
    /// "`$XDG_CONFIG_HOME`, else `~/.config`" rule with no special case.
    std::string_view baseVar;

    /// Path appended to the resolved base (or used as-is when `baseVar` is empty).
    std::string_view suffix;

    ConfigScope scope; ///< Which of the two roles this row fills.
};

/// Environment + filesystem seam for the default-config lookup.
///
/// The lookup is pure decision logic over these two questions, so injecting
/// them keeps it testable without touching the real environment or disk — the
/// project's standing rule for anything ambient.
class IConfigPathProbe
{
  public:
    IConfigPathProbe() = default;
    IConfigPathProbe(IConfigPathProbe const&) = delete;
    IConfigPathProbe(IConfigPathProbe&&) = delete;
    IConfigPathProbe& operator=(IConfigPathProbe const&) = delete;
    IConfigPathProbe& operator=(IConfigPathProbe&&) = delete;
    virtual ~IConfigPathProbe() = default;

    /// @param name Environment variable name.
    /// @return Its value, or nullopt when unset.
    [[nodiscard]] virtual std::optional<std::string> GetEnv(std::string_view name) const = 0;

    /// @param path Candidate file.
    /// @return true when `path` names an existing file that can actually be
    ///         opened for reading. Readability, not mere existence: the macOS
    ///         system config is mode 0640 root:_fastcached, and a per-user
    ///         agent must fall through it rather than fail to start.
    [[nodiscard]] virtual bool IsReadableFile(std::filesystem::path const& path) const = 0;
};

/// The production probe: the real process environment and the real filesystem.
class SystemConfigPathProbe final: public IConfigPathProbe
{
  public:
    [[nodiscard]] std::optional<std::string> GetEnv(std::string_view name) const override;
    [[nodiscard]] bool IsReadableFile(std::filesystem::path const& path) const override;
};

/// The platform's candidate locations, in priority order (user before system).
/// @return A view of the compiled-in table; never empty.
[[nodiscard]] std::span<ConfigCandidate const> DefaultConfigCandidates() noexcept;

/// Expand one candidate against the environment, without touching the disk.
/// @param candidate Row to expand.
/// @param probe Environment source.
/// @return The absolute path, or nullopt when the row's `baseVar` is unset or
///         empty (meaning this location does not apply to this process).
[[nodiscard]] std::optional<std::filesystem::path> ExpandConfigCandidate(ConfigCandidate const& candidate,
                                                                         IConfigPathProbe const& probe);

/// Find the config file to use when no `--config` was given.
/// @param probe Environment and filesystem source.
/// @return The first candidate that expands and is readable, or nullopt when
///         none is — in which case the daemon runs on its built-in defaults.
[[nodiscard]] std::optional<std::filesystem::path> ResolveDefaultConfigPath(IConfigPathProbe const& probe);

/// The config file a run should actually read.
///
/// Encodes the rule that a path the operator *named* is strict while one the
/// daemon *found* is not: `named` is returned verbatim, whether or not it
/// exists, so a typo fails loudly downstream instead of silently falling back
/// to different settings; a discovered path is returned only when it is
/// readable. Lives here rather than at the call site so the rule is testable.
///
/// @param named The `--config` value, empty when the operator gave none.
/// @param probe Environment and filesystem source.
/// @return The path to read, or an empty path when there is none and the
///         built-in defaults apply.
[[nodiscard]] std::filesystem::path EffectiveConfigPath(std::string_view named, IConfigPathProbe const& probe);

/// Where an installer should write the machine-wide config.
///
/// Fallible rather than optional, unlike ResolveDefaultConfigPath: "no
/// candidate" is a legitimate answer when *looking* for a config, but a caller
/// that intends to *write* one has nowhere to go, and only this module knows
/// which row failed and why. Returning the diagnosis keeps callers from
/// reassembling it out of exported table columns.
///
/// @param probe Environment source.
/// @return The machine-wide path, whether or not it exists yet, or
///         `UndefinedVariable` when the location's base variable is unset
///         (only reachable where that location is environment-derived).
[[nodiscard]] std::expected<std::filesystem::path, ConfigError> SystemConfigPath(IConfigPathProbe const& probe);

/// What SeedConfigFile did.
enum class SeedOutcome : std::uint8_t
{
    Written,        ///< The destination did not exist and now holds a copy of the template.
    AlreadyPresent, ///< The destination existed and was left untouched.
};

/// Copy `templatePath` to `destination` only when `destination` does not exist,
/// creating parent directories as needed.
///
/// Seed-once, never overwrite: this is how a packaging format with no conffile
/// mechanism (a macOS .pkg, an MSI) ships a default config without discarding
/// the operator's edits on the next upgrade.
///
/// Deliberately NOT routed through IConfigPathProbe, unlike the lookup above.
/// The seam exists so the *decision* of which path to read can be tested
/// without a disk; this is a one-shot installer action whose entire purpose is
/// to write to the real filesystem, and a faked version of it would assert
/// nothing worth knowing.
///
/// @param templatePath The shipped, replaceable copy.
/// @param destination Where the live config belongs.
/// @return What happened, or a ConfigError when the template is missing or the
///         copy fails.
[[nodiscard]] std::expected<SeedOutcome, ConfigError> SeedConfigFile(std::filesystem::path const& templatePath,
                                                                     std::filesystem::path const& destination);

} // namespace FastCache
