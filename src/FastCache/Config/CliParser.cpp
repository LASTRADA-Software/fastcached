// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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

    /// Re-stamp the `source` of an error produced by a lower layer as "argv".
    [[nodiscard]] ConfigError WithArgvSource(ConfigError err)
    {
        err.source = "argv";
        return err;
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

    /// Validate a bind address syntactically only. The authoritative check —
    /// "does this resolve to a bindable IPv4/IPv6 address?" — happens at bind
    /// time via getaddrinfo, so a hostname like "localhost" is accepted here
    /// and either resolves at bind or surfaces a clean fatal bind error. We
    /// only reject input that can never be a host: empty, or containing
    /// whitespace / control characters.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseBindAddress(std::string_view sv)
    {
        if (sv.empty())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, "bind", "empty bind address"));
        if (std::ranges::any_of(sv, [](char c) { return c == ' ' || c == '\t' || static_cast<unsigned char>(c) < 0x20; }))
            return std::unexpected(
                MakeError(ConfigErrorCode::TypeMismatch, "bind", std::format("invalid characters in bind address: {}", sv)));
        return std::string { sv };
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseMaxMemory(std::string_view sv)
    {
        return ParseByteSize(sv, "max-memory", QueryHostTotalMemoryBytes()).transform_error(WithArgvSource);
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseStorageMaxValue(std::string_view sv)
    {
        return ParseByteSize(sv, "storage-max-value").transform_error(WithArgvSource);
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

    [[nodiscard]] std::expected<ExecutionModel, ConfigError> ParseExecutionModel(std::string_view sv)
    {
        if (sv == "auto")
            return ExecutionModel::Auto;
        if (sv == "threaded")
            return ExecutionModel::Threaded;
        if (sv == "reactor")
            return ExecutionModel::Reactor;
        return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                         "execution-model",
                                         std::format("unknown model (expect auto|threaded|reactor): {}", sv)));
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
                                                                      CliResult& result)
    {
        auto& cfg = result.config;
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
        // Service-control requests record the desired outcome but keep parsing:
        // the remaining flags (--service-name, --port, --storage, ...) are
        // captured into the config that gets baked into the service command line.
        if (arg == "--install-service")
        {
            result.outcome = CliOutcome::InstallService;
            return ArgOutcome::Continue;
        }
        if (arg == "--uninstall-service")
        {
            result.outcome = CliOutcome::UninstallService;
            return ArgOutcome::Continue;
        }

        // String-valued flags. Each match flips an "explicit" bool so
        // Merge can override YAML even when the typed value happens to
        // equal the field's default.
        for (auto const& [name, target, seenPtr]: std::initializer_list<std::tuple<std::string_view, std::string*, bool*>> {
                 { "--config", &cfg.configPath, nullptr },
                 { "--pidfile", &cfg.pidfile, nullptr },
                 { "--service-name", &cfg.serviceName, nullptr },
                 { "--storage", &cfg.storagePath, &result.storagePathExplicit },
             })
        {
            auto const matched = ApplyStringFlag(args, i, name, *target);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                if (seenPtr != nullptr)
                    *seenPtr = true;
                return ArgOutcome::Continue;
            }
        }

        // Typed flags.
        {
            auto const matched = ApplyParsedFlag(args, i, "--bind", ParseBindAddress, cfg.bindAddress);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.bindAddressExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--port", ParsePort, cfg.port);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.portExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--max-memory", ParseMaxMemory, cfg.maxMemoryBytes);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.maxMemoryBytesExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--log-level", ParseLogLevel, cfg.logLevel);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.logLevelExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched =
                ApplyParsedFlag(args, i, "--storage-durability", ParseStorageDurability, cfg.storageDurability);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.storageDurabilityExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched =
                ApplyParsedFlag(args, i, "--storage-max-value", ParseStorageMaxValue, cfg.storageMaxValueBytes);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.storageMaxValueBytesExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--execution-model", ParseExecutionModel, cfg.executionModel);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.executionModelExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--threads", ParseThreads, cfg.workerThreads);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.workerThreadsExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        {
            auto const matched = ApplyParsedFlag(args, i, "--storage-shards", ParseStorageShards, cfg.storageShards);
            if (!matched.has_value())
                return std::unexpected(matched.error());
            if (*matched)
            {
                result.storageShardsExplicit = true;
                return ArgOutcome::Continue;
            }
        }
        return ArgOutcome::Unknown;
    }

} // namespace

