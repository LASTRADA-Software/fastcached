// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/YamlReader.hpp>

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

std::expected<Config, ConfigError> ReadYamlConfig(std::filesystem::path const& path)
{
    if (!std::filesystem::exists(path))
        return std::unexpected(MakeError(ConfigErrorCode::FileNotFound, path, {}, "no such file"));

    YAML::Node root;
    try
    {
        root = YAML::LoadFile(path.string());
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

    Config cfg;

    if (!root.IsMap() && !root.IsNull())
        return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, "top-level must be a map"));

    if (root.IsMap())
    {
        for (auto const& kv: root)
        {
            auto const keyNode = kv.first;
            auto const valueNode = kv.second;
            if (!keyNode.IsScalar())
                return std::unexpected(MakeError(ConfigErrorCode::ParseError,
                                                 path,
                                                 {},
                                                 "non-scalar key",
                                                 static_cast<unsigned>(keyNode.Mark().line + 1)));

            auto const key = keyNode.as<std::string>();
            auto const line = static_cast<unsigned>(valueNode.Mark().line + 1);

            if (key == "bind")
            {
                cfg.bindAddress = valueNode.as<std::string>();
            }
            else if (key == "port")
            {
                auto const raw = valueNode.as<int>();
                if (raw <= 0 || raw > 65535)
                    return std::unexpected(
                        MakeError(ConfigErrorCode::OutOfRange, path, "port", "must be in 1..65535", line));
                cfg.port = static_cast<std::uint16_t>(raw);
            }
            else if (key == "max_memory")
            {
                auto const raw = valueNode.as<long long>();
                if (raw < 0)
                    return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, path, "max_memory", "must be >= 0", line));
                cfg.maxMemoryBytes = static_cast<std::size_t>(raw);
            }
            else if (key == "log_level")
            {
                auto const level = ParseLogLevel(valueNode.as<std::string>(), path, line);
                if (!level.has_value())
                    return std::unexpected(level.error());
                cfg.logLevel = *level;
            }
            else
            {
                return std::unexpected(MakeError(ConfigErrorCode::UnknownKey, path, key, "unrecognised key", line));
            }
        }
    }

    return cfg;
}

} // namespace FastCache
