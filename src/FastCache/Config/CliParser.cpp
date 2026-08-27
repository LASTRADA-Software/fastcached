// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace FastCache
{

namespace
{

    /// Re-stamp the `source` of an error produced by a lower layer as "argv".
    [[nodiscard]] ConfigError WithArgvSource(ConfigError err)
    {
        err.source = "argv";
        return err;
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
            return std::unexpected(ArgvError(ConfigErrorCode::TypeMismatch, "bind", "empty bind address"));
        if (std::ranges::any_of(sv, [](char c) { return c == ' ' || c == '\t' || static_cast<unsigned char>(c) < 0x20; }))
            return std::unexpected(
                ArgvError(ConfigErrorCode::TypeMismatch, "bind", std::format("invalid characters in bind address: {}", sv)));
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

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseStorageMaxDisk(std::string_view sv)
    {
        return ParseByteSize(sv, "storage-max-disk").transform_error(WithArgvSource);
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParsePositiveInt(std::string_view sv, std::string_view field)
    {
        if (sv.empty())
            return std::unexpected(ArgvError(ConfigErrorCode::TypeMismatch, std::string { field }, "empty value"));
        std::size_t value = 0;
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        if (ec != std::errc {} || ptr != sv.data() + sv.size())
            return std::unexpected(
                ArgvError(ConfigErrorCode::TypeMismatch, std::string { field }, std::format("not a number: {}", sv)));
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

    [[nodiscard]] std::expected<int, ConfigError> ParseListenBacklog(std::string_view sv)
    {
        // The kernel clamps to its own SOMAXCONN ceiling, so we only guard
        // against absurd or non-positive input; 1..65535 is plenty of headroom.
        return ParsePositiveInt(sv, "listen-backlog").and_then([](std::size_t value) -> std::expected<int, ConfigError> {
            if (value == 0 || value > 65535)
                return std::unexpected(ArgvError(
                    ConfigErrorCode::OutOfRange, "listen-backlog", std::format("out of range (1..65535): {}", value)));
            return static_cast<int>(value);
        });
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
            ArgvError(ConfigErrorCode::OutOfRange, "storage-durability", std::format("unknown durability mode: {}", sv)));
    }

    [[nodiscard]] std::expected<LruRecency, ConfigError> ParseLruRecency(std::string_view sv)
    {
        if (sv == "approximate")
            return LruRecency::Approximate;
        if (sv == "strict")
            return LruRecency::Strict;
        return std::unexpected(ArgvError(
            ConfigErrorCode::OutOfRange, "lru-mode", std::format("unknown mode (expect approximate|strict): {}", sv)));
    }

    [[nodiscard]] std::expected<CpuAffinity, ConfigError> ParseCpuAffinity(std::string_view sv)
    {
        if (sv == "none")
            return CpuAffinity::None;
        if (sv == "per-core")
            return CpuAffinity::PerCore;
        return std::unexpected(ArgvError(
            ConfigErrorCode::OutOfRange, "cpu-affinity", std::format("unknown mode (expect none|per-core): {}", sv)));
    }

    [[nodiscard]] std::expected<CompressionCodec, ConfigError> ParseCompression(std::string_view sv)
    {
        auto const codec = Compression::CodecFromName(sv);
        if (!codec.has_value())
            return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange,
                                             "compression",
                                             std::format("unknown codec (expect {}): {}", Compression::NameList(), sv)));
        if (!Compression::IsAvailable(*codec))
            return std::unexpected(ArgvError(
                ConfigErrorCode::OutOfRange,
                "compression",
                std::format("codec '{}' is not available in this build (rebuild with FASTCACHED_ENABLE_COMPRESSION)", sv)));
        return *codec;
    }

    [[nodiscard]] std::expected<int, ConfigError> ParseCompressionLevel(std::string_view sv)
    {
        // zstd accepts roughly 1..22; keep a generous, codec-agnostic 1..22
        // guard so an absurd value is rejected up front.
        return ParsePositiveInt(sv, "compression-level").and_then([](std::size_t value) -> std::expected<int, ConfigError> {
            if (value == 0 || value > 22)
                return std::unexpected(ArgvError(
                    ConfigErrorCode::OutOfRange, "compression-level", std::format("out of range (1..22): {}", value)));
            return static_cast<int>(value);
        });
    }

    [[nodiscard]] std::expected<std::size_t, ConfigError> ParseCompressionMinBytes(std::string_view sv)
    {
        return ParseByteSize(sv, "compression-min-bytes").transform_error(WithArgvSource);
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
        return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, "log-level", std::format("unknown level: {}", sv)));
    }

    // The listener kinds live in Config.hpp's ListenerFlags table, so the flag that
    // PARSES an endpoint and the flag that SPELLS it back out in a service
    // registration are one row. They must agree, or a daemon registers with a
    // listener set it will not accept back.
    // Values, not references: a `constexpr auto const&` at namespace scope is a
    // global VARIABLE as far as readability-identifier-naming is concerned, and the
    // project's convention spells those camelBack. Copying a three-field aggregate
    // out of the table costs nothing and keeps these reading as the constants they
    // are -- each still initialized FROM the table, so there is one source of truth.
    constexpr ListenerFlagSpec PlainListener = ListenerFlags[0];
    constexpr ListenerFlagSpec TlsListener = ListenerFlags[1];

    /// Parse a listener flag's argument into a `BindConfig`. The grammar is the
    /// standard `host:port` form, with `[ipv6]:port` for IPv6 literals.
    /// @param sv   The flag's value text.
    /// @param kind Which listener flag this is.
    /// @return A populated BindConfig on success; ConfigError otherwise.
    [[nodiscard]] std::expected<BindConfig, ConfigError> ParseListenSpec(std::string_view sv, ListenerFlagSpec const& kind)
    {
        if (sv.empty())
            return std::unexpected(
                ArgvError(ConfigErrorCode::TypeMismatch, std::string { kind.flag }, "empty value (expected host:port)"));
        std::string_view host;
        std::string_view portText;
        if (sv.front() == '[')
        {
            // `[ipv6-literal]:port` form. Find the matching `]` and require
            // a `:port` tail immediately after.
            auto const close = sv.find(']');
            if (close == std::string_view::npos || close + 1 >= sv.size() || sv[close + 1] != ':')
                return std::unexpected(ArgvError(ConfigErrorCode::TypeMismatch,
                                                 std::string { kind.flag },
                                                 std::format("malformed [ipv6]:port spec: {}", sv)));
            host = sv.substr(1, close - 1);
            portText = sv.substr(close + 2);
        }
        else
        {
            // `host:port` form. The last `:` separates host and port — for
            // a bare IPv4 / hostname there is exactly one colon; for an
            // unbracketed IPv6 we reject (the standard requires brackets).
            auto const colon = sv.rfind(':');
            if (colon == std::string_view::npos)
                return std::unexpected(ArgvError(
                    ConfigErrorCode::TypeMismatch, std::string { kind.flag }, std::format("missing :port in: {}", sv)));
            host = sv.substr(0, colon);
            portText = sv.substr(colon + 1);
            // Reject unbracketed IPv6 literals: if `host` contains a `:`
            // we silently mis-parsed it (e.g. `2001:db8::1` would land
            // here with host=`2001:db8:` and portText=`1`). The comment
            // above promises rejection; the code now matches.
            if (host.contains(':'))
                return std::unexpected(
                    ArgvError(ConfigErrorCode::TypeMismatch,
                              std::string { kind.flag },
                              std::format("IPv6 literal requires brackets: [{}]:port (got: {})", host, sv)));
        }
        auto const address = ParseBindAddress(host);
        if (!address.has_value())
            return std::unexpected(address.error());
        auto const port = ParsePort(portText);
        if (!port.has_value())
            return std::unexpected(port.error());
        return BindConfig { .address = *address, .port = *port, .tls = kind.tls };
    }

    /// `ParseListenSpec` bound to one listener kind, so the two repeatable flags
    /// share one parser and differ only by the row that names them — and by a
    /// template argument rather than by a second copy of this body.
    /// @tparam Kind Which listener flag this is.
    /// @param sv The flag's value text.
    /// @return The BindConfig on success; ConfigError otherwise.
    template <ListenerFlagSpec const& Kind>
    [[nodiscard]] std::expected<BindConfig, ConfigError> ParseListen(std::string_view sv)
    {
        return ParseListenSpec(sv, Kind);
    }

    /// Every accepted command-line option, in the order `--help` documents them.
    ///
    /// One row per flag, carrying its spelling, how it is parsed, which
    /// "user typed this" tracker it flips, what it selects, and the help text
    /// describing it. `ParseCli` interprets these rows and `CliUsage` renders
    /// itself from the same ones, so the accepted spelling and the documented
    /// spelling cannot drift apart.
    ///
    /// Order is documentation order; it does not affect parsing. All primaries
    /// are distinct, and a value flag only claims an argument that continues
    /// with `=`, so a longer flag (`--listen-tls`) can never be swallowed by a
    /// shorter prefix of it (`--listen`).
    constexpr auto Options = std::to_array<OptionSpec<CliResult>>({
        { .primary = "--config",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&Config::configPath, ParseText>(),
          .description = "YAML config file; CLI flags override file values.\n"
                         "Without it, the first of these that exists and is readable is used:\n"
                         "{config-defaults}" },
        { .primary = "--bind",
          .arity = Arity::Value,
          .operand = "=<addr>",
          .apply = AssignFrom<&Config::bindAddress, ParseBindAddress>(),
          .explicitBit = &CliResult::bindAddressExplicit,
          .description = "bind address: IPv4/IPv6 literal or hostname; '::' is dual-stack (default 127.0.0.1)" },
        { .primary = "--port",
          .arity = Arity::Value,
          .operand = "=<num>",
          .apply = AssignFrom<&Config::port, ParsePort>(),
          .explicitBit = &CliResult::portExplicit,
          .description = "TCP port (default {port}); every protocol is auto-detected on it" },
        { .primary = "--max-memory",
          .arity = Arity::Value,
          .operand = "=<size>",
          .apply = AssignFrom<&Config::maxMemoryBytes, ParseMaxMemory>(),
          .explicitBit = &CliResult::maxMemoryBytesExplicit,
          .description = "in-memory budget; k/m/g = KiB/MiB/GiB or N% of host RAM\n"
                         "(default 25% of RAM, within 512m-8g)" },
        { .primary = "--log-level",
          .arity = Arity::Value,
          .operand = "=<level>",
          .apply = AssignFrom<&Config::logLevel, ParseLogLevel>(),
          .explicitBit = &CliResult::logLevelExplicit,
          .description = "trace|debug|info|warn|error|fatal (default info)" },
        { .primary = "--requirepass",
          .arity = Arity::Value,
          .operand = "=<secret>",
          .apply = AssignFrom<&Config::requirePass, ParseText>(),
          .explicitBit = &CliResult::requirePassExplicit,
          .description = "require clients to authenticate with this shared secret (default: no auth)\n"
                         "redis: AUTH; memcached binary: SASL PLAIN; memcached text has no auth" },
        { .primary = "--auth-username",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignFrom<&Config::authUsername, ParseText>(),
          .explicitBit = &CliResult::authUsernameExplicit,
          .description = "username for the AUTH <user> <pass> / SASL PLAIN form (default 'default')" },
        { .primary = "--metrics",
          .apply = SetTrue<&Config::metricsEnabled>(),
          .explicitBit = &CliResult::metricsEnabledExplicit,
          .description = "serve Prometheus /metrics and /healthz on a dedicated HTTP port (default off)" },
        { .primary = "--metrics-bind",
          .arity = Arity::Value,
          .operand = "=<addr>",
          .apply = AssignFrom<&Config::metricsBindAddress, ParseText>(),
          .explicitBit = &CliResult::metricsBindAddressExplicit,
          .description = "bind address for the metrics endpoint (default 127.0.0.1)" },
        { .primary = "--metrics-port",
          .arity = Arity::Value,
          .operand = "=<num>",
          .apply = AssignFrom<&Config::metricsPort, ParsePort>(),
          .explicitBit = &CliResult::metricsPortExplicit,
          .description = "TCP port for the metrics endpoint (default {metrics-port})" },
        { .primary = "--tls",
          .apply = SetTrue<&Config::tlsEnabled>(),
          .explicitBit = &CliResult::tlsEnabledExplicit,
          .description = "terminate TLS on the cache port (default off; needs a build with OpenSSL\n"
                         "and both --tls-cert and --tls-key)" },
        { .primary = "--tls-cert",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&Config::tlsCertPath, ParseText>(),
          .explicitBit = &CliResult::tlsCertPathExplicit,
          .description = "PEM certificate (chain) file for --tls / --listen-tls" },
        { .primary = "--tls-key",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&Config::tlsKeyPath, ParseText>(),
          .explicitBit = &CliResult::tlsKeyPathExplicit,
          .description = "PEM private key file for --tls / --listen-tls" },
        { .primary = "--listen",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AppendFrom<&Config::binds, ParseListen<PlainListener>>(),
          .description = "additional plaintext listener; repeatable. Use [::1]:{port} for IPv6 literals.\n"
                         "When given, supersedes --bind/--port — every endpoint must be listed.\n"
                         "Also how to keep serving legacy clients: add their port here." },
        { .primary = "--listen-tls",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AppendFrom<&Config::binds, ParseListen<TlsListener>>(),
          .description = "additional TLS listener; repeatable. Shares --tls-cert / --tls-key.\n"
                         "Needs a build with OpenSSL (FC_TLS_ENABLED)" },
        { .primary = "--notify-keyspace-events",
          .arity = Arity::Value,
          .operand = "=<flags>",
          .apply = AssignFrom<&Config::notifyKeyspaceEvents, ParseText>(),
          .explicitBit = &CliResult::notifyKeyspaceEventsExplicit,
          .description = "redis-style keyspace-event flag string; empty = off (default).\n"
                         "K=__keyspace, E=__keyevent, g=generic (del/expire/persist),\n"
                         "$=string (set/incr*), x=expired, e=evicted, A=alias for g$xe" },
        { .primary = "--log-timestamps",
          .apply = SetTrue<&Config::logTimestamps>(),
          .explicitBit = &CliResult::logTimestampsExplicit,
          .description = "prefix every log line with an ISO 8601 UTC timestamp (default off)" },
        { .primary = "--log-source",
          .apply = SetTrue<&Config::logSource>(),
          .explicitBit = &CliResult::logSourceExplicit,
          .description = "prefix every connection log line with the client IP (default off)" },
        { .primary = "--log-everything",
          .apply = SetTrue<&Config::logEverything>(),
          .explicitBit = &CliResult::logEverythingExplicit,
          .description = "include keepalive/admin commands (PING, HELLO, ...) in the trace command log, not just "
                         "keyspace data operations (default off)" },
        { .primary = "--storage",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&Config::storagePath, ParseText>(),
          .explicitBit = &CliResult::storagePathExplicit,
          .description = "persist cache to a CoW-tree file (default: in-memory only)" },
        { .primary = "--storage-durability",
          .arity = Arity::Value,
          .operand = "=<mode>",
          .apply = AssignFrom<&Config::storageDurability, ParseStorageDurability>(),
          .explicitBit = &CliResult::storageDurabilityExplicit,
          .description = "fsync|batched|none for --storage (default batched)" },
        { .primary = "--storage-max-value",
          .arity = Arity::Value,
          .operand = "=<size>",
          .apply = AssignFrom<&Config::storageMaxValueBytes, ParseStorageMaxValue>(),
          .explicitBit = &CliResult::storageMaxValueBytesExplicit,
          .description = "per-value byte cap for --storage; k/m/g suffixes accepted (default 256m).\n"
                         "also raises the wire frame-payload cap, so compile caches storing\n"
                         "large object files need this set above their biggest object" },
        { .primary = "--storage-max-disk",
          .arity = Arity::Value,
          .operand = "=<size>",
          .apply = AssignFrom<&Config::storageMaxDiskBytes, ParseStorageMaxDisk>(),
          .explicitBit = &CliResult::storageMaxDiskBytesExplicit,
          .description = "cap the on-disk (L2) tier for --storage; the CoW tree evicts its LRU tail to fit "
                         "(default 0 = unbounded). k/m/g suffixes accepted" },
        { .primary = "--compression",
          .arity = Arity::Value,
          .operand = "=<codec>",
          .apply = AssignFrom<&Config::compression, ParseCompression>(),
          .explicitBit = &CliResult::compressionExplicit,
          .description = "on-disk value codec for --storage: none|lz4|zstd (default zstd)\n"
                         "reads always return plaintext; each record decodes by its own tag" },
        { .primary = "--compression-level",
          .arity = Arity::Value,
          .operand = "=<N>",
          .apply = AssignFrom<&Config::compressionLevel, ParseCompressionLevel>(),
          .explicitBit = &CliResult::compressionLevelExplicit,
          .description = "codec effort level for --compression (1..22; default 3, zstd)" },
        { .primary = "--compression-min-bytes",
          .arity = Arity::Value,
          .operand = "=<size>",
          .apply = AssignFrom<&Config::compressionMinBytes, ParseCompressionMinBytes>(),
          .explicitBit = &CliResult::compressionMinBytesExplicit,
          .description = "skip compression for values smaller than this; k/m/g accepted (default 256)" },
        { .primary = "--lru-mode",
          .arity = Arity::Value,
          .operand = "=<mode>",
          .apply = AssignFrom<&Config::lruRecency, ParseLruRecency>(),
          .explicitBit = &CliResult::lruRecencyExplicit,
          .description = "approximate|strict in-memory LRU recency (default approximate)\n"
                         "approximate: same-shard reads run concurrently (faster);\n"
                         "strict: exact LRU order, reads serialise per shard" },
        { .primary = "--cpu-affinity",
          .arity = Arity::Value,
          .operand = "=<mode>",
          .apply = AssignFrom<&Config::cpuAffinity, ParseCpuAffinity>(),
          .explicitBit = &CliResult::cpuAffinityExplicit,
          .description = "none|per-core reactor thread pinning (default per-core)\n"
                         "per-core: pin each reactor to its own core (multi-reactor only)" },
        { .primary = "--threads",
          .arity = Arity::Value,
          .operand = "=<N>",
          .apply = AssignFrom<&Config::workerThreads, ParseThreads>(),
          .explicitBit = &CliResult::workerThreadsExplicit,
          .description = "number of independent pinned reactors to run (default hardware_concurrency);\n"
                         "each is a single-threaded event loop, so this is the server's across-core parallelism" },
        { .primary = "--listen-backlog",
          .arity = Arity::Value,
          .operand = "=<N>",
          .apply = AssignFrom<&Config::listenBacklog, ParseListenBacklog>(),
          .explicitBit = &CliResult::listenBacklogExplicit,
          .description = "::listen() backlog depth (default 511; clamped to the kernel's SOMAXCONN)" },
        { .primary = "--storage-shards",
          .arity = Arity::Value,
          .operand = "=<N>",
          .apply = AssignFrom<&Config::storageShards, ParseStorageShards>(),
          .explicitBit = &CliResult::storageShardsExplicit,
          .description = "shard storage into N partitions for write parallelism\n"
                         "default: 1 (single-file mode) when --storage names a regular file,\n"
                         "min(16, hardware_concurrency) otherwise;\n"
                         "when N>1 and --storage is set, --storage must be a directory" },
        { .primary = "--daemon",
          .apply = SetTrue<&Config::daemon>(),
          .description = "daemonize (POSIX) / run under the Windows SCM (used by the installed service)" },
        // Action flags select what the process does *instead* of running the
        // daemon, but deliberately keep parsing: the remaining flags are
        // captured into the config that gets baked into the service command line.
        { .primary = "--install-service",
          .select = SelectOutcome<&CliResult::outcome, CliOutcome::InstallService>(),
          .description = "register fastcached to start automatically: a Windows SCM service, or a\n"
                         "macOS launchd job (see --service-scope). Other flags are baked in.\n"
                         "Windows needs an elevated prompt; --service-scope=system needs sudo" },
        { .primary = "--uninstall-service",
          .select = SelectOutcome<&CliResult::outcome, CliOutcome::UninstallService>(),
          .description = "remove the registration made by --install-service (same privileges)" },
        // Targets the CliResult, not the Config: the scope selects where the
        // service is registered and has no meaning to a running daemon, so it
        // takes no part in the YAML merge and carries no explicit bit.
        { .primary = "--service-scope",
          .arity = Arity::Value,
          .operand = "=<user|system>",
          .apply = AssignFrom<&CliResult::serviceScope, ParseServiceScope>(),
          .description = "macOS only: which launchd domain --install-service acts on.\n"
                         "user (default) = a LaunchAgent in ~/Library/LaunchAgents, started at\n"
                         "login as you; system = a LaunchDaemon in /Library/LaunchDaemons,\n"
                         "started at boot as _fastcached (needs sudo)" },
        { .primary = "--healthcheck",
          .select = SelectOutcome<&CliResult::outcome, CliOutcome::HealthCheck>(),
          .description = "probe http://127.0.0.1:<metrics-port>/healthz and exit 0 (healthy) or 1\n"
                         "(self-contained container HEALTHCHECK; needs --metrics on the daemon)" },
        { .primary = "--seed-config",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&CliResult::seedConfigTemplate, ParseText>(),
          .select = SelectOutcome<&CliResult::outcome, CliOutcome::SeedConfig>(),
          .description = "copy <path> to the machine-wide config location, but only when no\n"
                         "config is there yet, then exit (used by the installer; needs the\n"
                         "same privileges as writing that location)" },
        { .primary = "--pidfile",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&Config::pidfile, ParseText>(),
          .description = "POSIX daemon mode only" },
        { .primary = "--service-name",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignFrom<&Config::serviceName, ParseText>(),
          .explicitBit = &CliResult::serviceNameExplicit,
          .description = "Windows service name (default FastCached)" },
        { .primary = "--help",
          .alias = "-h",
          .select = SelectOutcome<&CliResult::outcome, CliOutcome::ShowHelp>(),
          .flow = ParseFlow::Stop,
          .description = "show this help and exit" },
        { .primary = "--version",
          .alias = "-V",
          .select = SelectOutcome<&CliResult::outcome, CliOutcome::ShowVersion>(),
          .flow = ParseFlow::Stop,
          .description = "show version and exit" },
    });

    static_assert(TableIsWellFormed<CliResult>(Options),
                  "CLI option table is malformed: a row is undocumented, a value flag has no operand, "
                  "a row does nothing, or a spelling is claimed twice");

    /// The config locations `--help` lists, one per line, indented under the
    /// description column. Read straight off the table the lookup itself walks,
    /// so help cannot advertise a location the daemon would not actually read.
    /// @return Newline-separated lines, no trailing newline.
    [[nodiscard]] std::string FormatDefaultConfigLocations()
    {
        std::string out;
        for (auto const& candidate: DefaultConfigCandidates())
        {
            if (!out.empty())
                out += '\n';
            // The rows carry `{app}` so a second binary can look itself up in the
            // same table; help has to spell it out for this one.
            out += std::format("  {}", ExpandApplicationName(candidate.display, DaemonApplicationName));
        }
        return out;
    }

    /// The substitution table. A new default worth quoting in help is a new row
    /// here plus a `{token}` in the text — no change to the renderer.
    ///
    /// Built rather than `constexpr` because not every compiled default is a
    /// number: the config search path is assembled at runtime from the
    /// candidate table.
    /// @return The tokens `--help` splices into its text.
    [[nodiscard]] std::vector<UsageSubstitution> MakeUsageSubstitutions()
    {
        return {
            { .token = "{port}", .value = std::format("{}", DefaultPort) },
            { .token = "{metrics-port}", .value = std::format("{}", DefaultMetricsPort) },
            { .token = "{config-defaults}", .value = FormatDefaultConfigLocations() },
        };
    }

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
          .body = "  Start-Process fastcached\n"
                  "  $env:SCCACHE_MEMCACHED = 'tcp://127.0.0.1:{port}'\n"
                  "  sccache <compiler> /c hello.cpp /Fo:hello.obj" },
        { .title = "Use with sccache (Redis protocol, PowerShell):",
          .body = "  $env:SCCACHE_REDIS = 'redis://127.0.0.1:{port}'" },
