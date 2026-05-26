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
    Run,         ///< Parsing succeeded; proceed to run the daemon.
    ShowHelp,    ///< --help / -h was seen.
    ShowVersion, ///< --version / -V was seen.
};

struct CliResult
{
    CliOutcome outcome { CliOutcome::Run };
    Config config {};
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

/// @return Usage text (multi-line). Used by main when --help is requested.
[[nodiscard]] std::string_view CliUsage() noexcept;

} // namespace FastCache
