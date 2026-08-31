// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/EnvExpand.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <yaml-cpp/yaml.h>

#include <array>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache
{

namespace
{

    [[nodiscard]] ConfigError MakeError(
        ConfigErrorCode code, std::filesystem::path const& path, std::string field, std::string context, unsigned line = 0)
    {
        return ConfigError {
            .code = code,
            .source = path.string(),
            .line = line,
            .field = std::move(field),
            .context = std::move(context),
        };
    }

    /// Expand `$VAR` / `${VAR}` in a path-valued setting, re-stamping the error
    /// with this file and line so a bad reference points at the config line
    /// carrying it rather than at nothing.
    [[nodiscard]] std::expected<std::string, ConfigError> ExpandPathValue(std::string_view raw,
                                                                          std::string_view field,
                                                                          std::filesystem::path const& path,
                                                                          unsigned line)
    {
        auto expanded = ExpandEnvironmentVariables(raw, field);
        if (expanded.has_value())
            return expanded;

        auto err = std::move(expanded).error();
        err.source = path.string();
        err.line = line;
        return std::unexpected(std::move(err));
    }

    /// One path-valued setting: a YAML key and the Config field it fills.
    struct PathSetting
    {
        std::string_view key;         ///< YAML key naming this setting.
        std::string Config::* member; ///< Field the expanded value lands in.
    };

    /// The settings that take `$VAR` / `${VAR}` expansion. All three share one
    /// treatment — expand, propagate the error, assign — so they are a table
    /// the dispatcher walks rather than three copies of the same block.
    ///
    /// Deliberately not every string-valued key: expanding `requirepass` would
    /// turn a secret that merely contains a dollar sign into a different secret,
    /// silently. A new path setting is a row here.
    constexpr auto PathSettings = std::to_array<PathSetting>({
        { .key = "storage_path", .member = &Config::storagePath },
        { .key = "tls_cert", .member = &Config::tlsCertPath },
        { .key = "tls_key", .member = &Config::tlsKeyPath },
    });

    /// The path setting a YAML key names, or nullptr if the key is not one.
    ///
    /// Returns a pointer rather than an iterator on purpose: `std::array`'s
    /// iterator is a raw pointer in libstdc++ but a class type in MSVC's debug
    /// STL, so a caller cannot spell the type portably.
    ///
    /// @param key YAML key to look up.
    /// @return Pointer into PathSettings, or nullptr when `key` is not a path setting.
    [[nodiscard]] PathSetting const* FindPathSetting(std::string_view key)
    {
        for (auto const& setting: PathSettings)
            if (setting.key == key)
                return &setting;
        return nullptr;
    }

    [[nodiscard]] std::expected<StorageDurability, ConfigError> ParseStorageDurability(std::string_view sv,
                                                                                       std::filesystem::path const& path,
                                                                                       unsigned line)
    {
        if (sv == "fsync")
            return StorageDurability::Fsync;
        if (sv == "batched")
            return StorageDurability::Batched;
        if (sv == "none")
            return StorageDurability::None;
        return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                         path,
                                         "storage_durability",
                                         std::string { "unknown durability mode: " } + std::string { sv },
                                         line));
    }

    [[nodiscard]] std::expected<CompressionCodec, ConfigError> ParseCompression(std::string_view sv,
                                                                                std::filesystem::path const& path,
                                                                                unsigned line)
    {
        auto const codec = Compression::CodecFromName(sv);
        if (!codec.has_value())
            return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                             path,
                                             "compression",
                                             std::string { "unknown codec (expect none|lz4|zstd): " } + std::string { sv },
                                             line));
        if (!Compression::IsAvailable(*codec))
            return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                             path,
                                             "compression",
                                             std::string { "codec not available in this build: " } + std::string { sv },
                                             line));
        return *codec;
    }

    [[nodiscard]] std::expected<LogLevel, ConfigError> ParseLogLevel(std::string_view sv,
                                                                     std::filesystem::path const& path,
                                                                     unsigned line)
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
        return std::unexpected(MakeError(
            ConfigErrorCode::OutOfRange, path, "log_level", std::string { "unknown level: " } + std::string { sv }, line));
    }

} // namespace