#else
        { .title = "Use with sccache (memcached protocol):",
          .body = "  fastcached &\n"
                  "  export SCCACHE_MEMCACHED=tcp://127.0.0.1:{port}\n"
                  "  sccache <compiler> -c hello.c -o hello.o" },
        { .title = "Use with sccache (Redis protocol):", .body = "  export SCCACHE_REDIS=redis://127.0.0.1:{port}" },
#endif
    });

    /// Closing note printed after the examples.
    constexpr std::string_view UsageFooter = "sccache <= 0.7 speaks memcached text; >= 0.8 speaks memcached binary;\n"
                                             "either works because fastcached auto-detects the wire format.";

} // namespace

std::span<OptionSpec<CliResult> const> CliOptions() noexcept
{
    return Options;
}

std::expected<std::uint16_t, ConfigError> ParsePort(std::string_view sv)
{
    std::uint32_t raw = 0;
    auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), raw);
    if (ec != std::errc {} || ptr != sv.data() + sv.size())
        return std::unexpected(ArgvError(ConfigErrorCode::TypeMismatch, "port", std::format("not a number: {}", sv)));
    if (raw == 0 || raw > 65535)
        return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, "port", std::format("out of range: {}", raw)));
    return static_cast<std::uint16_t>(raw);
}

std::string CliUsage(UsageColor color)
{
    auto const substitutions = MakeUsageSubstitutions();

    UsageRows optionRows;
    AddOptionRows(optionRows, CliOptions());

    // The renderer supplies the section indent every example line shares; the
    // second level -- the body under its own title -- is still the two spaces
    // each `body` literal carries.
    std::vector<std::string> exampleTexts;
    exampleTexts.reserve(UsageExamples.size());
    for (auto const& example: UsageExamples)
        exampleTexts.push_back(std::format("{}\n{}", example.title, example.body));

    std::vector<UsageBlock> blocks;
    blocks.reserve(1 + exampleTexts.size() + 1);
    blocks.push_back({ .entries = optionRows.Rows() });
    for (auto const& text: exampleTexts)
        blocks.push_back({ .text = text, .textIndent = 2 });
    blocks.push_back({ .text = UsageFooter });

    std::span<UsageBlock const> const allBlocks { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .title = "usage:", .subject = " fastcached [options]" },
        { .title = "OPTIONS", .blocks = allBlocks.subspan(0, 1) },
        { .title = "EXAMPLES", .blocks = allBlocks.subspan(1, exampleTexts.size()) },
        { .blocks = allBlocks.subspan(1 + exampleTexts.size(), 1) },
    });

    return RenderUsage({ .sections = sections }, color, substitutions);
}

std::expected<CliResult, ConfigError> ParseCli(std::span<char const* const> args)
{
    return ParseOptions(CliOptions(), args);
}

} // namespace FastCache
