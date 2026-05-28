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

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseStorageMaxValue(std::string_view sv)
    {
        return ParseByteSize(sv, "storage-max-value").transform_error([](ConfigError err) {
            err.source = "argv";
            return err;
        });
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParsePositiveInt(std::string_view sv, std::string_view field)
    {
        if (sv.empty())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, std::string { field }, "empty value"));
        std::size_t value = 0;
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        if (ec != std::errc {} || ptr != sv.data() + sv.size())
            return std::unexpected(
                MakeError(ConfigErrorCode::TypeMismatch, std::string { field }, std::format("not a number: {}", sv)));
        return value;
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseThreads(std::string_view sv)
    {
        return ParsePositiveInt(sv, "threads");
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseStorageShards(std::string_view sv)
    {
        return ParsePositiveInt(sv, "storage-shards");
    }

    [[nodiscard]] std::expected<ThreadingModel, ConfigError> ParseThreadingModel(std::string_view sv)
    {
        if (sv == "threaded")
            return ThreadingModel::Threaded;
        if (sv == "reactor")
            return ThreadingModel::Reactor;
        return std::unexpected(MakeError(
            ConfigErrorCode::OutOfRange, "threading-model", std::format("unknown model (expect threaded|reactor): {}", sv)));
    }

    [[nodiscard]] std::expected<StorageDurability, ConfigError> ParseStorageDurability(std::string_view sv)
    {
        if (sv == "fsync")
            return StorageDurability::Fsync;
        if (sv == "batched")
            return StorageDurability::Batched;
        if (sv == "none")
            return StorageDurability::None;
        return std::unexpected(
            MakeError(ConfigErrorCode::OutOfRange, "storage-durability", std::format("unknown durability mode: {}", sv)));
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
                 { "--storage", &cfg.storagePath },
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
        {
            auto const matched =
                ApplyParsedFlag(args, i, "--storage-durability", ParseStorageDurability, cfg.storageDurability);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        {
            auto const matched =
                ApplyParsedFlag(args, i, "--storage-max-value", ParseStorageMaxValue, cfg.storageMaxValueBytes);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        {
            auto const matched =
                ApplyParsedFlag(args, i, "--threading-model", ParseThreadingModel, cfg.threadingModel);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--threads", ParseThreads, cfg.workerThreads);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
                return ArgOutcome::Continue;
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--storage-shards", ParseStorageShards, cfg.storageShards);
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
           "  --storage=<path>       persist cache to a CoW-tree file (default: in-memory only)\n"
           "  --storage-durability=<mode>  fsync|batched|none for --storage (default batched)\n"
           "  --storage-max-value=<size>   per-value byte cap for --storage; k/m/g suffixes accepted (default 1m)\n"
           "  --threading-model=<mode>     threaded|reactor (default threaded)\n"
           "  --threads=<N>                worker thread count for threaded mode (default: hardware_concurrency)\n"
           "  --storage-shards=<N>         shard storage into N partitions for write parallelism\n"
           "                                   when N>1 and --storage is set, --storage must be a directory\n"
           "  --daemon               daemonize (POSIX) / register as Windows service\n"
           "  --pidfile=<path>       POSIX daemon mode only\n"
           "  --service-name=<name>  Windows service name (default FastCached)\n"
           "  --help, -h             show this help and exit\n"
           "  --version, -V          show version and exit\n"
           "\n"
#if defined(_WIN32)
           "Use with sccache (memcached protocol, PowerShell):\n"
           "  Start-Process fastcached -ArgumentList '--port=11211'\n"
           "  $env:SCCACHE_MEMCACHED = 'tcp://127.0.0.1:11211'\n"
           "  sccache <compiler> /c hello.cpp /Fo:hello.obj\n"
           "\n"
           "Use with sccache (Redis protocol, PowerShell):\n"
           "  $env:SCCACHE_REDIS = 'redis://127.0.0.1:11211'\n"
#else
           "Use with sccache (memcached protocol):\n"
           "  fastcached --port=11211 &\n"
           "  export SCCACHE_MEMCACHED=tcp://127.0.0.1:11211\n"
           "  sccache <compiler> -c hello.c -o hello.o\n"
           "\n"
           "Use with sccache (Redis protocol):\n"
           "  export SCCACHE_REDIS=redis://127.0.0.1:11211\n"
#endif
           "\n"
           "sccache <= 0.7 speaks memcached text; >= 0.8 speaks memcached binary;\n"
           "either works because fastcached auto-detects the wire format.\n";
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
