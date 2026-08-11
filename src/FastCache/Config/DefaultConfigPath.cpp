// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Platform/Environment.hpp>

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
    /// rows rather than one row with a fallback: an unset variable already
    /// skips a row, so "$XDG_CONFIG_HOME, else ~/.config" needs no branch.
#if defined(_WIN32)
    constexpr auto Candidates = std::to_array<ConfigCandidate>({
        { .display = "%APPDATA%\\fastcached\\fastcached.yaml",
          .baseVar = "APPDATA",
          .suffix = "fastcached\\fastcached.yaml",
          .scope = ConfigScope::User },
        { .display = "%ProgramData%\\fastcached\\fastcached.yaml",
          .baseVar = "ProgramData",
          .suffix = "fastcached\\fastcached.yaml",
          .scope = ConfigScope::System },
    });
#else
    constexpr auto Candidates = std::to_array<ConfigCandidate>({
        { .display = "$XDG_CONFIG_HOME/fastcached/fastcached.yaml",
          .baseVar = "XDG_CONFIG_HOME",
          .suffix = "fastcached/fastcached.yaml",
          .scope = ConfigScope::User },
        { .display = "~/.config/fastcached/fastcached.yaml",
          .baseVar = "HOME",
          .suffix = ".config/fastcached/fastcached.yaml",
          .scope = ConfigScope::User },
        { .display = FC_SYSCONF_DIR "/fastcached.yaml",
          .baseVar = "",
          .suffix = FC_SYSCONF_DIR "/fastcached.yaml",
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
    constexpr ConfigCandidate const& SystemRow = *FindOrNull(Candidates, ConfigScope::System, &ConfigCandidate::scope);

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

std::span<ConfigCandidate const> DefaultConfigCandidates() noexcept
{
    return Candidates;
}

std::optional<std::filesystem::path> ExpandConfigCandidate(ConfigCandidate const& candidate, IConfigPathProbe const& probe)
{
    if (candidate.baseVar.empty())
        return std::filesystem::path { candidate.suffix };

    auto const base = probe.GetEnv(candidate.baseVar);

    // An unset base means the location does not apply to this process; an empty
    // one would make the suffix relative to the working directory, which for a
    // service is C:\Windows\System32 or /. Both skip the row.
    if (!base.has_value() || base->empty())
        return std::nullopt;

    return std::filesystem::path { *base } / candidate.suffix;
}

std::optional<std::filesystem::path> ResolveDefaultConfigPath(IConfigPathProbe const& probe)
{
    for (auto const& candidate: Candidates)
        if (auto path = ExpandConfigCandidate(candidate, probe); path && probe.IsReadableFile(*path))
            return path;

    return std::nullopt;
}

std::filesystem::path EffectiveConfigPath(std::string_view named, IConfigPathProbe const& probe)
{
    if (!named.empty())
        return std::filesystem::path { named };

    return ResolveDefaultConfigPath(probe).value_or(std::filesystem::path {});
}

std::expected<std::filesystem::path, ConfigError> SystemConfigPath(IConfigPathProbe const& probe)
{
    // The row itself is a compile-time fact; only its base variable can be
    // missing, so that is the only thing left to report.
    if (auto path = ExpandConfigCandidate(SystemRow, probe))
        return *std::move(path);

    return std::unexpected(MakeConfigPathError(ConfigErrorCode::UndefinedVariable,
                                               SystemRow.display,
                                               std::format("environment variable is not set: {}", SystemRow.baseVar)));
}

std::expected<SeedOutcome, ConfigError> SeedConfigFile(std::filesystem::path const& templatePath,
                                                       std::filesystem::path const& destination)
{
    // The error_code overloads report failure by returning false, so a probe
    // that cannot answer is treated as "not there" — which is the safe reading
    // for the destination and is caught by copy_options::skip_existing below.
    std::error_code ec;
    if (std::filesystem::exists(destination, ec))
        return SeedOutcome::AlreadyPresent;

    if (!std::filesystem::is_regular_file(templatePath, ec))
        return std::unexpected(MakeConfigPathError(ConfigErrorCode::FileNotFound, templatePath, "no such config template"));

    if (auto const parent = destination.parent_path(); !parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
            return std::unexpected(MakeConfigPathError(
                ConfigErrorCode::WriteFailed, parent, std::format("cannot create directory: {}", ec.message())));
    }

    // copy_file rather than a rename: the template stays in place as package
    // payload, to be replaced wholesale by the next upgrade.
    std::filesystem::copy_file(templatePath, destination, std::filesystem::copy_options::skip_existing, ec);
    if (ec)
        return std::unexpected(MakeConfigPathError(
            ConfigErrorCode::WriteFailed, destination, std::format("cannot write config: {}", ec.message())));

    return SeedOutcome::Written;
}

} // namespace FastCache
