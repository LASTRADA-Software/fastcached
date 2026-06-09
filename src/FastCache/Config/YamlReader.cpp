// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <yaml-cpp/yaml.h>

#include <exception>
#include <expected>
#include <filesystem>
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

    /// Parse one entry of a `listeners:` YAML sequence into a `BindConfig`.
    /// Required fields: `address` (string), `port` (1..65535). Optional: `tls`
    /// (boolean, defaults to false). Any other key is rejected so a typo
    /// (`tsl: true`) fails fast instead of silently disabling TLS.
    /// @param node The YAML node for one listener entry (must be a Map).
    /// @param path Source YAML file for error reporting.
    /// @param idx  Zero-based index within the `listeners:` sequence.
    /// @return The populated BindConfig, or ConfigError on malformed input.
    /// One-based YAML source line for an error message. yaml-cpp's `Mark::line`
    /// is zero-based and signed; this normalises to the human convention.
    [[nodiscard]] unsigned YamlLine(YAML::Node const& node) noexcept
    {
        auto const raw = node.Mark().line;
        return raw < 0 ? 0U : static_cast<unsigned>(raw) + 1U;
    }

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
                return std::unexpected(MakeError(
                    ConfigErrorCode::ParseError, path, std::move(unknownField), "unknown listener field", line));
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
        for (std::size_t i = 0; i < valueNode.size(); ++i)
        {
            auto entry = ParseListenerEntry(valueNode[i], path, i);
            if (!entry.has_value())
                return std::unexpected(std::move(entry).error());
            cfg.binds.push_back(*entry);
        }
        return {};
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
        /// `storage_path`: filesystem path of the CoW-tree backing file.
        if (key == "storage_path")
        {
            cfg.storagePath = valueNode.as<std::string>();
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
        /// `tls_cert`: PEM certificate (chain) file for TLS.
        if (key == "tls_cert")
        {
            cfg.tlsCertPath = valueNode.as<std::string>();
            return {};
        }
        /// `tls_key`: PEM private key file for TLS.
        if (key == "tls_key")
        {
            cfg.tlsKeyPath = valueNode.as<std::string>();
            return {};
        }
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
    }

    return out;
}

} // namespace FastCache