namespace
{

    [[nodiscard]] std::expected<YAML::Node, ConfigError> LoadRoot(std::filesystem::path const& path)
    {
        try
        {
            return YAML::LoadFile(path.string());
        }
        catch (YAML::BadFile const&)
        {
            // Caught BEFORE the generic handler, which is what makes the code
            // right: `YAML::BadFile` derives from `YAML::Exception`, so a path that
            // does not exist used to arrive as `ParseError` carrying yaml-cpp's
            // "bad file: <path>". A missing file is the commonest way to get this
            // wrong -- a typo in `--config` -- and `ParseError` sends an operator
            // to look for a syntax mistake in a file that is not there.
            //
            // The code is what monitoring and every caller switch on, and
            // `FileNotFound` is documented for exactly this. yaml-cpp cannot tell
            // absent from unreadable, so the sentence says both rather than
            // asserting the one it did not check.
            return std::unexpected(
                MakeError(ConfigErrorCode::FileNotFound, path, {}, "no such file, or this account may not read it"));
        }
        catch (YAML::ParserException const& e)
        {
            return std::unexpected(
                MakeError(ConfigErrorCode::ParseError, path, {}, e.msg, static_cast<unsigned>(e.mark.line + 1)));
        }
        catch (YAML::Exception const& e)
        {
            return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, e.what()));
        }
    }

    /// One-based YAML source line for an error message. yaml-cpp's `Mark::line`
    /// is zero-based and signed; this normalises to the human convention.
    /// @param node Source node whose mark is consulted.
    /// @return One-based line number, or 0 if the mark is missing.
    [[nodiscard]] unsigned YamlLine(YAML::Node const& node) noexcept
    {
        auto const raw = node.Mark().line;
        return raw < 0 ? 0U : static_cast<unsigned>(raw) + 1U;
    }

    /// Parse one entry of a `listeners:` YAML sequence into a `BindConfig`.
    /// Required fields: `address` (string), `port` (1..65535). Optional: `tls`
    /// (boolean, defaults to false) and `roles` (sequence, defaults to
    /// `[cache]`). Any other key is rejected so a typo (`tsl: true`) fails fast
    /// instead of silently disabling TLS.
    /// @param node The YAML node for one listener entry (must be a Map).
    /// @param path Source YAML file for error reporting.
    /// @param idx  Zero-based index within the `listeners:` sequence.
    /// @return The populated BindConfig, or ConfigError on malformed input.
    [[nodiscard]] std::expected<BindConfig, ConfigError> ParseListenerEntry(YAML::Node const& node,
                                                                            std::filesystem::path const& path,
                                                                            std::size_t idx)
    {
        auto field = std::string { "listeners[" };
        field += std::to_string(idx);
        field += ']';
        if (!node.IsMap())
            return std::unexpected(MakeError(
                ConfigErrorCode::TypeMismatch, path, field, "expected a map with address/port/tls", YamlLine(node)));
        BindConfig bind {};
        bool sawAddress = false;
        bool sawPort = false;
        for (auto const& kv: node)
        {
            auto const k = kv.first.as<std::string>();
            auto const line = YamlLine(kv.second);
            if (k == "address")
            {
                bind.address = kv.second.as<std::string>();
                sawAddress = true;
            }
            else if (k == "port")
            {
                auto const raw = kv.second.as<int>();
                if (raw <= 0 || raw > 65535)
                {
                    auto portField = field;
                    portField += ".port";
                    return std::unexpected(
                        MakeError(ConfigErrorCode::OutOfRange, path, std::move(portField), "must be in 1..65535", line));
                }
                bind.port = static_cast<std::uint16_t>(raw);
                sawPort = true;
            }
            else if (k == "tls")
            {
                bind.tls = kv.second.as<bool>();
            }
            else
            {
                auto unknownField = field;
                unknownField += '.';
                unknownField += k;
                return std::unexpected(
                    MakeError(ConfigErrorCode::ParseError, path, std::move(unknownField), "unknown listener field", line));
            }
        }
        if (!sawAddress)
        {
            auto missingField = field;
            missingField += ".address";
            return std::unexpected(MakeError(
                ConfigErrorCode::ParseError, path, std::move(missingField), "missing required field", YamlLine(node)));
        }
        if (!sawPort)
        {
            auto missingField = field;
            missingField += ".port";
            return std::unexpected(MakeError(
                ConfigErrorCode::ParseError, path, std::move(missingField), "missing required field", YamlLine(node)));
        }
        return bind;
    }

    /// Parse the top-level `listeners:` YAML sequence into `cfg.binds`.
    /// Extracted from ApplyEntry to keep that function under the
    /// cognitive-complexity threshold; the body is just a sequence loop with
    /// validation around it.
    [[nodiscard]] std::expected<void, ConfigError> ApplyListenersKey(Config& cfg,
                                                                     YAML::Node const& valueNode,
                                                                     std::filesystem::path const& path,
                                                                     unsigned line)
    {
        if (!valueNode.IsSequence())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch,
                                             path,
                                             "listeners",
                                             "expected a sequence of {address, port, tls?} maps",
                                             line));
        if (valueNode.size() == 0)
            return std::unexpected(
                MakeError(ConfigErrorCode::ParseError, path, "listeners", "listener sequence must not be empty", line));
        cfg.binds.clear();
        cfg.binds.reserve(valueNode.size());
        // yaml-cpp's Node has no iterator concept we can pipe through
        // std::ranges, so iterate over an index range with std::views::iota.
        // Each listener row's diagnostic must name its position in the
        // sequence, so we need the index, not just the element.
        for (auto const i: std::views::iota(std::size_t { 0 }, valueNode.size()))
        {
            auto entry = ParseListenerEntry(valueNode[i], path, i);
            if (!entry.has_value())
                return std::unexpected(std::move(entry).error());
            cfg.binds.push_back(*entry);
        }
        return {};
    }

    /// Apply the compression-related YAML keys. Factored out of ApplyEntry to
    /// keep that function's branch count within the clang-tidy cognitive-
    /// complexity budget while preserving the one-arm-per-key style.
    /// @return `nullopt` if `key` is not a compression key (caller keeps
    ///         matching); otherwise the parse result for that key.
    [[nodiscard]] std::optional<std::expected<void, ConfigError>> ApplyCompressionEntry(
        Config& cfg, std::string const& key, YAML::Node const& valueNode, std::filesystem::path const& path, unsigned line)
    {
        /// `compression`: none | lz4 | zstd on-disk value codec.
        if (key == "compression")
        {
            auto const c = ParseCompression(valueNode.as<std::string>(), path, line);
            if (!c.has_value())
                return std::expected<void, ConfigError> { std::unexpect, c.error() };
            cfg.compression = *c;
            return std::expected<void, ConfigError> {};
        }
        /// `compression_level`: codec effort level (1..22).
        if (key == "compression_level")
        {
            auto const raw = valueNode.as<int>();
            if (raw < 1 || raw > 22)
                return std::expected<void, ConfigError> {
                    std::unexpect,
                    MakeError(ConfigErrorCode::OutOfRange, path, "compression_level", "must be in 1..22", line)
                };
            cfg.compressionLevel = raw;
            return std::expected<void, ConfigError> {};
        }
        /// `compression_min_bytes`: skip compression below this size.
        if (key == "compression_min_bytes")
        {
            auto const raw = valueNode.as<std::string>();
            auto parsed = ParseByteSize(raw, "compression_min_bytes");
            if (!parsed.has_value())
            {
                auto err = std::move(parsed).error();
                err.source = path.string();
                err.line = line;
                return std::expected<void, ConfigError> { std::unexpect, std::move(err) };
            }
            cfg.compressionMinBytes = *parsed;
            return std::expected<void, ConfigError> {};
        }
        /// `memory_compression`: none | lz4 | zstd codec for the IN-MEMORY tier.
        if (key == "memory_compression")
        {
            auto const c = ParseCompression(valueNode.as<std::string>(), path, line);
            if (!c.has_value())
                return std::expected<void, ConfigError> { std::unexpect, c.error() };
            cfg.memoryCompression = *c;
            return std::expected<void, ConfigError> {};
        }
        /// `memory_compression_level`: codec effort level (1..22).
        if (key == "memory_compression_level")
        {
            auto const raw = valueNode.as<int>();
            if (raw < 1 || raw > 22)
                return std::expected<void, ConfigError> {
                    std::unexpect,
                    MakeError(ConfigErrorCode::OutOfRange, path, "memory_compression_level", "must be in 1..22", line)
                };
            cfg.memoryCompressionLevel = raw;
            return std::expected<void, ConfigError> {};
        }
        /// `memory_compression_min_bytes`: keep values below this uncompressed.
        if (key == "memory_compression_min_bytes")
        {
            auto const raw = valueNode.as<std::string>();
            auto parsed = ParseByteSize(raw, "memory_compression_min_bytes");
            if (!parsed.has_value())
            {
                auto err = std::move(parsed).error();
                err.source = path.string();
                err.line = line;
                return std::expected<void, ConfigError> { std::unexpect, std::move(err) };
            }
            cfg.memoryCompressionMinBytes = *parsed;
            return std::expected<void, ConfigError> {};
        }
        return std::nullopt;
    }

    /// Apply the active-expiry keys, if `key` names one of them.
    ///
    /// Extracted from ApplyEntry to keep that function under the
    /// cognitive-complexity threshold, exactly as the compression keys are --
    /// adding these two to the ladder is what pushed it over.
    ///
    /// Both read as `long long` rather than the `int` the neighbouring keys use,
    /// so a figure typed in the wrong unit lands in the range check below and is
    /// refused by name. Read as `int` it would overflow the conversion first and
    /// come back as "bad conversion", which says nothing about what to type
    /// instead -- and being told what to type instead is the entire point of a
    /// ceiling nobody reaches on purpose.
    /// @param cfg       Config being populated.
    /// @param key       The key being applied.
    /// @param valueNode Its value node.
    /// @param path      Config file path, for diagnostics.
    /// @param line      Line number, for diagnostics.
    /// @return nullopt when `key` is not one of these; otherwise the outcome.
    [[nodiscard]] std::optional<std::expected<void, ConfigError>> ApplyExpiryEntry(
        Config& cfg, std::string const& key, YAML::Node const& valueNode, std::filesystem::path const& path, unsigned line)
    {
        /// `active_expiry_interval_ms`: expiry sweep period; 0 disables the cycle.
        if (key == "active_expiry_interval_ms")
        {
            auto const raw = valueNode.as<long long>();
            constexpr long long Ceiling = 86'400'000;
            if (raw < 0 || raw > Ceiling)
                return std::expected<void, ConfigError> {
                    std::unexpect,
                    MakeError(ConfigErrorCode::OutOfRange, path, "active_expiry_interval_ms", "must be in 0..86400000", line)
                };
            cfg.activeExpiryIntervalMs = static_cast<std::uint32_t>(raw);
            return std::expected<void, ConfigError> {};
        }
        /// `active_expiry_scan`: entries one sweep examines per shard. Zero is
        /// `PurgeBudget`'s spelling of *no ceiling*, so it is refused here.
        if (key == "active_expiry_scan")
        {
            auto const raw = valueNode.as<long long>();
            if (raw < 1)
                return std::expected<void, ConfigError> {
                    std::unexpect, MakeError(ConfigErrorCode::OutOfRange, path, "active_expiry_scan", "must be >= 1", line)
                };
            cfg.activeExpiryScanBudget = static_cast<std::size_t>(raw);
            return std::expected<void, ConfigError> {};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::expected<void, ConfigError> ApplyEntry(
        Config& cfg, std::string const& key, YAML::Node const& valueNode, std::filesystem::path const& path, unsigned line)
    {
        /// `bind`: interface address to listen on. Free-form string; left to
        /// the OS resolver. Examples: "127.0.0.1", "0.0.0.0", "::".
        if (key == "bind")
        {
            cfg.bindAddress = valueNode.as<std::string>();
            return {};
        }
        /// `port`: TCP listen port. Integer in 1..65535.
        if (key == "port")
        {
            auto const raw = valueNode.as<int>();
            if (raw <= 0 || raw > 65535)
                return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, path, "port", "must be in 1..65535", line));
            cfg.port = static_cast<std::uint16_t>(raw);
            return {};
        }
        /// `max_memory`: in-memory cache byte budget. Integer with optional
        /// unit suffix k/K=1024, m/M=1024², g/G=1024³ (1024-based). Plain
        /// integer means bytes. A trailing "%" sets the budget to that
        /// percentage of the host's total RAM (e.g., 50%). 0 disables
        /// eviction.
        if (key == "max_memory")
        {
            auto const raw = valueNode.as<std::string>();
            auto parsed = ParseByteSize(raw, "max_memory", QueryHostTotalMemoryBytes());
            if (!parsed.has_value())
            {
                auto err = std::move(parsed).error();
                err.source = path.string();
                err.line = line;
                return std::unexpected(std::move(err));
            }
            cfg.maxMemoryBytes = *parsed;
            return {};
        }
        /// `log_level`: verbosity. One of trace|debug|info|warn|error|fatal.
        if (key == "log_level")
        {
            auto const level = ParseLogLevel(valueNode.as<std::string>(), path, line);
            if (!level.has_value())
                return std::unexpected(level.error());
            cfg.logLevel = *level;
            return {};
        }
        /// `log_timestamps`: prefix log lines with ISO 8601 UTC timestamps.
        if (key == "log_timestamps")
        {
            cfg.logTimestamps = valueNode.as<bool>();
            return {};
        }
        /// `log_source`: prefix connection log lines with the client IP.
        if (key == "log_source")
        {
            cfg.logSource = valueNode.as<bool>();
            return {};
        }
        /// `log_everything`: include keepalive/admin commands in the command log.
        if (key == "log_everything")
        {
            cfg.logEverything = valueNode.as<bool>();
            return {};
        }
        /// The path-valued settings — `storage_path` (CoW-tree backing file),
        /// `tls_cert` and `tls_key`. Each has `$VAR` / `${VAR}` expanded before
        /// it is stored; see PathSettings for the table and EnvExpand.hpp for
        /// the grammar.
        if (auto const* const setting = FindPathSetting(key); setting != nullptr)
        {
            auto expanded = ExpandPathValue(valueNode.as<std::string>(), setting->key, path, line);
            if (!expanded.has_value())
                return std::expected<void, ConfigError> { std::unexpect, std::move(expanded).error() };
            cfg.*(setting->member) = *std::move(expanded);
            return {};
        }
        /// `requirepass`: shared authentication secret (redis-style). Empty or
        /// absent = auth disabled.
        if (key == "requirepass")
        {
            cfg.requirePass = valueNode.as<std::string>();
            return {};
        }
        /// `auth_username`: username for the AUTH <user> <pass> / SASL PLAIN
        /// form. Defaults to "default".
        if (key == "auth_username")
        {
            cfg.authUsername = valueNode.as<std::string>();
            return {};
        }
        /// `metrics`: enable the admin HTTP endpoint (/metrics, /healthz).
        if (key == "metrics")
        {
            cfg.metricsEnabled = valueNode.as<bool>();
            return {};
        }
        /// `metrics_bind`: bind address for the admin HTTP endpoint.
        if (key == "metrics_bind")
        {
            cfg.metricsBindAddress = valueNode.as<std::string>();
            return {};
        }
        /// `metrics_port`: TCP port for the admin HTTP endpoint (1..65535).
        if (key == "metrics_port")
        {
            auto const raw = valueNode.as<int>();
            if (raw <= 0 || raw > 65535)
                return std::unexpected(
                    MakeError(ConfigErrorCode::OutOfRange, path, "metrics_port", "must be in 1..65535", line));
            cfg.metricsPort = static_cast<std::uint16_t>(raw);
            return {};
        }
        /// `tls`: terminate TLS on the cache port (needs an OpenSSL build).
        if (key == "tls")
        {
            cfg.tlsEnabled = valueNode.as<bool>();
            return {};
        }
        /// (`tls_cert` and `tls_key` are handled by the PathSettings branch above.)
        /// `notify_keyspace_events`: redis-style keyspace-event flag string
        /// (e.g. "AKE"). Default empty (off). Parsed (validated) at daemon
        /// startup; an unknown letter fails fast.
        if (key == "notify_keyspace_events")
        {
            cfg.notifyKeyspaceEvents = valueNode.as<std::string>();
            return {};
        }
        /// `listeners`: sequence of {address, port, tls?} maps. Populates
        /// `cfg.binds`. When present, supersedes the legacy single-bind
        /// `bind:` / `port:` fields. Empty sequence is rejected — the
        /// operator likely meant to write a list and is asking for a footgun
        /// (a daemon that comes up with zero listeners would still pass the
        /// legacy fallback in main.cpp and may not be what they intended).
        if (key == "listeners")
            return ApplyListenersKey(cfg, valueNode, path, line);
        /// `storage_durability`: fsync|batched|none.
        if (key == "storage_durability")
        {
            auto const d = ParseStorageDurability(valueNode.as<std::string>(), path, line);
            if (!d.has_value())
                return std::unexpected(d.error());
            cfg.storageDurability = *d;
            return {};
        }
        /// `storage_max_value`: per-value byte cap for the persistent
        /// backend. Same byte-size grammar as `max_memory` but without
        /// the `%` form (host-RAM-relative makes no sense for a value
        /// size).
        if (key == "storage_max_value")
        {
            auto const raw = valueNode.as<std::string>();
            auto parsed = ParseByteSize(raw, "storage_max_value");
            if (!parsed.has_value())
            {
                auto err = std::move(parsed).error();
                err.source = path.string();
                err.line = line;
                return std::unexpected(std::move(err));
            }
            cfg.storageMaxValueBytes = *parsed;
            return {};
        }
        /// `storage_max_disk`: cap the on-disk (L2) tier for the persistent
        /// backend; the CoW tree evicts its LRU tail to fit. Same byte-size
        /// grammar as `storage_max_value`. 0 (or unset) = unbounded.
        if (key == "storage_max_disk")
        {
            auto const raw = valueNode.as<std::string>();
            auto parsed = ParseByteSize(raw, "storage_max_disk");
            if (!parsed.has_value())
            {
                auto err = std::move(parsed).error();
                err.source = path.string();
                err.line = line;
                return std::unexpected(std::move(err));
            }
            cfg.storageMaxDiskBytes = *parsed;
            return {};
        }
        /// `lru_mode`: approximate | strict in-memory LRU recency policy.
        if (key == "lru_mode")
        {
            auto const raw = valueNode.as<std::string>();
            if (raw == "approximate")
                cfg.lruRecency = LruRecency::Approximate;
            else if (raw == "strict")
                cfg.lruRecency = LruRecency::Strict;
            else
                return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                                 path,
                                                 "lru_mode",
                                                 std::string { "unknown mode (expect approximate|strict): " } + raw,
                                                 line));
            return {};
        }
        /// `cpu_affinity`: none | per-core reactor thread pinning.
        if (key == "cpu_affinity")
        {
            auto const raw = valueNode.as<std::string>();
            if (raw == "none")
                cfg.cpuAffinity = CpuAffinity::None;
            else if (raw == "per-core")
                cfg.cpuAffinity = CpuAffinity::PerCore;
            else
                return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                                 path,
                                                 "cpu_affinity",
                                                 std::string { "unknown mode (expect none|per-core): " } + raw,
                                                 line));
            return {};
        }
        /// Compression keys are handled in a dedicated helper to keep this
        /// function within the cognitive-complexity budget.
        if (auto handled = ApplyCompressionEntry(cfg, key, valueNode, path, line); handled.has_value())
            return std::move(*handled);
        /// `threads`: positive integer worker count for threaded mode.
        if (key == "threads")
        {
            auto const raw = valueNode.as<int>();
            if (raw < 0)
                return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, path, "threads", "must be >= 0", line));
            cfg.workerThreads = static_cast<std::size_t>(raw);
            return {};
        }
        /// `storage_shards`: positive integer shard count.
        if (key == "storage_shards")
        {
            auto const raw = valueNode.as<int>();
            if (raw < 0)
                return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, path, "storage_shards", "must be >= 0", line));
            cfg.storageShards = static_cast<std::size_t>(raw);
            return {};
        }
        /// The active-expiry keys are handled in a dedicated helper, for the
        /// same reason the compression ones are.
        if (auto handled = ApplyExpiryEntry(cfg, key, valueNode, path, line); handled.has_value())
            return std::move(*handled);
        /// `listen_backlog`: ::listen() backlog depth (1..65535).
        if (key == "listen_backlog")
        {
            auto const raw = valueNode.as<int>();
            if (raw < 1 || raw > 65535)
                return std::unexpected(
                    MakeError(ConfigErrorCode::OutOfRange, path, "listen_backlog", "must be in 1..65535", line));
            cfg.listenBacklog = raw;
            return {};
        }
        return std::unexpected(MakeError(ConfigErrorCode::UnknownKey, path, key, "unrecognised key", line));
    }

} // namespace

