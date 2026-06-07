// SPDX-License-Identifier: Apache-2.0
#pragma once

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
};

struct CliResult
{
    CliOutcome outcome { CliOutcome::Run };
    Config config {};

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
    bool workerThreadsExplicit { false };
    bool storageShardsExplicit { false };
    bool listenBacklogExplicit { false };
    bool logTimestampsExplicit { false };
    bool requirePassExplicit { false };
    bool authUsernameExplicit { false };
    bool metricsEnabledExplicit { false };
    bool metricsBindAddressExplicit { false };
    bool metricsPortExplicit { false };
    bool tlsEnabledExplicit { false };
    bool tlsCertPathExplicit { false };
    bool tlsKeyPathExplicit { false };
};

/// Parse `argv[1..argc-1]` into a Config. Returns ConfigError on bad input.
/// Recognised flags:
///   --bind <addr>            (default 127.0.0.1)
///   --port <num>             (default 11211)
///   --max-memory <size>      (default 64 MiB; integer with optional unit suffix
///                             k/K=1024, m/M=1024², g/G=1024³; plain int = bytes;
///                             trailing % = percentage of host total RAM, e.g. 50%)
///   --log-level <level>      (trace|debug|info|warn|error|fatal; default info)
///   --help, -h               print usage and exit
///   --version, -V            print version and exit
///
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

/// Whether CliUsage should colorize its output.
enum class UsageColor : std::uint8_t
{
    Plain,   ///< Plain text — for files, pipes, NO_COLOR, and tests.
    Colored, ///< ANSI SGR color escapes — for interactive terminals.
};

/// Render the multi-line usage/help text with column-aligned option
/// descriptions. Used by main when --help is requested.
/// @param color UsageColor::Colored to emit ANSI SGR escapes for headings and
///              option flags (appropriate only for interactive terminals, see
///              StdoutSupportsColor); UsageColor::Plain for plain text.
/// @return Fully formatted usage text.
[[nodiscard]] std::string CliUsage(UsageColor color = UsageColor::Plain);

} // namespace FastCache
