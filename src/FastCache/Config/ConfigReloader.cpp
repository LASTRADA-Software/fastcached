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
    std::scoped_lock const lock { _swapMutex };
    return _current;
}

void ConfigReloader::Subscribe(Subscriber subscriber)
{
    std::scoped_lock const lock { _swapMutex };
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
        std::scoped_lock const lock { _swapMutex };
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
    // Fields enforced as immutable: the live wiring (listener address,
    // backend type, shard layout, execution model, durability mode,
    // value-size cap) is baked into objects constructed at startup. A
    // SIGHUP that swaps Config but leaves the backend untouched would
    // otherwise silently disagree with `reloader.Current()`.
    auto const reject = [](std::string field, std::string context) {
        return std::unexpected(ConfigError {
            .code = ConfigErrorCode::ImmutableChanged,
            .source = {},
            .line = 0,
            .field = std::move(field),
            .context = std::move(context),
        });
    };
    if (previous.bindAddress != candidate.bindAddress)
        return reject("bind", "cannot change bind address at runtime");
    if (previous.port != candidate.port)
        return reject("port", "cannot change port at runtime");
    // The explicit-listeners vector is just as live-wired as the legacy
    // single-bind triplet: main.cpp constructs the listener pool from
    // `serverOpts.binds` once at startup, and there is no live-rebuild
    // path. A SIGHUP that adds, removes, or swaps a `listeners:` entry
    // must therefore reject — silently accepting it would leave
    // reloader.Current() reporting a Config whose `binds` no longer
    // matches the kernel-bound sockets (split-brain).
    if (previous.binds != candidate.binds)
        return reject("listeners", "cannot change listeners at runtime");
    if (previous.storagePath != candidate.storagePath)
        return reject("storage", "cannot change storage path at runtime");
    if (previous.storageShards != candidate.storageShards)
        return reject("storage-shards", "cannot change shard count at runtime");
    if (previous.storageDurability != candidate.storageDurability)
        return reject("storage-durability", "cannot change durability mode at runtime");
    if (previous.storageMaxValueBytes != candidate.storageMaxValueBytes)
        return reject("storage-max-value", "cannot change value-size cap at runtime");
    if (previous.workerThreads != candidate.workerThreads)
        return reject("threads", "cannot change worker-thread count at runtime");
    return {};
}

} // namespace FastCache
