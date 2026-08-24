// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Net/IConnector.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include <CacheProtocol.hpp>

namespace FastCache::Node
{

/// The shared `fastcached`, reached over the `0xFC` wire.
///
/// Built on the launcher's own `CacheFetch`/`CacheStore` rather than a second
/// client, for the reason `_fc_node_shared` exists at all: these are the two ends
/// of one protocol, and a second implementation of either is how they drift apart.
///
/// ## Every failure is a miss, and that is the contract rather than laziness
///
/// `ICacheUpstream` promises that an unreachable shared cache is indistinguishable
/// from one that does not hold the key, and this is where that promise is kept: a
/// refused connection, a timeout, a typed refusal and a genuine miss all return
/// `nullopt`. The caller compiles in every one of those cases, so a client that
/// could tell them apart would have nothing to do with the distinction — and giving
/// a build a way to *fail* because a cache was down is the one thing this whole
/// subsystem must never do.
///
/// A connection per operation, deliberately: it is what the launcher already does,
/// and a node holding a pooled connection to the shared cache would have to decide
/// what to do when it goes stale — which is a state machine bought for a cost that
/// has already been measured as irrelevant next to a compile.
class RemoteUpstream final: public ICacheUpstream
{
  public:
    /// @param endpoint `host:port` of the shared cache.
    /// @param credential Presented on every operation; empty when none is configured.
    /// @param connector How to dial. Injected, and reactor-driven in production:
    ///        this runs inside the node's cache endpoint, so a blocking dial here
    ///        would stall every other connection sharing that loop -- which is
    ///        precisely the defect this class used to cause.
    /// @param connectTimeout Ceiling on the dial, resolution included. Separate
    ///        from `ioTimeout` because they bound different things and neither
    ///        implies the other; collapsing them gave this a five-second
    ///        resolve-plus-connect budget nobody chose.
    /// @param reactor Where the per-operation deadline is armed, or **nullptr**
    ///        when the connector is a blocking one -- the same nullable-reactor
    ///        convention `SleepUntil` and `IAsyncAddressResolver` use. With a
    ///        blocking connector the socket's own `SO_RCVTIMEO` is the bound and
    ///        arming a timer would be a second mechanism for one job; with a
    ///        reactor connector that option is inert and the timer is the only
    ///        thing that bounds anything.
    /// @param ioTimeout Per-operation ceiling. Bounded rather than generous: a node
    ///        waiting on an unreachable cache is a node not compiling, and the
    ///        fallback costs one local build.
    ///
    ///        Armed as a `DeadlineTimer` that CLOSES the socket, rather than as
    ///        `SO_RCVTIMEO` which is what it used to be. That is strictly more
    ///        than the socket option gave: the option bounds a single call, so a
    ///        peer dribbling one byte at a time could still take forever, while
    ///        this bounds the whole exchange. It is also the only thing that
    ///        works at all on a reactor socket, whose reads suspend rather than
    ///        block.
    RemoteUpstream(std::string endpoint,
                   Cc::Credential credential,
                   IConnector& connector,
                   IReactor* reactor,
                   std::chrono::milliseconds connectTimeout,
                   std::chrono::milliseconds ioTimeout) noexcept;

    [[nodiscard]] Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override;
    [[nodiscard]] Task<bool> Store(std::string_view key, std::span<std::byte const> value) override;

  private:
    std::string _endpoint;
    Cc::Credential _credential;
    IConnector& _connector;
    IReactor* _reactor;
    std::chrono::milliseconds _connectTimeout;
    std::chrono::milliseconds _ioTimeout;
};

} // namespace FastCache::Node
