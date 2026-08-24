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
#include <FastCache/Net/BlockingConnector.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace FastCache::Node
{

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
    /// @param cfg The parsed configuration.
    /// @param membership Decides who may read this tier; must outlive it.
    /// @param clock Time source for the tier's expiry; must outlive the tier.
    /// @param metrics Where hits, misses and upstream outcomes are counted.
    /// @param logger Where to announce the bound address.
    /// @return The running tier, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<CacheTier>, std::string> Start(
        NodeConfig const& cfg,
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

  private:
    CacheTier(std::unique_ptr<BlockingConnector> upstreamConnector,
              std::unique_ptr<IStorage> storage,
              std::unique_ptr<ICacheUpstream> upstream,
              Distributed::IMembershipOracle const& membership,
              IClock& clock,
              IMetricsSink& metrics);

    // Declaration order IS construction order, and every one of these is referenced
    // by the one below it. Reordering them is a dangling reference, which is why
    // they live here rather than as locals somebody has to keep in the right order.
    /// How `RemoteUpstream` dials the shared cache.
    ///
    /// Owned here and declared before `_upstream`, which holds a reference to it.
    /// A `unique_ptr` rather than a value because the upstream is built BEFORE the
    /// tier -- it is a constructor argument -- so the connector has to exist at a
    /// stable address first and be moved in without moving what the reference
    /// names.
    ///
    /// Blocking, because `FrameServer` still serves its connections one at a time
    /// on a thread of its own -- so this blocks that thread and nothing else. It
    /// becomes a `PlatformConnector` when that loop moves onto a reactor.
    std::unique_ptr<BlockingConnector> _upstreamConnector;

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
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    IClock& clock,
    IMetricsSink& metrics,
    ILogger& logger);

} // namespace FastCache::Node
