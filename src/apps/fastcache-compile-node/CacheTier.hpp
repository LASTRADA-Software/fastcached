// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"
#include "LocalCache.hpp"
#include "NodeConfig.hpp"
#include "Responders.hpp"

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace FastCache::Node
{

class NodeIoLoop;

/// The node's cache surface: storage, upstream, read-through logic, protocol and
/// listener, owned as one thing.
///
/// A class rather than six locals in `WorkerBody`, for two reasons and the second is
/// the one that matters. They form a *reference chain* -- the endpoint holds the
/// responder, which holds the proxy, which holds the cache, which holds the storage
/// and the upstream -- so their declaration order in a function body is load-bearing
/// and silently so; getting it wrong is a dangling reference rather than a compile
/// error. Here the order is the member order, which is checked by the language.
///
/// And `WorkerBody` reached a cognitive complexity of 67 against clang-tidy's limit
/// of 60 when this was inline. That number is a symptom worth listening to rather
/// than a rule to argue with: it went over because a coherent decision with one
/// answer -- "does this node cache, and where" -- had been spread across a
/// function that already had several.
class CacheTier
{
  public:
    /// Build the tier and start serving it.
    ///
    /// The error is a diagnostic string rather than one of the project's error enums,
    /// the same departure `AdminEndpoint::Start` documents and for the same reason:
    /// this fails in ways belonging to two taxonomies and the caller's response is
    /// identical either way.
    /// The storage arrives already built, rather than being assembled in here, and
    /// that is what keeps two unequal failures apart. A store that will not open is
    /// always fatal; a port that will not bind is fatal only when the operator typed
    /// the address. Both reduce to a diagnostic string, so building the store inside
    /// this function would leave `StartCacheTierOrExplain` unable to tell which it
    /// was holding — and a bad `--cache-dir` on a node using the default cache port
    /// would be logged as a warning and stepped over.
    /// @param cfg The parsed configuration.
    /// @param storage Where this tier keeps objects; already opened.
    /// @param membership Decides who may read this tier; must outlive it.
    /// @param clock Time source for the tier's expiry; must outlive the tier.
    /// @param metrics Where hits, misses and upstream outcomes are counted.
    /// @param logger Where to announce the bound address.
    /// @return The running tier, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<CacheTier>, std::string> Start(
        NodeIoLoop& io,
        NodeConfig const& cfg,
        std::unique_ptr<IStorage> storage,
        Distributed::IMembershipOracle const& membership,
        IClock& clock,
        IMetricsSink& metrics,
        ILogger& logger);

    ~CacheTier() = default;

    CacheTier(CacheTier const&) = delete;
    CacheTier& operator=(CacheTier const&) = delete;
    CacheTier(CacheTier&&) = delete;
    CacheTier& operator=(CacheTier&&) = delete;

    /// The address the cache surface bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _endpoint->BoundEndpoint();
    }

    /// What this node's cache holds, as one figure.
    ///
    /// Safe from any thread, which is the whole reason the storage below is behind
    /// a `ShardedStorage`: the tier is mutated on the reactor thread while the
    /// heartbeat thread and the `/metrics` scrape read it.
    /// @return The cache's own statistics.
    [[nodiscard]] StorageStats Snapshot() const noexcept
    {
        return _storage->Snapshot();
    }

    /// The same, kept apart by the tier holding each number.
    ///
    /// What `Snapshot()` cannot say when both halves are configured: the composite
    /// reports the on-disk store's item count, bytes and budget, so the in-memory
    /// half above it leaves no trace. See `IStorage::SnapshotTiers` for why these
    /// are per-tier answers rather than a total waiting to be summed.
    /// @return One entry per tier this node configured.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept
    {
        return _storage->SnapshotTiers();
    }

  private:
    CacheTier(std::unique_ptr<IStorage> storage,
              std::unique_ptr<ICacheUpstream> upstream,
              Distributed::IMembershipOracle const& membership,
              IClock& clock,
              IMetricsSink& metrics);

    // Declaration order IS construction order, and every one of these is referenced
    // by the one below it. Reordering them is a dangling reference, which is why
    // they live here rather than as locals somebody has to keep in the right order.
    std::unique_ptr<IStorage> _storage;
    std::unique_ptr<ICacheUpstream> _upstream;
    LocalCache _cache;
    CacheProxy _proxy;
    CacheResponder _responder;
    std::unique_ptr<FrameEndpoint> _endpoint;
};

/// Start the node's cache tier, or explain why the node must not start at all.
///
/// Three outcomes, which is why the return type looks the way it does:
///
///   - **A tier** — it is serving.
///   - **Success carrying nothing** — there is deliberately no tier, either because
///     `--listen-cache` was emptied or because its *default* address was taken. The
///     node continues; a warning has already been logged.
///   - **An error** — the operator NAMED an address and it could not be served, so
///     startup must stop.
///
/// That middle state is the whole reason this is a function rather than four lines
/// in `WorkerBody`. The fatal/tolerable rule turns on whether the operator typed the
/// address, which is a judgement worth stating once and testing, and `main.cpp` is
/// in no test target — the lesson `CacheProtocol.cpp`, `RootReconciler.cpp` and
/// `AdminEndpoint.cpp` were each extracted for. It also kept `WorkerBody` under
/// clang-tidy's cognitive-complexity limit, which is the symptom that said so.
/// @param cfg The parsed configuration.
/// @param membership Decides who may read the tier; must outlive it.
/// @param clock Time source for the tier's expiry; must outlive it.
/// @param metrics Where hits, misses and upstream outcomes are counted.
/// @param logger Where the bound address, or the tolerated failure, is announced.
/// @return The tier, a null tier meaning "carry on without one", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<CacheTier>, std::string> StartCacheTierOrExplain(
    NodeIoLoop& io,
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    IClock& clock,
    IMetricsSink& metrics,
    ILogger& logger);

} // namespace FastCache::Node
