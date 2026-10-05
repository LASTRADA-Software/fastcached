// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    Last, ///< Not an outcome: the count `CliOutcomeTable` is sized by.
};

/// What an outcome is judged by once the configuration is assembled.
struct CliOutcomeRow
{
    /// The enumerator this row describes. `Last` until a row says otherwise, so a row nobody
    /// wrote fails `RowsInEnumeratorOrder` rather than claiming to be `Run`.
    CliOutcome outcome { CliOutcome::Last };

    /// Whether the rules a SERVING daemon is held to -- the bind-flag shape, the keyspace-event
    /// grammar and `DaemonStartupRejection` -- refuse this outcome.
    ///
    /// True for the two that serve: a start, and an install, which registers the command line a
    /// start will replay forever and so is refused while somebody is watching. False for the
    /// verbs that act on something else and exit: an uninstall refused over the very typo it was
    /// reached for is recovery blocked, and a `--healthcheck` probe refused because this BUILD
    /// lacks TLS reports a serving daemon unhealthy.
    bool judgedByServingRules { false };

    /// Who reads this outcome's exit (`Platform/ProcessExit.hpp`), which decides what a refusal
    /// decided before it runs -- a command line that did not parse, a configuration file that did
    /// not load, an install the serving rules refuse -- answers.
    ///
    /// A start's `Supervisor` gets the step's own code, 1 or 78. A one-shot command's `Operator`
    /// gets the ending a command stopped at that step has, 1 for an I/O arm and 2 for a verdict:
    /// an operator runs it and reads the answer, and nothing supervises it. `--healthcheck`'s
    /// `Prober` gets 1, because a container runtime reads it and Docker's HEALTHCHECK documents 0
    /// and 1 and reserves 2 -- so its every refusal and its every failure is 1.
    ExitReader reader { ExitReader::Supervisor };
};

/// Every outcome, and what judges it.
inline constexpr EnumTable<CliOutcome, CliOutcomeRow> CliOutcomeTable { {
    { .outcome = CliOutcome::Run, .judgedByServingRules = true, .reader = ExitReader::Supervisor },
    { .outcome = CliOutcome::ShowHelp, .judgedByServingRules = false, .reader = ExitReader::Operator },
    { .outcome = CliOutcome::ShowVersion, .judgedByServingRules = false, .reader = ExitReader::Operator },
    { .outcome = CliOutcome::InstallService, .judgedByServingRules = true, .reader = ExitReader::Operator },
    { .outcome = CliOutcome::UninstallService, .judgedByServingRules = false, .reader = ExitReader::Operator },
    { .outcome = CliOutcome::HealthCheck, .judgedByServingRules = false, .reader = ExitReader::Prober },
    { .outcome = CliOutcome::SeedConfig, .judgedByServingRules = false, .reader = ExitReader::Operator },
    { .outcome = CliOutcome::MigrateStorage, .judgedByServingRules = false, .reader = ExitReader::Operator },
} };
static_assert(RowsInEnumeratorOrder(CliOutcomeTable, &CliOutcomeRow::outcome),
              "CliOutcomeTable must hold one row per CliOutcome, in enumerator order");

/// Whether @p outcome is refused by the rules a serving daemon is held to.
/// @param outcome What the command line asked for.
/// @return `CliOutcomeRow::judgedByServingRules` for it.
[[nodiscard]] constexpr bool JudgedByServingRules(CliOutcome outcome) noexcept
{
    return CliOutcomeTable[static_cast<std::size_t>(outcome)].judgedByServingRules;
}

