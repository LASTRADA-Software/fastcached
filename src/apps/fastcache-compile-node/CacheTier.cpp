// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "RemoteUpstream.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Config/ByteSize.hpp>

#include <chrono>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// Per-operation ceiling on talking to the shared cache.
    ///
    /// Bounded rather than generous, and short: a node waiting on an unreachable
    /// shared cache is a node not compiling, and the fall-back costs one local build.
    constexpr std::chrono::milliseconds UpstreamIoTimeout { 5'000 };

    /// Ceiling on OPENING the upstream connection, name resolution included.
    ///
    /// Separate from the I/O ceiling above, and much shorter. The two used to be
    /// one value passed twice, which gave the dial a five-second
    /// resolve-plus-connect budget nobody chose: five seconds is a sensible
    /// per-operation bound and a very long time to wait for a TCP handshake.
    constexpr std::chrono::milliseconds UpstreamConnectTimeout { 1'000 };
} // namespace

CacheTier::CacheTier(std::unique_ptr<BlockingConnector> upstreamConnector,
                     std::unique_ptr<IStorage> storage,
                     std::unique_ptr<ICacheUpstream> upstream,
                     Distributed::IMembershipOracle const& membership,
                     IClock& clock,
                     IMetricsSink& metrics):
    _upstreamConnector { std::move(upstreamConnector) },
    _storage { std::move(storage) },
    _upstream { std::move(upstream) },
    _cache { *_storage, *_upstream, clock, metrics },
    _proxy { _cache },
    _responder { _proxy, membership }
{
}

std::expected<std::unique_ptr<CacheTier>, std::string> CacheTier::Start(NodeConfig const& cfg,
                                                                        Distributed::IMembershipOracle const& membership,
                                                                        IClock& clock,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger)
{
    // An absent upstream is a named type rather than a null pointer: one developer's
    // machine has no shared cache, and that configuration should not cost every call
    // site a branch.
    // Created before the upstream that references it, and at a stable address, so
    // moving it into the tier below does not move what that reference names.
    auto upstreamConnector = std::make_unique<BlockingConnector>(
        DefaultAddressResolver(), BlockingConnectorOptions { .ioTimeout = UpstreamIoTimeout });

    std::unique_ptr<ICacheUpstream> upstream;
    if (cfg.upstream.empty())
        upstream = std::make_unique<NoUpstream>();
    else
        // A BLOCKING connector and no reactor, for now. `FrameServer` still
        // serves its connections one at a time on a thread of its own, so this
        // dial blocks that thread and nothing else -- which is the serialization
        // defect this tier still has, not one this change introduces. Moving it
        // onto a reactor is what `NodeIoLoop` is for, and at that point this
        // becomes a `PlatformConnector` and the reactor stops being null.
        upstream = std::make_unique<RemoteUpstream>(cfg.upstream,
                                                    Cc::Credential { .username = {}, .secret = cfg.token },
                                                    *upstreamConnector,
                                                    /*reactor*/ nullptr,
                                                    UpstreamConnectTimeout,
                                                    UpstreamIoTimeout);

    auto tier = std::unique_ptr<CacheTier> { new CacheTier {
        std::move(upstreamConnector),
        std::make_unique<InMemoryLruStorage>(static_cast<std::size_t>(cfg.cacheMemoryBytes)),
        std::move(upstream),
        membership,
        clock,
        metrics } };

    // Loopback for a bare port, the OPPOSITE of the scheduler's wildcard: this is the
    // surface `fastcache-cc` on this machine talks to, and a node's private cache
    // reachable from the network is a decision rather than something an operator gets
    // by typing a port.
    auto started = FrameEndpoint::Start(cfg.cacheListen, "127.0.0.1", tier->_responder, "cache", logger);
    if (!started.has_value())
        return std::unexpected { started.error() };

    tier->_endpoint = std::move(*started);
    logger.Logf(LogLevel::Info,
                "local cache on {} ({}, upstream {})",
                tier->BoundEndpoint(),
                FormatByteSize(static_cast<std::size_t>(cfg.cacheMemoryBytes)),
                cfg.upstream.empty() ? std::string { "none" } : cfg.upstream);
    return tier;
}

std::expected<std::unique_ptr<CacheTier>, std::string> StartCacheTierOrExplain(
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    IClock& clock,
    IMetricsSink& metrics,
    ILogger& logger)
{
    // Emptied deliberately. A node that only compiles for others wants no cache port
    // at all, and saying so must not be an error.
    if (cfg.cacheListen.empty())
        return std::unique_ptr<CacheTier> {};

    auto started = CacheTier::Start(cfg, membership, clock, metrics, logger);
    if (started.has_value())
        return std::move(*started);

    // Fatal only when the operator NAMED this address -- the same distinction the
    // admin endpoint draws between an endpoint asked for and one got anyway. It
    // matters because this port is on by default: a node sharing a machine with
    // `fastcached` would otherwise refuse to start over a convenience nobody
    // requested. Typed, it is a promise, and a broken promise is fatal.
    if (cfg.cacheListen != NodeConfig {}.cacheListen)
        return std::unexpected { started.error() };

    // Never silent. The launcher will reach whatever else holds that port -- very
    // likely the daemon -- so the build still works, but "the cache quietly did less
    // than you configured" is the failure mode this codebase keeps a list about.
    logger.Logf(LogLevel::Warn,
                "default cache endpoint {}: {}; continuing without a local cache tier",
                cfg.cacheListen,
                started.error());
    return std::unique_ptr<CacheTier> {};
}

} // namespace FastCache::Node
