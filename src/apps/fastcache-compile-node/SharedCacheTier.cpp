// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "SharedCacheTier.hpp"
#include "SharedTierProfile.hpp"

#include <FastCache/Core/Compression.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

SharedCacheTier::SharedCacheTier(std::unique_ptr<IStorage> storage, core::platform::IClock& clock, IMetricsSink& metrics):
    _storage { std::move(storage) },
    _cache { *_storage, _upstream, clock, metrics, SharedTierProfile },
    _proxy { _cache, metrics }
{
}

core::async::Task<std::vector<std::byte>> SharedCacheTier::Answer(std::span<std::byte const> frame)
{
    co_return co_await _proxy.Answer(frame);
}

core::async::Task<std::optional<std::vector<std::byte>>> SharedCacheTier::Fetch(std::string_view key)
{
    co_return co_await _cache.Fetch(key);
}

core::async::Task<bool> SharedCacheTier::Store(std::string_view key, std::span<std::byte const> value)
{
    co_return co_await _cache.Store(key, value);
}

TieredStorageStats SharedCacheTier::SnapshotTiers() const noexcept
{
    return _storage->SnapshotTiers();
}

DiskSharedTierOpener::DiskSharedTierOpener(NodeConfig const& cfg,
                                           core::platform::IClock& clock,
                                           IMetricsSink& metrics,
                                           ILogger& logger) noexcept:
    _cfg { cfg },
    _clock { clock },
    _metrics { metrics },
    _logger { logger }
{
}

std::expected<std::shared_ptr<SharedCacheTier>, std::string> DiskSharedTierOpener::Open()
{
    auto opened = OpenDiskStore(
        DiskStoreSpec {
            .path = SharedCacheStorePath(_cfg),
            .maxBytes = _cfg.sharedCacheDiskBytes,
            .codec = Compression::EffectiveCodec(_cfg.compression),
            .level = _cfg.compressionLevel,
            .minBytes = _cfg.compressionMinBytes,
            // Not only "another process": a --cache-dir naming this very directory is this node's own
            // private tier holding the file.
            .inUse = "something already has this cache open -- another process, or this node's own --cache-dir if it "
                     "names <state-dir>/shared-cache. A state directory belongs to one node, and its shared-cache "
                     "directory to the shared tier alone.",
        },
        _logger);
    if (!opened.has_value())
        return std::unexpected { std::move(opened.error()) };
    return std::make_shared<SharedCacheTier>(ShareStorage(std::move(*opened), _logger), _clock, _metrics);
}

} // namespace FastCache::Node
