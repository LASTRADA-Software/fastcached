// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <charconv>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache
{

namespace
{

    [[nodiscard]] ConfigError MakeError(ConfigErrorCode code, std::string field, std::string context)
    {
        return ConfigError {
            .code = code, .source = "argv", .line = 0, .field = std::move(field), .context = std::move(context)
        };
    }

    [[nodiscard]] std::expected<std::uint16_t, ConfigError> ParsePort(std::string_view sv)
    {
        std::uint32_t raw = 0;
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), raw);
        if (ec != std::errc {} || ptr != sv.data() + sv.size())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, "port", std::format("not a number: {}", sv)));
        if (raw == 0 || raw > 65535)
            return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, "port", std::format("out of range: {}", raw)));
        return static_cast<std::uint16_t>(raw);
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseMaxMemory(std::string_view sv)
    {
        return ParseByteSize(sv, "max-memory", QueryHostTotalMemoryBytes()).transform_error([](ConfigError err) {
            err.source = "argv";
            return err;
        });
    }

    [[nodiscard]] std::expected<LogLevel, ConfigError> ParseLogLevel(std::string_view sv)
    {
        if (sv == "trace")
            return LogLevel::Trace;
        if (sv == "debug")
            return LogLevel::Debug;
        if (sv == "info")
            return LogLevel::Info;
        if (sv == "warn")
            return LogLevel::Warn;
        if (sv == "error")
            return LogLevel::Error;
        if (sv == "fatal")
            return LogLevel::Fatal;
        return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, "log-level", std::format("unknown level: {}", sv)));
    }

    [[nodiscard]] bool FlagMatches(std::string_view arg, std::string_view name) noexcept
    {
        if (arg == name)
            return true;
        return arg.starts_with(std::string { name } + "=");
    }

    /// Pull the value out of `args[i]` for a `--flag=value` or `--flag value`
    /// shape, advancing i past the value if it's a separate argv element.
    [[nodiscard]] std::expected<std::string_view, ConfigError> TakeValue(std::span<char const* const> args,
                                                                         std::size_t& i,
                                                                         std::string_view flag)
    {
        auto const arg = std::string_view { args[i] };
        if (auto const eq = arg.find('='); eq != std::string_view::npos)
            return arg.substr(eq + 1);
        if (i + 1 >= args.size())
            return std::unexpected(MakeError(ConfigErrorCode::ParseError, std::string { flag }, "missing value"));
        ++i;
        return std::string_view { args[i] };
    }

    /// Per-arg dispatch outcome.
    enum class ArgOutcome : std::uint8_t
    {
        Continue,    ///< Argument handled; loop continues.
        ShowHelp,    ///< Caller should return CliOutcome::ShowHelp.
        ShowVersion, ///< Caller should return CliOutcome::ShowVersion.
        Unknown,     ///< Argument not recognised by any handler.
    };

    /// Apply a string-valued flag to a Config field if it matches. Returns
    /// std::nullopt when the flag does not match (so the dispatcher can try
    /// the next handler).
    [[nodiscard]] std::expected<bool, ConfigError> ApplyStringFlag(std::span<char const* const> args,
                                                                   std::size_t& i,
                                                                   std::string_view flagName,
                                                                   std::string& target)
    {
        if (!FlagMatches(std::string_view { args[i] }, flagName))
            return false;
        auto const value = TakeValue(args, i, flagName);
        if (!value.has_value())
            return std::unexpected(value.error());
        target = std::string { *value };
        return true;
    }

    /// Templated equivalent for typed (parsed) flags: --port, --max-memory,
    /// --log-level.
    template <typename Parser, typename Target>
    [[nodiscard]] std::expected<bool, ConfigError> ApplyParsedFlag(
        std::span<char const* const> args, std::size_t& i, std::string_view flagName, Parser parser, Target& target)
    {
        if (!FlagMatches(std::string_view { args[i] }, flagName))
            return false;
        auto const value = TakeValue(args, i, flagName);
        if (!value.has_value())
            return std::unexpected(value.error());
        auto const parsed = parser(*value);
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        target = *parsed;
        return true;
    }

    /// Dispatch a single argument. Returns ArgOutcome on success or a
    /// ConfigError if the argument matched a flag but parsing failed.
    [[nodiscard]] std::expected<ArgOutcome, ConfigError> HandleOneArg(std::span<char const* const> args,
                                                                      std::size_t& i,
                                                                      Config& cfg)
    {
        std::string_view const arg { args[i] };
        if (arg == "--help" || arg == "-h")
            return ArgOutcome::ShowHelp;
        if (arg == "--version" || arg == "-V")
            return ArgOutcome::ShowVersion;
        if (arg == "--daemon")
        {
            cfg.daemon = true;
            return ArgOutcome::Continue;
        }

        // String-valued flags.
        for (auto const& [name, target]: std::initializer_list<std::pair<std::string_view, std::string*>> {
                 { "--config", &cfg.configPath },
                 { "--bind", &cfg.bindAddress },
                 { "--pidfile", &cfg.pidfile },
                 { "--service-name", &cfg.serviceName },
             })
        {
            auto const matched = ApplyStringFlag(args, i, name, *target);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }

        // Typed flags.
        {
            auto const matched = ApplyParsedFlag(args, i, "--port", ParsePort, cfg.port);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--max-memory", ParseMaxMemory, cfg.maxMemoryBytes);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--log-level", ParseLogLevel, cfg.logLevel);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        return ArgOutcome::Unknown;
    }

} // namespace

std::string_view CliUsage() noexcept
{
    return "usage: fastcached [options]\n"
           "  --config=<path>        YAML config file; CLI flags override file values\n"
           "  --bind=<addr>          bind address (default 127.0.0.1)\n"
           "  --port=<num>           TCP port (default 11211)\n"
           "  --max-memory=<size>    in-memory budget; k/m/g = KiB/MiB/GiB or N% of host RAM (default 64 MiB)\n"
           "  --log-level=<level>    trace|debug|info|warn|error|fatal (default info)\n"
           "  --daemon               daemonize (POSIX) / register as Windows service\n"
           "  --pidfile=<path>       POSIX daemon mode only\n"
           "  --service-name=<name>  Windows service name (default FastCached)\n"
           "  --help, -h             show this help and exit\n"
           "  --version, -V          show version and exit\n";
}

std::expected<CliResult, ConfigError> ParseCli(std::span<char const* const> args)
{
    CliResult outcome;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        auto const result = HandleOneArg(args, i, outcome.config);
        if (!result.has_value())
            return std::unexpected(result.error());
        switch (*result)
        {
            case ArgOutcome::Continue:
                continue;
            case ArgOutcome::ShowHelp:
                outcome.outcome = CliOutcome::ShowHelp;
                return outcome;
            case ArgOutcome::ShowVersion:
                outcome.outcome = CliOutcome::ShowVersion;
                return outcome;
            case ArgOutcome::Unknown:
                return std::unexpected(
                    MakeError(ConfigErrorCode::UnknownKey, std::string { args[i] }, "unrecognised argument"));
        }
    }
    return outcome;
}

} // namespace FastCache
