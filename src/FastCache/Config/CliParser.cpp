// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>

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
        return ConfigError { .code = code, .source = "argv", .line = 0, .field = std::move(field), .context = std::move(context) };
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
        std::uint64_t raw = 0;
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), raw);
        if (ec != std::errc {} || ptr != sv.data() + sv.size())
            return std::unexpected(
                MakeError(ConfigErrorCode::TypeMismatch, "max-memory", std::format("not a number: {}", sv)));
        return static_cast<std::size_t>(raw);
    }

    [[nodiscard]] std::expected<LogLevel, ConfigError> ParseLogLevel(std::string_view sv)
    {
        if (sv == "trace") return LogLevel::Trace;
        if (sv == "debug") return LogLevel::Debug;
        if (sv == "info") return LogLevel::Info;
        if (sv == "warn") return LogLevel::Warn;
        if (sv == "error") return LogLevel::Error;
        if (sv == "fatal") return LogLevel::Fatal;
        return std::unexpected(
            MakeError(ConfigErrorCode::OutOfRange, "log-level", std::format("unknown level: {}", sv)));
    }

} // namespace

std::string_view CliUsage() noexcept
{
    return "usage: fastcached [options]\n"
           "  --bind=<addr>          bind address (default 127.0.0.1)\n"
           "  --port=<num>           TCP port (default 11211)\n"
           "  --max-memory=<bytes>   in-memory storage byte budget (default 64 MiB)\n"
           "  --log-level=<level>    trace|debug|info|warn|error|fatal (default info)\n"
           "  --help, -h             show this help and exit\n"
           "  --version, -V          show version and exit\n";
}

std::expected<CliResult, ConfigError> ParseCli(std::span<char const* const> args)
{
    CliResult outcome;
    Config& cfg = outcome.config;

    auto takeValue =
        [&](std::string_view flag, std::size_t& index) -> std::expected<std::string_view, ConfigError> {
        // Supports both `--flag=value` and `--flag value` shapes.
        auto const arg = std::string_view { args[index] };
        if (auto const eq = arg.find('='); eq != std::string_view::npos)
            return arg.substr(eq + 1);
        if (index + 1 >= args.size())
            return std::unexpected(MakeError(
                ConfigErrorCode::ParseError, std::string { flag }, "missing value"));
        ++index;
        return std::string_view { args[index] };
    };

    auto matchesFlag = [](std::string_view arg, std::string_view name) noexcept {
        if (arg == name)
            return true;
        return arg.starts_with(std::string { name } + "=");
    };

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        std::string_view const arg { args[i] };
        if (arg == "--help" || arg == "-h")
        {
            outcome.outcome = CliOutcome::ShowHelp;
            return outcome;
        }
        if (arg == "--version" || arg == "-V")
        {
            outcome.outcome = CliOutcome::ShowVersion;
            return outcome;
        }
        if (matchesFlag(arg, "--bind"))
        {
            auto const value = takeValue("--bind", i);
            if (!value.has_value())
                return std::unexpected(value.error());
            cfg.bindAddress = std::string { *value };
            continue;
        }
        if (matchesFlag(arg, "--port"))
        {
            auto const value = takeValue("--port", i);
            if (!value.has_value())
                return std::unexpected(value.error());
            auto const port = ParsePort(*value);
            if (!port.has_value())
                return std::unexpected(port.error());
            cfg.port = *port;
            continue;
        }
        if (matchesFlag(arg, "--max-memory"))
        {
            auto const value = takeValue("--max-memory", i);
            if (!value.has_value())
                return std::unexpected(value.error());
            auto const mem = ParseMaxMemory(*value);
            if (!mem.has_value())
                return std::unexpected(mem.error());
            cfg.maxMemoryBytes = *mem;
            continue;
        }
        if (matchesFlag(arg, "--log-level"))
        {
            auto const value = takeValue("--log-level", i);
            if (!value.has_value())
                return std::unexpected(value.error());
            auto const level = ParseLogLevel(*value);
            if (!level.has_value())
                return std::unexpected(level.error());
            cfg.logLevel = *level;
            continue;
        }
        return std::unexpected(
            MakeError(ConfigErrorCode::UnknownKey, std::string { arg }, "unrecognised argument"));
    }
    return outcome;
}

} // namespace FastCache
