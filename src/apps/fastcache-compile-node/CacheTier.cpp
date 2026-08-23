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
} // namespace

CacheTier::CacheTier(std::unique_ptr<IStorage> storage,
                     std::unique_ptr<ICacheUpstream> upstream,
                     IClock& clock,
                     IMetricsSink& metrics):
    _storage { std::move(storage) },
    _upstream { std::move(upstream) },
    _cache { *_storage, *_upstream, clock, metrics },
    _proxy { _cache },
    _responder { _proxy }
{
}

std::expected<std::unique_ptr<CacheTier>, std::string> CacheTier::Start(NodeConfig const& cfg,
                                                                        IClock& clock,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger)
{
    // An absent upstream is a named type rather than a null pointer: one developer's
    // machine has no shared cache, and that configuration should not cost every call
    // site a branch.
    std::unique_ptr<ICacheUpstream> upstream;
    if (cfg.upstream.empty())
        upstream = std::make_unique<NoUpstream>();
    else
        upstream = std::make_unique<RemoteUpstream>(
            cfg.upstream, Cc::Credential { .username = {}, .secret = cfg.token }, UpstreamIoTimeout);

    auto tier = std::unique_ptr<CacheTier> { new CacheTier {
        std::make_unique<InMemoryLruStorage>(static_cast<std::size_t>(cfg.cacheMemoryBytes)),
        std::move(upstream),
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

} // namespace FastCache::Node
