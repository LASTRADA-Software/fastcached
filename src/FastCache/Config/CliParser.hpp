// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// Outcome categories returned by ParseCli.
enum class CliOutcome : std::uint8_t
{
    Run,              ///< Parsing succeeded; proceed to run the daemon.
    ShowHelp,         ///< --help / -h was seen.
    ShowVersion,      ///< --version / -V was seen.
    InstallService,   ///< --install-service was seen; register a Windows service.
    UninstallService, ///< --uninstall-service was seen; remove the Windows service.
    HealthCheck,      ///< --healthcheck was seen; probe /healthz and exit 0/1.
    SeedConfig,       ///< --seed-config was seen; install the default config file and exit.

    /// --migrate-storage was seen; convert the configured store's on-disk
    /// record layout to the one this build writes, then exit.
    MigrateStorage,
};

struct CliResult
{
    CliOutcome outcome { CliOutcome::Run };
    Config config {};

    /// Which launchd domain `--install-service` / `--uninstall-service` act on.
    ///
    /// Deliberately outside Config and outside the explicit-tracker list below:
    /// it configures the *installation*, not the daemon, so it takes no part in
    /// the YAML merge. Keeping it in Config would additionally make
    /// BuildServiceArgv bake a meaningless `--service-scope` into the job's own
    /// recorded arguments. Ignored on Windows, which has a single SCM domain.
    ServiceScope serviceScope { ServiceScope::User };

    /// Template `--seed-config` copies to the machine-wide config location.
    ///
    /// Outside Config for the same reason as serviceScope: it describes an
    /// *installation* step, not the running daemon, so it takes no part in the
    /// YAML merge and is never baked into a service command line.
    std::string seedConfigTemplate;

    /// Per-flag "user typed this on the CLI" trackers. Without these,
    /// a user-typed value that happens to equal the field's default
    /// (`--threads=0`, `--storage-shards=0`, `--storage-durability=batched`,
    /// `--storage-max-value=1m`, ...) would be indistinguishable from
    /// "flag not given" in the Merge step, so the YAML value would
    /// silently win. Each handler in `ParseCli` sets the matching
    /// bool when the flag appears in argv.
    bool bindAddressExplicit { false };
    bool portExplicit { false };
    bool maxMemoryBytesExplicit { false };
    bool logLevelExplicit { false };
    bool storagePathExplicit { false };
    bool storageDurabilityExplicit { false };
    bool storageMaxValueBytesExplicit { false };
    bool storageMaxDiskBytesExplicit { false };
    bool workerThreadsExplicit { false };
    bool storageShardsExplicit { false };
    bool activeExpiryIntervalMsExplicit { false };
    bool activeExpiryScanBudgetExplicit { false };
    bool listenBacklogExplicit { false };
    bool logTimestampsExplicit { false };
    bool logSourceExplicit { false };
    bool logEverythingExplicit { false };
    bool requirePassExplicit { false };
    bool authUsernameExplicit { false };
    bool metricsEnabledExplicit { false };
    bool metricsBindAddressExplicit { false };
    bool metricsPortExplicit { false };
    bool tlsEnabledExplicit { false };
    bool tlsCertPathExplicit { false };
    bool tlsKeyPathExplicit { false };
    bool notifyKeyspaceEventsExplicit { false };
    bool lruRecencyExplicit { false };
    bool cpuAffinityExplicit { false };
    bool serviceNameExplicit { false };
    bool compressionExplicit { false };
    bool compressionLevelExplicit { false };
    bool compressionMinBytesExplicit { false };
};

/// The accepted command-line options, in the order `--help` documents them.
///
/// The single source of truth for the daemon's CLI: `ParseCli` matches these
/// rows and `CliUsage` renders itself from them, so a flag cannot be accepted
/// without being documented, nor documented without being accepted. Adding a
/// flag is adding a row.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<OptionSpec<CliResult> const> CliOptions() noexcept;