std::expected<Config, ConfigError> ReadYamlConfig(std::filesystem::path const& path)
{
    if (!std::filesystem::exists(path))
        return std::unexpected(MakeError(ConfigErrorCode::FileNotFound, path, {}, "no such file"));

    auto root = LoadRoot(path);
    if (!root.has_value())
        return std::unexpected(root.error());

    if (!root->IsMap() && !root->IsNull())
        return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, "top-level must be a map"));

    Config cfg;
    if (!root->IsMap())
        return cfg;

    for (auto const& kv: *root)
    {
        auto const keyNode = kv.first;
        auto const valueNode = kv.second;
        if (!keyNode.IsScalar())
            return std::unexpected(MakeError(
                ConfigErrorCode::ParseError, path, {}, "non-scalar key", static_cast<unsigned>(keyNode.Mark().line + 1)));

        auto const line = static_cast<unsigned>(valueNode.Mark().line + 1);
        auto const key = keyNode.as<std::string>(); // safe: keyNode.IsScalar() checked above

        // A malformed scalar (e.g. `metrics: maybe`, `metrics_port: oops`) makes
        // yaml-cpp's valueNode.as<T>() throw TypedBadConversion. Translate it into
        // a clean ConfigError so a config typo is reported as a TypeMismatch with
        // file:line rather than escaping this std::expected API as an uncaught
        // exception that terminates the process at startup.
        try
        {
            if (auto const result = ApplyEntry(cfg, key, valueNode, path, line); !result.has_value())
                return std::unexpected(result.error());
        }
        catch (std::exception const& ex)
        {
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, path, key, ex.what(), line));
        }
    }

    return cfg;
}