/// Who reads what @p outcome exits with.
/// @param outcome What the command line asked for; `Last` -- a command line that selected no
///        outcome -- names no command, and is refused as a start is.
/// @return `CliOutcomeRow::reader` for it, `Supervisor` for `Last`.
[[nodiscard]] constexpr ExitReader ExitReaderOf(CliOutcome outcome) noexcept
{
    return outcome == CliOutcome::Last ? ExitReader::Supervisor : CliOutcomeTable[static_cast<std::size_t>(outcome)].reader;
}

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

    /// How `--install-service` registers the job. Outside `Config` for `serviceScope`'s reason:
    /// it describes the registration, takes no part in the YAML merge and is never replayed.
    ServiceStart serviceStart { ServiceStart::Auto };

    /// `--firewall-allow` scopes: the remote addresses the firewall rules `--install-service`
    /// creates admit; empty admits any address. Install-time only, outside `Config` for
    /// `serviceScope`'s reason.
    std::vector<std::string> firewallAllow;

    /// Template `--seed-config` copies to the machine-wide config location.
    ///
    /// Outside Config for the same reason as serviceScope: it describes an
    /// *installation* step, not the running daemon, so it takes no part in the
    /// YAML merge and is never baked into a service command line.
    std::string seedConfigTemplate;

    /// Per-setting "the operator named this" trackers, set by the row's
    /// `explicitBit` whenever an applier ran for it -- from argv, or from a
    /// configuration file through `ApplyFileSettings`. Which RESULT a bit is read
    /// from is what says where it was named: the command-line-only parse answers
    /// "typed on the command line" (what a service registration bakes in), the
    /// assembled configuration answers "named anywhere" (what the environment
    /// fallback and the listener-shape refusal ask). Without them a typed value
    /// equal to the default (`--threads=0`, `--storage-durability=batched`) could
    /// not be told from a setting nobody gave.
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
    bool activeExpiryIntervalExplicit { false };
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
    bool memoryCompressionExplicit { false };
    bool memoryCompressionLevelExplicit { false };
    bool memoryCompressionMinBytesExplicit { false };
};

/// The accepted command-line options, in the order `--help` documents them.
///
/// The single source of truth for the daemon's CLI: `ParseCli` matches these
/// rows and `CliUsage` renders itself from them, so a flag cannot be accepted
/// without being documented, nor documented without being accepted. Adding a
/// flag is adding a row.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<OptionSpec<CliResult> const> CliOptions() noexcept;

/// One setting a configuration file can carry.
struct ConfigFileSetting
{
    std::string_view key {};                  ///< The YAML key, which is what a refusal names.
    Reloadable reloadable { Reloadable::No }; ///< Whether a reload may apply a change to it.
    SameFieldFn<CliResult> same { nullptr };  ///< Whether two configurations agree about it.
};

/// Every setting a configuration file can carry.
///
/// **One accessor rather than a walk every caller re-spells.** The accepted key set
/// is the option table's `yamlKey` column, and the reload check needs it as a list of
/// its own. The FILE needs no such list: `ApplyFileSettings` walks `CliOptions()`
/// itself, so a key is accepted by the same walk that applies it.
///
/// It used to be that column PLUS a second list of keys no flag could express, and
/// the accessor existed so no consumer would walk one and be silently blind to the
/// other — which is
/// [#406](https://github.com/LASTRADA-Software/fastcached/issues/406)'s own failure
/// shape one level up. [#623](https://github.com/LASTRADA-Software/fastcached/issues/623)
/// gave those three keys option rows and deleted the second source, so this now has
/// one input; keeping the accessor is what made that a change to a *source* rather
/// than to every consumer, and it is why the next such key costs nothing here.
/// @return The settings, in table order. Never empty.
[[nodiscard]] std::span<ConfigFileSetting const> ConfigFileSettings() noexcept;

/// Parse `argv[1..argc-1]` into a Config, driven by `CliOptions()`.
/// @param args argv slice excluding the program name itself.
/// @return Parsed CliResult on success; ConfigError on failure.
[[nodiscard]] std::expected<CliResult, ConfigError> ParseCli(std::span<char const* const> args);

/// `ParseCli` into a result the caller holds, which KEEPS what was parsed before a refusal.
///
/// A command line refused halfway still says whether it is a service's: `--daemon` and
/// `--service-name` may well precede the token that failed, and a service refused without its
/// host being told exits before the SCM hears anything -- error 1053. `main` reads those two off
/// what this leaves behind.
/// @param args argv slice excluding the program name itself.
/// @param result Receives every option applied before the first refusal, and all of them otherwise.
/// @return Nothing on success; the first ConfigError on failure.
[[nodiscard]] std::expected<void, ConfigError> ParseCliInto(std::span<char const* const> args, CliResult& result);

/// What a command line that did NOT parse still named, from the whole of it: every option
/// `CliOptions()` can apply, each bad token stepped over (`ApplyRecognisedOptions`).
///
/// A refusal of such a command line reads this rather than what parsed before the bad token, so
/// the verb it exits as and the service host it reports to are the ones the operator typed
/// wherever they typed them: `--no-such-flag --install-service` is an install's refusal. Evidence
/// about a refused command line, never a configuration to run with.
/// @param args argv slice excluding the program name itself.
/// @return Whatever applied.
[[nodiscard]] CliResult RecognisedCli(std::span<char const* const> args);

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