/// A setting a configuration FILE can carry that no command-line flag can.
///
/// **Every entry here is a defect, named rather than hidden.** A setting reachable
/// from a file and not from argv is two mechanisms for one thing — the shape
/// `TableIsWellFormed`'s own comment says the `yamlKey` column exists to remove —
/// and it cannot be registered as a service either, because `--install-service`
/// replays a command line. Closing that is
/// [#623](https://github.com/LASTRADA-Software/fastcached/issues/623), which gives
/// each of these a row and deletes this list.
///
/// Until then they still have to be answered for, because they are live-wired and
/// a reload that accepted one would leave the running daemon disagreeing with the
/// configuration that claims to describe it. That is what `same` is for: it is the
/// *same* comparator type the option table's own column carries, so `ValidateImmutable`
/// walks the two lists with one rule rather than two. There is no reloadability
/// column, because there is nothing to decide — a setting nothing re-applies at
/// runtime is immutable, and a fourth entry that were genuinely reloadable would
/// have to earn a subscriber in `main.cpp` first.
struct YamlOnlySetting
{
    std::string_view key;        ///< The YAML key, which is what a refusal names.
    std::string_view reason;     ///< Why no option row answers for it.
    SameFieldFn<CliResult> same; ///< Whether two configurations agree about this setting.
};

/// Every setting a file can carry that `CliOptions()` cannot express.
///
/// Exposed only so a test can state that this list is not empty. Everything that
/// asks what a FILE can carry asks `ConfigFileSettings()` instead — see there.
/// @return The list; empty once #623 lands.
[[nodiscard]] std::span<YamlOnlySetting const> YamlOnlySettings() noexcept;

/// One setting a configuration file can carry, whatever declares it.
struct ConfigFileSetting
{
    std::string_view key {};                  ///< The YAML key, which is what a refusal names.
    Reloadable reloadable { Reloadable::No }; ///< Whether a reload may apply a change to it.
    SameFieldFn<CliResult> same { nullptr };  ///< Whether two configurations agree about it.
};

/// Every setting a configuration file can carry, from both places that declare one.
///
/// **One accessor rather than two lists every caller remembers to walk.** The
/// accepted key set is the option table's `yamlKey` column PLUS
/// `YamlOnlySettings()`, and three consumers need it — the reader's gate, the
/// reload check, and the test that connects them. A caller that walked only
/// `CliOptions()` would be right about most settings and silently blind to the
/// rest, which is
/// [#406](https://github.com/LASTRADA-Software/fastcached/issues/406)'s own failure
/// shape one level up. So the split is not visible here, and #623 deletes a
/// *source* rather than a walk in every consumer.
/// @return The merged set, option rows first, in table order. Never empty.
[[nodiscard]] std::span<ConfigFileSetting const> ConfigFileSettings() noexcept;

/// Whether a configuration file may carry @p key at its top level.
///
/// `YamlReader` asks this BEFORE its own dispatch, so "the reader accepts a key
/// nothing declares" is impossible by construction rather than scanned for. The
/// other direction, "a declared key the reader does not handle", is asserted by
/// `CliOptions_test`, because only a real parse can answer it.
/// @param key The top-level key read from the file.
/// @return True when some setting answers to it.
[[nodiscard]] bool ConfigFileAcceptsKey(std::string_view key) noexcept;

/// Parse `argv[1..argc-1]` into a Config, driven by `CliOptions()`.
/// @param args argv slice excluding the program name itself.
/// @return Parsed CliResult on success; ConfigError on failure.
[[nodiscard]] std::expected<CliResult, ConfigError> ParseCli(std::span<char const* const> args);

/// Parse and range-check a TCP port (1..65535) from its decimal text. The single
/// source of truth for port parsing, shared by the CLI flag handlers and the
/// `FASTCACHED_METRICS_PORT` environment fallback so both accept exactly the same
/// syntax and range.
/// @param sv Decimal port text (no surrounding whitespace).
/// @return The port on success, or a ConfigError describing the rejection.
[[nodiscard]] std::expected<std::uint16_t, ConfigError> ParsePort(std::string_view sv);

/// Render the multi-line usage/help text with column-aligned option
/// descriptions. Used by main when --help is requested.
/// @param color UsageColor::Colored to emit ANSI SGR escapes for headings and
///              option flags (appropriate only for interactive terminals, see
///              StdoutSupportsColor); UsageColor::Plain for plain text.
/// @return Fully formatted usage text.
[[nodiscard]] std::string CliUsage(UsageColor color = UsageColor::Plain);

} // namespace FastCache