std::expected<YamlConfigWithPresence, ConfigError> ReadYamlConfigWithPresence(std::filesystem::path const& path)
{
    if (!std::filesystem::exists(path))
        return std::unexpected(MakeError(ConfigErrorCode::FileNotFound, path, {}, "no such file"));

    auto root = LoadRoot(path);
    if (!root.has_value())
        return std::unexpected(root.error());

    if (!root->IsMap() && !root->IsNull())
        return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, "top-level must be a map"));

    YamlConfigWithPresence out;
    if (!root->IsMap())
        return out;

    for (auto const& kv: *root)
    {
        auto const keyNode = kv.first;
        auto const valueNode = kv.second;
        if (!keyNode.IsScalar())
            return std::unexpected(MakeError(
                ConfigErrorCode::ParseError, path, {}, "non-scalar key", static_cast<unsigned>(keyNode.Mark().line + 1)));

        auto const line = static_cast<unsigned>(valueNode.Mark().line + 1);
        auto const key = keyNode.as<std::string>();

        try
        {
            if (auto const result = ApplyEntry(out.config, key, valueNode, path, line); !result.has_value())
                return std::unexpected(result.error());
        }
        catch (std::exception const& ex)
        {
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, path, key, ex.what(), line));
        }

        // Record presence for fields whose env-fallback logic needs to
        // distinguish "explicitly set to the default" from "not mentioned".
        // Mirrors the per-flag *Explicit bits CliResult tracks for the CLI.
        if (key == "metrics_port")
            out.metricsPortExplicit = true;
        else if (key == "metrics_bind")
            out.metricsBindAddressExplicit = true;
        else if (key == "metrics")
            out.metricsEnabledExplicit = true;
        else if (key == "requirepass")
            out.requirePassExplicit = true;
        else if (key == "auth_username")
            out.authUsernameExplicit = true;
        else if (key == "tls")
            out.tlsEnabledExplicit = true;
        else if (key == "tls_cert")
            out.tlsCertPathExplicit = true;
        else if (key == "tls_key")
            out.tlsKeyPathExplicit = true;
        else if (key == "bind")
            out.bindAddressExplicit = true;
        else if (key == "port")
            out.portExplicit = true;
    }

    return out;
}