namespace
{
    /// ANSI SGR escape sequences used to colorize help output. Every field is
    /// an empty string in the plain palette so the exact same renderer drives
    /// both colored (TTY) and plain (file/pipe/test) output.
    struct UsagePalette
    {
        std::string_view reset;   ///< Reset all attributes.
        std::string_view heading; ///< "usage:" prefix and example section titles.
        std::string_view flag;    ///< Option flag names.
    };

    /// Bold-cyan headings, bold-green flags.
    constexpr UsagePalette ColoredPalette { .reset = "\x1b[0m", .heading = "\x1b[1;36m", .flag = "\x1b[1;32m" };
    /// No escapes — identical layout, just no color.
    constexpr UsagePalette PlainPalette { .reset = "", .heading = "", .flag = "" };

    /// One documented command-line option. `description` may embed '\n'; the
    /// renderer re-indents every continuation line to the description column so
    /// wrapped text stays aligned under the first line.
    struct UsageOption
    {
        std::string_view flag;        ///< e.g. "--port=<num>".
        std::string_view description; ///< Help text; '\n' separates wrapped lines.
    };

    /// The option table — single source of truth for both the help text and its
    /// alignment. Add a row here and it lines up automatically.
    constexpr auto UsageOptions = std::to_array<UsageOption>({
        { .flag = "--config=<path>", .description = "YAML config file; CLI flags override file values" },
        { .flag = "--bind=<addr>", .description = "bind address: IPv4/IPv6 literal or hostname (default 127.0.0.1)" },
        { .flag = "--port=<num>", .description = "TCP port (default 11211)" },
        { .flag = "--max-memory=<size>",
          .description = "in-memory budget; k/m/g = KiB/MiB/GiB or N% of host RAM (default 64 MiB)" },
        { .flag = "--log-level=<level>", .description = "trace|debug|info|warn|error|fatal (default info)" },
        { .flag = "--storage=<path>", .description = "persist cache to a CoW-tree file (default: in-memory only)" },
        { .flag = "--storage-durability=<mode>", .description = "fsync|batched|none for --storage (default batched)" },
        { .flag = "--storage-max-value=<size>",
          .description = "per-value byte cap for --storage; k/m/g suffixes accepted (default 1m)" },
        { .flag = "--execution-model=<mode>",
          .description = "auto|threaded|reactor (default auto)\n"
                         "auto: reactor for in-memory, threaded for --storage on disk" },
        { .flag = "--threads=<N>", .description = "worker thread count for threaded mode (default: hardware_concurrency)" },
        { .flag = "--storage-shards=<N>",
          .description = "shard storage into N partitions for write parallelism\n"
                         "default: 1 (single-file mode) when --storage names a regular file,\n"
                         "min(16, hardware_concurrency) otherwise;\n"
                         "when N>1 and --storage is set, --storage must be a directory" },
        { .flag = "--daemon",
          .description = "daemonize (POSIX) / run under the Windows SCM (used by the installed service)" },
        { .flag = "--install-service",
          .description = "register fastcached as an auto-start Windows service (Windows only;\n"
                         "needs an elevated prompt; other flags are baked into the service)" },
        { .flag = "--uninstall-service",
          .description = "remove the fastcached Windows service (Windows only; needs elevation)" },
        { .flag = "--pidfile=<path>", .description = "POSIX daemon mode only" },
        { .flag = "--service-name=<name>", .description = "Windows service name (default FastCached)" },
        { .flag = "--help, -h", .description = "show this help and exit" },
        { .flag = "--version, -V", .description = "show version and exit" },
    });

    /// A worked example block printed below the option table.
    struct UsageExample
    {
        std::string_view title; ///< Section heading.
        std::string_view body;  ///< Shell snippet; '\n' separates lines.
    };

