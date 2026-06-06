// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <yaml-cpp/yaml.h>

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
        /// `execution_model`: auto | threaded | reactor. `auto` picks
        /// reactor for in-memory and threaded for CoW on-disk storage.
        if (key == "execution_model")
        {
            auto const raw = valueNode.as<std::string>();
            if (raw == "auto")
                cfg.executionModel = ExecutionModel::Auto;
            else if (raw == "threaded")
                cfg.executionModel = ExecutionModel::Threaded;
            else if (raw == "reactor")
                cfg.executionModel = ExecutionModel::Reactor;
            else
                return std::unexpected(MakeError(ConfigErrorCode::OutOfRange,
                                                 path,
                                                 "execution_model",
                                                 std::string { "unknown model (expect auto|threaded|reactor): " } + raw,
                                                 line));
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
        if (auto const result = ApplyEntry(cfg, keyNode.as<std::string>(), valueNode, path, line); !result.has_value())
            return std::unexpected(result.error());
    }

    return cfg;
}

} // namespace FastCache