std::expected<std::vector<YamlSetting>, ConfigError> ReadYamlSettings(std::filesystem::path const& path)
{
    auto root = LoadRoot(path);
    if (!root.has_value())
        return std::unexpected(root.error());

    // A null document is an empty file, or one that is entirely comments. Both are
    // ordinary: both reference configurations this project ships are nothing BUT
    // commented-out keys, so an operator who has not uncommented any of them yet has
    // a valid configuration rather than a broken one -- which is every fresh package
    // install.
    if (root->IsNull())
        return std::vector<YamlSetting> {};
    if (!root->IsMap())
        return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, "top-level must be a map"));

    std::vector<YamlSetting> settings;
    for (auto const& kv: *root)
    {
        auto const key = kv.first.as<std::string>();
        auto const& value = kv.second;
        YamlSetting setting { .key = key, .values = {}, .line = YamlLine(kv.first) };

        if (value.IsScalar())
            setting.values.push_back(value.as<std::string>());
        else if (value.IsSequence())
        {
            for (auto const& element: value)
            {
                // Refused rather than flattened or skipped. A caller applies these
                // through a table of appliers that each take one string, so there is
                // no representation for a nested element -- and dropping it silently
                // would be a setting an operator wrote and nothing read, which is the
                // failure this whole mechanism exists to remove.
                if (!element.IsScalar())
                    return std::unexpected(MakeError(
                        ConfigErrorCode::TypeMismatch, path, key, "sequence elements must be scalars", YamlLine(element)));
                setting.values.push_back(element.as<std::string>());
            }
        }
        else if (!value.IsNull())
            return std::unexpected(MakeError(
                ConfigErrorCode::TypeMismatch, path, key, "expected a scalar or a sequence of scalars", setting.line));

        // A key present with no value (`toolchain:` and nothing after it) carries no
        // values and is kept rather than dropped: the caller decides whether naming a
        // setting and giving it nothing is meaningful, and for a list-valued row it
        // legitimately means "empty this".
        settings.push_back(std::move(setting));
    }
    return settings;
}

} // namespace FastCache
