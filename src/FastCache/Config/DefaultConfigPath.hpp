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
#include <vector>

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
    /// empty) skips the row entirely, so `$XDG_CONFIG_HOME` needs no special
    /// case to fall back to `~/.config`.
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

    /// @param path Candidate file, already known to be readable.
    /// @return true when only an administrator could have put a file there.
    ///         Asked of every candidate a *privileged* process would use, for a
    ///         specific attack: `%ProgramData%` subdirectories are
    ///         user-creatable by default, so a standard account could plant the
    ///         configuration a LocalSystem service then obeys. See
    ///         Platform/FileTrust.hpp for why the test is the containing
    ///         directory and not the owner.
    [[nodiscard]] virtual bool IsTrustedSystemLocation(std::filesystem::path const& path) const = 0;

    /// @return true when this process runs with the rights the machine-wide
    ///         daemon has — root, or an elevated administrator / LocalSystem.
    ///         Decides both halves of the rule below: which candidates apply,
    ///         and which have to be vouched for.
    [[nodiscard]] virtual bool IsPrivilegedProcess() const = 0;
};

/// The production probe: the real process environment and the real filesystem.
class SystemConfigPathProbe final: public IConfigPathProbe
{
  public:
    [[nodiscard]] std::optional<std::string> GetEnv(std::string_view name) const override;
    [[nodiscard]] bool IsReadableFile(std::filesystem::path const& path) const override;
    [[nodiscard]] bool IsTrustedSystemLocation(std::filesystem::path const& path) const override;
    [[nodiscard]] bool IsPrivilegedProcess() const override;
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

/// A candidate the lookup passed over for a reason the operator has to hear.
///
/// Only *some* skips qualify. A location that does not exist, or that this
/// account may not read, is ordinary and stays quiet — that silence is the
/// documented rule which lets a per-user daemon start alongside a machine-wide
/// config it has no access to. A machine-wide file rejected as untrusted is the
/// opposite: the file is there, it is readable, and it is being ignored anyway,
/// so saying nothing would be the silent no-op this codebase keeps paying for.
struct RejectedCandidate
{
    std::filesystem::path path; ///< The candidate that was not used.
    std::string reason;         ///< Why, phrased for an operator, with the remedy.
};

/// What a lookup decided, and what it wants said out loud.
struct ConfigLookup
{
    /// The file to read; empty when none applies and the built-in defaults do.
    std::filesystem::path path;

    /// Candidates skipped for a reason worth reporting. Usually empty.
    std::vector<RejectedCandidate> rejected;
};

/// Find the config file to use when no `--config` was given.
///
/// Which candidates apply, and which have to be vouched for, both follow from
/// one question: is this process the machine-wide daemon or somebody's own?
///
/// - **Unprivileged**: only the per-user rows apply, and none is trust-checked.
///   The machine-wide file describes a daemon this process is not — its cache
///   lives where only the service account can write — so adopting it would
///   configure a per-user instance for the wrong job. That is also why the
///   packaged systemd *user* unit passes no `--config` and expects built-in
///   defaults.
/// - **Privileged** (root, elevated administrator, LocalSystem): every row
///   applies, and *every* row must be one only an administrator could have
///   written — the per-user rows included. `$HOME` and `$XDG_CONFIG_HOME` are
///   inputs an unprivileged account often controls, and `sudo -E fastcached`
///   would otherwise take root's configuration from them.
///
/// @param probe Environment, filesystem and privilege source.
/// @return The first usable candidate, with an empty path when there is none,
///         plus anything rejected along the way.
[[nodiscard]] ConfigLookup ResolveDefaultConfigPath(IConfigPathProbe const& probe);

/// The config file a run should actually read.
///
/// Encodes the rule that a path the operator *named* is strict while one the
/// daemon *found* is not: `named` is returned verbatim, whether or not it
/// exists, so a typo fails loudly downstream instead of silently falling back
/// to different settings; a discovered path is returned only when it is
/// readable, and a machine-wide one only when it is also trusted. A named path
/// is never trust-checked — the operator asserted that file, which is their
/// call to make. Lives here rather than at the call site so the rule is
/// testable.
///
/// @param named The `--config` value, empty when the operator gave none.
/// @param probe Environment and filesystem source.
/// @return The path to read — empty when there is none and the built-in
///         defaults apply — plus anything rejected along the way.
[[nodiscard]] ConfigLookup EffectiveConfigPath(std::string_view named, IConfigPathProbe const& probe);

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

/// What a directory SeedConfigFile has to create should end up permitting.
enum class DirectoryPolicy : std::uint8_t
{
    /// Whatever the parent hands down. For a destination that is nobody's
    /// machine-wide configuration — a scratch path under test, say.
    Inherit,

    /// Restricted so only administrators can write, before anything is put in
    /// it. Required for the machine-wide config, because a fresh `%ProgramData%`
    /// subdirectory inherits create-file for every standard account, and a
    /// config sitting in one is a config the startup lookup will refuse.
    AdministratorsOnly,
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
/// @param policy What a directory created on the way should permit. With
///        `AdministratorsOnly` the restriction is applied *before* the config is
///        copied, so nothing ever sits in a loose directory — which also means a
///        caller without administrative rights fails here rather than planting a
///        machine-wide config it would still own.
/// @return What happened, or a ConfigError when the template is missing, the
///         directory cannot be restricted, or the copy fails.
[[nodiscard]] std::expected<SeedOutcome, ConfigError> SeedConfigFile(std::filesystem::path const& templatePath,
                                                                     std::filesystem::path const& destination,
                                                                     DirectoryPolicy policy);

} // namespace FastCache
