// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/IAsyncAddressResolver.hpp>
#include <FastCache/Net/IConnector.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
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
    /// @param resolver How the endpoint's host is turned into an address, at most
    ///        once per @p addressRefreshInterval. Injected like every other ambient
    ///        dependency: a cache with a hidden resolver or a hidden clock is
    ///        untestable by construction.
    /// @param clock What ages the held address. Injected for the same reason.
    /// @param addressRefreshInterval How long a resolved address is reused before it
    ///        is looked up again. See `DefaultAddressRefreshInterval` for both
    ///        directions this trades off.
    RemoteUpstream(std::string endpoint,
                   Cc::Credential credential,
                   Cc::CredentialNotice::Sink noticeSink,
                   IConnector& connector,
                   IReactor* reactor,
                   IAsyncAddressResolver& resolver,
                   IClock& clock,
                   std::chrono::milliseconds connectTimeout,
                   std::chrono::milliseconds ioTimeout,
                   std::chrono::milliseconds addressRefreshInterval);

    [[nodiscard]] Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override;
    [[nodiscard]] Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) override;

    /// @copydoc ICacheUpstream::Configured
    ///
    /// True unconditionally: this type exists only because an operator named an
    /// endpoint. Whether that endpoint is currently *reachable* is a different
    /// question, and it is the store counters that answer it.
    [[nodiscard]] bool Configured() const noexcept override
    {
        return true;
    }

  private:
    /// The endpoint text to dial: the held address once one has been resolved,
    /// otherwise the configured endpoint verbatim.
    ///
    /// Refreshed on an INTERVAL and never on a miss. A miss-triggered refresh would
    /// hand a remote peer a free amplifier -- one forced `getaddrinfo` per request,
    /// simply by asking for keys this cache does not have.
    /// @return Endpoint text for `DialEndpoint`.
    [[nodiscard]] Task<std::string> DialTarget();

    std::string _endpoint;

    /// `_endpoint` split once, at construction, rather than per operation. Empty
    /// `_host` means it did not split, and every dial then falls back to `_endpoint`
    /// so the failure stays exactly where it was.
    std::string _host;
    std::uint16_t _port {};

    /// The address last resolved for `_host`, already in endpoint form. Disengaged
    /// until the first SUCCESSFUL lookup.
    std::optional<std::string> _resolved;

    /// When a lookup was last ATTEMPTED, successful or not; disengaged before the
    /// first.
    ///
    /// The attempt rather than the success, and the distinction is the whole guard: a
    /// timer that only advanced on success would leave a node whose resolver is down
    /// looking up once per cache operation -- which is the amplifier the interval
    /// exists to remove, reached by the other door. Caught by
    /// `RemoteUpstream_test.cpp`'s failing-resolver case, which is why it is a
    /// separate member rather than a comment on the old one.
    std::optional<TimePoint> _lastLookupAt;

    Cc::Credential _credential;
    /// Where "your credential went unchecked" is said; the node is a CLIENT here,
    /// and had the same silence the launcher did (#363).
    ///
    /// OWNED, and the sink is what is injected. A reference would have to be
    /// threaded through `CacheTier::Start` and `StartCacheTierOrExplain` purely to
    /// reach here, and neither of them has any other use for it -- so the object
    /// lives with the exchanges it reports on, and only the destination travels.
    Cc::CredentialNotice _notice;
    IConnector& _connector;
    IReactor* _reactor;
    IAsyncAddressResolver& _resolver;
    IClock& _clock;
    std::chrono::milliseconds _connectTimeout;
    std::chrono::milliseconds _ioTimeout;
    std::chrono::milliseconds _addressRefreshInterval;
};

} // namespace FastCache::Node
