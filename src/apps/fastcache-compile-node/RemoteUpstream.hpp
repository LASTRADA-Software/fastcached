// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"

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
    /// @param ioTimeout Per-operation ceiling. Bounded rather than generous: a node
    ///        waiting on an unreachable cache is a node not compiling, and the
    ///        fallback costs one local build.
    RemoteUpstream(std::string endpoint, Cc::Credential credential, std::chrono::milliseconds ioTimeout) noexcept;

    [[nodiscard]] std::optional<std::vector<std::byte>> Fetch(std::string_view key) override;
    [[nodiscard]] bool Store(std::string_view key, std::span<std::byte const> value) override;

  private:
    std::string _endpoint;
    Cc::Credential _credential;
    std::chrono::milliseconds _ioTimeout;
};

} // namespace FastCache::Node