    /// Platform-specific sccache usage snippets (PowerShell vs POSIX shell).
    constexpr auto UsageExamples = std::to_array<UsageExample>({
#if defined(_WIN32)
        { .title = "Use with sccache (memcached protocol, PowerShell):",
          .body = "  Start-Process fastcached -ArgumentList '--port=11211'\n"
                  "  $env:SCCACHE_MEMCACHED = 'tcp://127.0.0.1:11211'\n"
                  "  sccache <compiler> /c hello.cpp /Fo:hello.obj" },
        { .title = "Use with sccache (Redis protocol, PowerShell):",
          .body = "  $env:SCCACHE_REDIS = 'redis://127.0.0.1:11211'" },
#else
        { .title = "Use with sccache (memcached protocol):",
          .body = "  fastcached --port=11211 &\n"
                  "  export SCCACHE_MEMCACHED=tcp://127.0.0.1:11211\n"
                  "  sccache <compiler> -c hello.c -o hello.o" },
        { .title = "Use with sccache (Redis protocol):", .body = "  export SCCACHE_REDIS=redis://127.0.0.1:11211" },
#endif
    });

    /// Closing note printed after the examples.
    constexpr std::string_view UsageFooter = "sccache <= 0.7 speaks memcached text; >= 0.8 speaks memcached binary;\n"
                                             "either works because fastcached auto-detects the wire format.\n";

    /// Invoke `fn(line)` for each '\n'-separated segment of `text`. A trailing
    /// segment with no newline is still delivered, so a non-terminated string
    /// yields exactly its visual lines.
    template <typename Fn>
    void ForEachLine(std::string_view text, Fn fn)
    {
        while (true)
        {
            auto const newline = text.find('\n');
            if (newline == std::string_view::npos)
            {
                fn(text);
                return;
            }
            fn(text.substr(0, newline));
            text.remove_prefix(newline + 1);
        }
    }

    /// Render the full usage text using the given color palette.
    /// @param palette Color escapes (ColoredPalette or PlainPalette).
    /// @return Column-aligned, optionally colored usage text.
    [[nodiscard]] std::string RenderUsage(UsagePalette const& palette)
    {
        constexpr std::size_t LeftIndent = 2; ///< Spaces before each flag.
        constexpr std::size_t ColumnGap = 2;  ///< Spaces between flag and description.

        // Align all descriptions to a column derived from the widest flag, so
        // the layout adapts automatically when options are added or renamed.
        auto const flagWidth = std::ranges::max(
            UsageOptions | std::views::transform([](UsageOption const& option) { return option.flag.size(); }));
        auto const descColumn = LeftIndent + flagWidth + ColumnGap;

        std::string out;
        out += std::format("{}usage:{} fastcached [options]\n", palette.heading, palette.reset);

        for (auto const& option: UsageOptions)
        {
            auto firstLine = true;
            ForEachLine(option.description, [&](std::string_view line) {
                if (firstLine)
                {
                    auto const pad = descColumn - LeftIndent - option.flag.size();
                    out += std::format("{}{}{}{}{}{}\n",
                                       std::string(LeftIndent, ' '),
                                       palette.flag,
                                       option.flag,
                                       palette.reset,
                                       std::string(pad, ' '),
                                       line);
                    firstLine = false;
                }
                else
                {
                    out += std::format("{}{}\n", std::string(descColumn, ' '), line);
                }
            });
        }

        for (auto const& example: UsageExamples)
        {
            out += '\n';
            out += std::format("{}{}{}\n", palette.heading, example.title, palette.reset);
            ForEachLine(example.body, [&](std::string_view line) { out += std::format("{}\n", line); });
        }

        out += '\n';
        out += UsageFooter;
        return out;
    }
} // namespace

std::string CliUsage(UsageColor color)
{
    return RenderUsage(color == UsageColor::Colored ? ColoredPalette : PlainPalette);
}

std::expected<CliResult, ConfigError> ParseCli(std::span<char const* const> args)
{
    CliResult outcome;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        auto const result = HandleOneArg(args, i, outcome);
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
