// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace FastCache
{

namespace
{

    // The machine-wide config directory, handed down from CMake so the binary
    // and the packaging cannot disagree about it. Deliberately no fallback
    // literal: a second spelling of the path is exactly what this module exists
    // to prevent, and src/FastCache/CMakeLists.txt defines it for every
    // non-Windows build. Windows needs none — its machine-wide location is
    // resolved from %ProgramData% at runtime.
#if !defined(_WIN32) && !defined(FC_SYSCONF_DIR)
    #error "FC_SYSCONF_DIR must be defined by the build (see src/FastCache/CMakeLists.txt)"
#endif

    /// The candidate table, in priority order. See ConfigCandidate.
    ///
    /// The user rows come first so an operator can shadow the machine-wide file
    /// without touching it (and without root). On POSIX the XDG pair is two
    /// rows rather than one row with a fallback, which needs no branch: an
    /// unset variable already skips its row.
    ///
    /// Note that two rows is "$XDG_CONFIG_HOME, **then** ~/.config" — the
    /// fallback is per *file*, not per *variable*. The basedir specification
    /// says the second location is what `$XDG_CONFIG_HOME` means when unset,
    /// and so drops out of the search entirely when it is set; here, setting it
    /// and leaving no file there still falls through to ~/.config. Probing both
    /// is what most tools do and is the friendlier reading of a half-migrated
    /// setup, but it is a deviation, and one an operator who moved their config
    /// deliberately can see: the startup banner names the file that was chosen.
#if defined(_WIN32)
    constexpr auto Candidates = std::to_array<ConfigCandidate>({
        { .display = "%APPDATA%\\{app}\\{app}.yaml",
          .baseVar = "APPDATA",
          .suffix = "{app}\\{app}.yaml",
          .scope = ConfigScope::User },
        { .display = "%ProgramData%\\{app}\\{app}.yaml",
          .baseVar = "ProgramData",
          .suffix = "{app}\\{app}.yaml",
          .scope = ConfigScope::System },
    });
#else
    constexpr auto Candidates = std::to_array<ConfigCandidate>({
        { .display = "$XDG_CONFIG_HOME/{app}/{app}.yaml",
          .baseVar = "XDG_CONFIG_HOME",
          .suffix = "{app}/{app}.yaml",
          .scope = ConfigScope::User },
        { .display = "~/.config/{app}/{app}.yaml",
          .baseVar = "HOME",
          .suffix = ".config/{app}/{app}.yaml",
          .scope = ConfigScope::User },
        { .display = FC_SYSCONF_DIR "/{app}.yaml",
          .baseVar = "",
          .suffix = FC_SYSCONF_DIR "/{app}.yaml",
          .scope = ConfigScope::System },
    });
#endif

    // Table invariants the lookup relies on. Asserted here rather than in a test
    // because they are properties of a constexpr array: a violation should stop
    // the build, not wait for `ctest`.
    static_assert(std::ranges::is_sorted(Candidates, {}, &ConfigCandidate::scope),
                  "user locations must precede system ones: a per-account file shadows the machine-wide one");
    static_assert(std::ranges::contains(Candidates, ConfigScope::System, &ConfigCandidate::scope),
                  "SystemConfigPath needs a machine-wide row to write to");
    static_assert(std::ranges::none_of(Candidates,
                                       [](ConfigCandidate const& c) { return c.display.empty() || c.suffix.empty(); }),
                  "every row needs a display form for --help and a path to probe");

    /// The machine-wide row — where an installer writes and where a system
    /// service looks. Resolved at compile time, so the assertion above is what
    /// guarantees it exists rather than a runtime end() check at each use.
    ///
    /// FindOrNull rather than `std::ranges::find` because Candidates is a
    /// `std::array`, whose iterator has no portable spelling; see Ranges.hpp.
    ///
    /// A copy rather than a `constexpr` reference: the row is four `string_view`s
    /// and an enum, so copying it costs nothing at compile time, and
    /// readability-identifier-naming classifies a reference as a *variable*
    /// whatever its constness — which would demand a name that says this is
    /// mutable state when it is a constant.
    constexpr ConfigCandidate SystemRow = *FindOrNull(Candidates, ConfigScope::System, &ConfigCandidate::scope);

    /// Build a ConfigError for a failure in this module.
    /// @param code Error category.
    /// @param source Path the error is about.
    /// @param context Free-form explanation.
    /// @return The populated error.
    [[nodiscard]] ConfigError MakeConfigPathError(ConfigErrorCode code,
                                                  std::filesystem::path const& source,
                                                  std::string context)
    {
        // `field` names the flag that surfaces the error, matching the
        // convention elsewhere (ServiceControl's "service-scope", the keyspace
        // notifier's "notify-keyspace-events"); `source` already has the path.
        return ConfigError {
            .code = code, .source = source.string(), .line = 0, .field = "seed-config", .context = std::move(context)
        };
    }

} // namespace

std::optional<std::string> SystemConfigPathProbe::GetEnv(std::string_view name) const
{
    return ReadEnvironmentVariable(name);
}

bool SystemConfigPathProbe::IsReadableFile(std::filesystem::path const& path) const
{
    // is_regular_file first: fopen() on a directory succeeds on Linux, so
    // opening alone would accept one as a config file.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return false;

    // Opening is the only portable way to learn whether this process may
    // actually read the file; see the header for why that matters.
    std::ifstream probe { path, std::ios::binary };
    return probe.is_open();
}

bool SystemConfigPathProbe::IsTrustedSystemLocation(std::filesystem::path const& path) const
{
    return IsAdministratorOnlyWritable(path);
}

bool SystemConfigPathProbe::IsPrivilegedProcess() const
{
    return FastCache::IsPrivilegedProcess();
}

std::span<ConfigCandidate const> DefaultConfigCandidates() noexcept
{
    return Candidates;
}

std::string ExpandApplicationName(std::string_view pattern, std::string_view appName)
{
    std::string out;
    out.reserve(pattern.size() + appName.size());

    for (std::size_t at = 0; at < pattern.size();)
    {
        auto const found = pattern.find(ApplicationNameToken, at);
        if (found == std::string_view::npos)
        {
            out.append(pattern.substr(at));
            break;
        }
        out.append(pattern.substr(at, found - at));
        out.append(appName);
        at = found + ApplicationNameToken.size();
    }

    return out;
}

std::optional<std::filesystem::path> ExpandConfigCandidate(ConfigCandidate const& candidate,
                                                           IConfigPathProbe const& probe,
                                                           std::string_view appName)
{
    auto const suffix = ExpandApplicationName(candidate.suffix, appName);

    if (candidate.baseVar.empty())
        return std::filesystem::path { suffix };

    auto const base = probe.GetEnv(candidate.baseVar);

    // An unset base means the location does not apply to this process; an empty
    // one would make the suffix relative to the working directory, which for a
    // service is C:\Windows\System32 or /. Both skip the row.
    if (!base.has_value() || base->empty())
        return std::nullopt;

    return std::filesystem::path { *base } / suffix;
}

ConfigLookup ResolveDefaultConfigPath(IConfigPathProbe const& probe, std::string_view appName)
{
    ConfigLookup lookup;

    // Both halves of the rule turn on this one question; see the header.
    auto const privileged = probe.IsPrivilegedProcess();

    for (auto const& candidate: Candidates)
    {
        // The machine-wide file describes the machine-wide daemon, whose cache
        // only the service account can write. A per-user instance that adopted
        // it would be configured for a job it cannot do — pointed at a state
        // directory it has no access to, or at the port the real daemon holds.
        if (candidate.scope == ConfigScope::System && !privileged)
            continue;

        auto const path = ExpandConfigCandidate(candidate, probe, appName);
        if (!path.has_value() || !probe.IsReadableFile(*path))
            continue; // absent or not ours to read: ordinary, and silent

        // Anything a privileged process obeys has to be something only an
        // administrator could have written — the per-user rows included, since
        // $HOME and $XDG_CONFIG_HOME are inputs an unprivileged account often
        // controls. An unprivileged process is only ever offered its own files,
        // so there is nothing there to vouch for.
        if (privileged && !probe.IsTrustedSystemLocation(*path))
        {
            auto const directory = path->parent_path();
            lookup.rejected.push_back(
                { .path = *path,
                  .reason = std::format("ignored: {} can be written by accounts other than the administrative ones, "
                                        "so this file is not necessarily an administrator's. Secure it with `{}`, or "
                                        "name a config explicitly with --config.",
                                        directory.string(),
                                        SecureDirectoryHint(directory)) });
            continue;
        }

        lookup.path = *path;
        break;
    }

    return lookup;
}

ConfigLookup EffectiveConfigPath(std::string_view named, IConfigPathProbe const& probe, std::string_view appName)
{
    if (!named.empty())
        return { .path = std::filesystem::path { named }, .rejected = {} };

    return ResolveDefaultConfigPath(probe, appName);
}

std::expected<std::filesystem::path, ConfigError> SystemConfigPath(IConfigPathProbe const& probe, std::string_view appName)
{
    // The row itself is a compile-time fact; only its base variable can be
    // missing, so that is the only thing left to report.
    if (auto path = ExpandConfigCandidate(SystemRow, probe, appName))
        return *std::move(path);

    return std::unexpected(MakeConfigPathError(ConfigErrorCode::UndefinedVariable,
                                               ExpandApplicationName(SystemRow.display, appName),
                                               std::format("environment variable is not set: {}", SystemRow.baseVar)));
}

std::expected<SeedOutcome, ConfigError> SeedConfigFile(std::filesystem::path const& templatePath,
                                                       std::filesystem::path const& destination,
                                                       DirectoryPolicy policy)
{
    std::error_code ec;

    // The directory comes first, BEFORE the seed-once check — not only before
    // the copy. A destination that is already there is exactly the case the
    // repair exists for: any standard account can create a %ProgramData%
    // subdirectory and drop a config into it long before the installer runs,
    // and returning AlreadyPresent at that point would leave the squatter
    // owning the directory of a LocalSystem service's configuration forever.
    // The MSI cannot close it either — its PermissionEx replaces the access
    // list but not the owner.
    auto const parent = destination.parent_path();

    // Two questions, two names, because the header says they are separate and a
    // single flag would make that untrue. The DIRECTORY clause carries
    // `!parent.empty()` since there is nothing to create or restrict without a
    // parent; the FILE's secrecy does not depend on the destination having one,
    // and gating it on a directory-shaped condition would silently leave a
    // bare-named config unprotected under a policy that asked for protection.
    auto const secureDirectory = policy == DirectoryPolicy::AdministratorsOnly && !parent.empty();
    auto const secureFile = policy == DirectoryPolicy::AdministratorsOnly;

    // Whether the location could already have been written by somebody else.
    // Asked before the repair, because afterwards the answer is always no — and
    // what it decides is the standing of a file found sitting there.
    auto const wasUnattributable =
        secureDirectory && std::filesystem::exists(parent, ec) && !IsAdministratorOnlyWritable(parent);

    if (!parent.empty())
    {
        auto const created = std::filesystem::create_directories(parent, ec);
        if (ec)
            return std::unexpected(MakeConfigPathError(
                ConfigErrorCode::WriteFailed, parent, std::format("cannot create directory: {}", ec.message())));

        // SecureDirectoryForAdministrators reports the resulting property
        // rather than the syscall, so this both sets and confirms it.
        if (secureDirectory && !SecureDirectoryForAdministrators(parent))
        {
            // Leave nothing squattable behind. A directory this call created and
            // could not secure is one owned by whoever ran it, sitting at the
            // machine-wide config location — the exact shape being defended
            // against, authored by the defence itself.
            if (created)
                std::filesystem::remove(parent, ec);

            return std::unexpected(
                MakeConfigPathError(ConfigErrorCode::WriteFailed,
                                    parent,
                                    std::format("cannot restrict this directory to administrators, so a config "
                                                "placed in it would not be trusted at startup. Seeding the "
                                                "machine-wide config needs administrative rights; to repair a "
                                                "directory that is already there, run: {}",
                                                SecureDirectoryHint(parent))));
        }
    }

    // The error_code overloads report failure by returning false, so a probe
    // that cannot answer is treated as "not there" — which is the safe reading
    // for the destination and is caught by copy_options::skip_existing below.
    if (std::filesystem::exists(destination, ec))
    {
        // Seed-once keeps an operator's edits, but a file that was sitting in a
        // directory anybody could write is not established to be an operator's.
        // Securing the directory has stopped it from changing further; whether
        // to keep what is in it is a judgement only a human can make, so say so
        // rather than bless it by silence or destroy it by overwriting.
        if (wasUnattributable)
            return std::unexpected(
                MakeConfigPathError(ConfigErrorCode::WriteFailed,
                                    destination,
                                    std::format("a config was already here, in a directory that until now could be "
                                                "written by accounts other than the administrative ones, so it "
                                                "cannot be assumed to be an administrator's. {} has been secured; "
                                                "review this file and delete it to seed a fresh one.",
                                                parent.string())));

        // Seed-once keeps the file's CONTENT; it says nothing about who may read
        // it. A config seeded by an older installer inherited the directory's read
        // for every local account, and that is the file `requirepass:` is told to
        // live in (#741) -- so an upgrade repairs it. Only an established broad
        // exposure: a narrower grant is an administrator's own delegation, and an
        // undetermined answer is not an exposure anybody has shown.
        if (secureFile && SecretFileExposure(destination) == SecretExposure::AnyLocalAccount)
        {
            if (!SecureSecretFileForServices(destination))
                return std::unexpected(
                    MakeConfigPathError(ConfigErrorCode::WriteFailed,
                                        destination,
                                        std::format("the permissions of this config could not be tightened. {}",
                                                    SecretExposureHint(destination, SecretExposure::AnyLocalAccount))));

            return SeedOutcome::AlreadyPresentRestricted;
        }

        return SeedOutcome::AlreadyPresent;
    }

    if (!std::filesystem::is_regular_file(templatePath, ec))
        return std::unexpected(MakeConfigPathError(ConfigErrorCode::FileNotFound, templatePath, "no such config template"));

    // copy_file rather than a rename: the template stays in place as package
    // payload, to be replaced wholesale by the next upgrade.
    std::filesystem::copy_file(templatePath, destination, std::filesystem::copy_options::skip_existing, ec);
    if (ec)
        return std::unexpected(MakeConfigPathError(
            ConfigErrorCode::WriteFailed, destination, std::format("cannot write config: {}", ec.message())));

    // The file's own access list, and it has to be its own: `copy_file` carries the
    // template's permissions across, and on Windows the destination additionally
    // inherits the directory's read for BUILTIN\Users -- which is the grant that
    // made the file `requirepass:` is told to live in readable by every local
    // account (#741).
    if (secureFile && !SecureSecretFileForServices(destination))
    {
        // Take back what could not be secured. The same rule the directory branch
        // above follows, for the same reason: a config sitting at the machine-wide
        // location with permissions this call could not establish is the state the
        // action exists to prevent, authored by the action. Nothing is lost -- the
        // template is still on disk and the seed is re-runnable -- and a daemon
        // that finds no machine-wide config falls back to built-in defaults, which
        // is exactly what the Windows installer already degrades to when the seed
        // does not happen.
        std::filesystem::remove(destination, ec);

        // No `icacls`/`chmod` line here, unlike every other refusal in this
        // function: the remedy those print names a path, and this one has just
        // been removed. What is left to say is what happened and what it needs.
        return std::unexpected(MakeConfigPathError(ConfigErrorCode::WriteFailed,
                                                   destination,
                                                   "the config was written and its permissions could not be "
                                                   "tightened, so it has been removed rather than left readable by "
                                                   "every account on this machine. Seeding the machine-wide config "
                                                   "needs administrative rights; re-run it with them."));
    }

    return SeedOutcome::Written;
}

} // namespace FastCache
