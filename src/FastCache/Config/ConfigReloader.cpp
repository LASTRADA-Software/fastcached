// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/YamlReader.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

namespace FastCache
{

ConfigReloader::ConfigReloader(Config initial, std::filesystem::path configPath):
    _configPath { std::move(configPath) },
    _current { std::make_shared<Config>(std::move(initial)) }
{
}

ConfigReloader::Snapshot ConfigReloader::Current() const noexcept
{
    std::lock_guard const lock { _swapMutex };
    return _current;
}

void ConfigReloader::Subscribe(Subscriber subscriber)
{
    std::lock_guard const lock { _swapMutex };
    _subscribers.push_back(std::move(subscriber));
}

std::expected<void, ConfigError> ConfigReloader::Reload()
{
    if (_configPath.empty())
        return std::unexpected(ConfigError {
            .code = ConfigErrorCode::FileNotFound,
            .source = {},
            .line = 0,
            .field = {},
            .context = "no config path",
        });

    auto reloaded = ReadYamlConfig(_configPath);
    if (!reloaded.has_value())
        return std::unexpected(reloaded.error());

    Snapshot previous;
    Snapshot next;
    std::vector<Subscriber> observers;
    {
        std::lock_guard const lock { _swapMutex };
        previous = _current;

        auto const validation = ValidateImmutable(*previous, *reloaded);
        if (!validation.has_value())
            return std::unexpected(validation.error());

        next = std::make_shared<Config>(std::move(*reloaded));
        _current = next;
        observers = _subscribers;
    }

    for (auto const& obs: observers)
        obs(previous, next);
    return {};
}

std::expected<void, ConfigError> ConfigReloader::ValidateImmutable(Config const& previous, Config const& candidate)
{
    if (previous.bindAddress != candidate.bindAddress)
        return std::unexpected(ConfigError {
            .code = ConfigErrorCode::ImmutableChanged,
            .source = {},
            .line = 0,
            .field = "bind",
            .context = "cannot change bind address at runtime",
        });
    if (previous.port != candidate.port)
        return std::unexpected(ConfigError {
            .code = ConfigErrorCode::ImmutableChanged,
            .source = {},
            .line = 0,
            .field = "port",
            .context = "cannot change port at runtime",
        });
    return {};
}

} // namespace FastCache
