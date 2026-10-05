// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "LocalCache.hpp"
#include "NodeConfig.hpp"

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// The fleet's shared tier: one store, read through to nothing, answering the fleet cache verbs.
///
/// Storage, `NoUpstream`, a `LocalCache` built with `SharedTierProfile` and a `CacheProxy` over
/// it -- so a STORE here is canonicalized by the same `CacheProxy` every other server of the wire
/// runs, a stored value reads back the way it does from the private tier, and every outcome moves
/// the `NodeSharedCache*` series and never the private tier's.
///
/// **The upstream is `NoUpstream` by TYPE**, a member rather than a parameter. The shared tier is
/// the top of the fleet's cache, so reading through again would be a loop, and `SharedTierProfile`
/// names no upstream counter: a real upstream here would read and write another store while
/// moving nothing an operator reads. `LocalCache` refuses that pairing as well
/// (`ReadsThroughCountedly`), so a second tier built this way cannot reach it either.
///
/// Neither copied nor moved: the cache and the proxy hold references into this object.
class SharedCacheTier
{
  public:
    /// @param storage The store; the tier owns it, and it is closed when the tier is destroyed.
    /// @param clock What a stored entry's time is read from; must outlive this.
    /// @param metrics Where the tier's outcomes and refusals are counted; must outlive this.
    SharedCacheTier(std::unique_ptr<IStorage> storage, core::platform::IClock& clock, IMetricsSink& metrics);

    SharedCacheTier(SharedCacheTier const&) = delete;
    SharedCacheTier& operator=(SharedCacheTier const&) = delete;
    SharedCacheTier(SharedCacheTier&&) = delete;
    SharedCacheTier& operator=(SharedCacheTier&&) = delete;
    ~SharedCacheTier() = default;

    /// Answer one complete request frame.
    /// @param frame Header plus payload, exactly as received.
    /// @return The encoded reply; empty only when the peer is not speaking this protocol.
    [[nodiscard]] core::async::Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame);

    /// Read one object as this machine reads it: through the tier's own cache, so every outcome moves
    /// the fleet's series exactly as another machine's read of the same object does -- but with no
    /// frame, because the reader is this process.
    /// @param key The object key.
    /// @return The stored value, as the tier holds it -- canonical; nullopt on a miss.
    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key);

    /// Store one object as this machine stores it: through the tier's own cache, with no frame.
    ///
    /// **@p value must already be canonical**, which is `InProcessSharedUpstream`'s to guarantee: the
    /// private tier's `CacheProxy` canonicalized it at the STORE that reached this node, as it does
    /// for every value `RemoteUpstream` forwards. `Answer` canonicalizes a frame's value itself; this
    /// takes no roots and so cannot.
    /// @param key The object key.
    /// @param value The encoded, canonical compile value.
    /// @return Whether the tier kept it.
    [[nodiscard]] core::async::Task<bool> Store(std::string_view key, std::span<std::byte const> value);

    /// The store's own figures, per tier.
    /// @return What the store reports.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept;

  private:
    // Declaration order IS construction order: the cache borrows the storage and the upstream,
    // and the proxy borrows the cache.
    std::unique_ptr<IStorage> _storage;
    NoUpstream _upstream;
    LocalCache _cache;
    CacheProxy _proxy;
};

/// Opens the shared tier this node serves while the fleet's `shared-cache` setting names it.
///
/// A seam rather than a call, because opening a disk store claims a file and may walk it: the
/// host's reconciliation is tested over memory, and only this seam knows about files.
class ISharedTierOpener
{
  public:
    virtual ~ISharedTierOpener() = default;

    ISharedTierOpener() = default;
    ISharedTierOpener(ISharedTierOpener const&) = delete;
    ISharedTierOpener& operator=(ISharedTierOpener const&) = delete;
    ISharedTierOpener(ISharedTierOpener&&) = delete;
    ISharedTierOpener& operator=(ISharedTierOpener&&) = delete;

    /// Open the tier.
    /// @return The tier, or a sentence saying why it will not open -- what the condition reports.
    [[nodiscard]] virtual std::expected<std::shared_ptr<SharedCacheTier>, std::string> Open() = 0;
};

/// Opens the shared tier on disk, at `SharedCacheStorePath`, holding at most
/// `NodeConfig::sharedCacheDiskBytes`.
///
/// Through `OpenDiskStore` and `ShareStorage`, the private tier's own two steps, so both tiers
/// create their directory, refuse a store another process holds and report a filesystem that
/// cannot lock in one way.
class DiskSharedTierOpener final: public ISharedTierOpener
{
  public:
    /// @param cfg The configuration; must outlive this.
    /// @param clock Handed to every tier this opens; must outlive this.
    /// @param metrics Handed to every tier this opens; must outlive this.
    /// @param logger Where an unenforceable claim on the store is reported; must outlive this.
    DiskSharedTierOpener(NodeConfig const& cfg,
                         core::platform::IClock& clock,
                         IMetricsSink& metrics,
                         ILogger& logger) noexcept;

    /// @copydoc ISharedTierOpener::Open
    [[nodiscard]] std::expected<std::shared_ptr<SharedCacheTier>, std::string> Open() override;

  private:
    NodeConfig const& _cfg;
    core::platform::IClock& _clock;
    IMetricsSink& _metrics;
    ILogger& _logger;
};

} // namespace FastCache::Node
