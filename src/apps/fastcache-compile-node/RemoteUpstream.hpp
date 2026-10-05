// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"
#include "NodeCredential.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <CacheProtocol.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// The four bounds a `RemoteUpstream` works to, named where it is built.
///
/// A struct rather than four positional `std::chrono::milliseconds`: they share one type, so a
/// transposition compiles and runs -- and the newest, `unreachableRetryInterval`, sits beside
/// `addressRefreshInterval`, the pair most likely to be swapped and the least likely to be
/// noticed, since both are intervals of seconds.
struct UpstreamTimings
{
    std::chrono::milliseconds connectTimeout;         ///< Ceiling on the dial, resolution included.
    std::chrono::milliseconds ioTimeout;              ///< Ceiling on one whole exchange.
    std::chrono::milliseconds addressRefreshInterval; ///< How long a resolved address is reused.
    /// How long a failed EXCHANGE is believed: a dial that did not connect, or a connection that
    /// reached the I/O ceiling or lost its peer before it was answered.
    std::chrono::milliseconds unreachableRetryInterval;
};

/// Whether the shared cache answered the last time this node asked it -- a failed EXCHANGE
/// remembered for an INTERVAL, so an unreachable or stalling upstream costs one exchange per
/// interval rather than one per miss.
///
/// ## Safety was decided first, and it is what licenses remembering at all
///
/// `AGENT.md`'s *Caching an expensive repeated answer*: the question is what a STALE answer does,
/// not what the probe costs.
///
/// - **Stale UNREACHABLE** -- the cache came back inside the interval. A local miss it could have
///   answered is answered as a miss and compiled locally, and a store is not offered upstream.
///   That fails CLOSED (it is the answer an unreachable upstream already produced) and self-heals
///   at the next probe, at most one interval later.
/// - **Stale REACHABLE** -- the cache went away since the last exchange that was answered. Every
///   operation that passed `ShouldDial` before the first failure LANDS is already dialling, and
///   each of them pays its own ceiling -- the connect ceiling where the dial fails, the connect
///   and I/O ceilings where the peer accepts and stalls. On a reactor that is every miss in flight
///   at that moment, not one. Only operations arriving after the first failure lands are spared.
///   Each operation still pays at most what EVERY operation paid before this existed, so this
///   direction is no worse than no memo at all.
///
/// Neither direction produces a wrong answer that looks right: every outcome this changes was
/// already a miss or a declined store.
///
/// ## An exchange that never completed counts, exactly like a dial that failed
///
/// A connection that opened and then timed out or lost its peer -- `CacheOutcomeKind::Transport` --
/// is remembered as a failed dial is. It is the COSTLIER failure: it spends up to the per-operation
/// I/O ceiling, not a connect ceiling. Both staleness directions stay safe:
/// - a peer that recovered costs a MISS for at most one interval;
/// - a peer that stalls again costs one wasted exchange per interval rather than one per miss.
///
/// ## Refreshed on an INTERVAL, never on a miss
///
/// Only whether the EXCHANGE completed is evidence. A `Miss` or a `Rejected` from a cache that
/// answered says the cache is up, and no answer moves this toward unreachable -- so no request
/// pattern can make it probe more than once per interval, which is the amplifier
/// `RemoteUpstream::DialTarget`'s own comment rules out.
///
/// ## The probe is stamped BEFORE it runs
///
/// `ShouldDial` moves the next-probe instant the moment it grants a probe, not when the probe
/// fails. A dial suspends on the reactor for up to the connect ceiling, and every miss arriving
/// meanwhile would otherwise find the window expired and dial too -- a burst of exactly the dials
/// this exists to remove, at exactly the moment the cache is least likely to answer.
///
/// That keeps probes from overlapping only while a probe finishes inside one interval, which is
/// why `CacheTier.cpp` asserts the interval above the connect and I/O ceilings together. It is a
/// floor rather than a guarantee: `RemoteUpstream::DialTarget`'s address lookup is bounded by
/// neither ceiling, so a probe whose lookup hangs can still outlast an interval.
///
/// ## One thread
///
/// Read and written only on the node's I/O reactor, which is one thread (`NodeIoLoop`), as
/// `RemoteUpstream::_lastLookupAt` is -- so it takes no lock.
class UpstreamReachability
{
  public:
    /// @param clock What ages a failed exchange. Injected: a memo with a hidden clock is untestable.
    /// @param retryInterval How long a failed exchange is believed before the next probe.
    UpstreamReachability(core::platform::IClock const& clock, std::chrono::milliseconds retryInterval) noexcept;

    /// Whether an operation should dial now.
    ///
    /// True while nothing has failed, or once a failure's interval has run out -- and in that
    /// second case the next probe is stamped before returning, so only one operation probes.
    /// @return True to dial; false to answer as a miss (or a declined store) without dialling.
    [[nodiscard]] bool ShouldDial();

    /// An exchange completed (any answer, a `Miss` included): the upstream is reachable, and every
    /// operation dials again.
    void Answered() noexcept;

    /// A dial failed, or a connected exchange never completed: believe it for one retry interval.
    void Unanswered();

  private:
    core::platform::IClock const& _clock;
    std::chrono::milliseconds _retryInterval;
    /// When the next probe may dial; disengaged while the upstream is believed reachable.
    std::optional<core::platform::SteadyTimePoint> _nextProbeAt;
};

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
    /// @param credential Asked on every operation for what to present. A SEAM rather
    ///        than a value, and threaded through `CacheTier::Start` and
    ///        `StartCacheTierOrExplain` to reach here -- which the notice sink next
    ///        door is deliberately NOT, and the difference is what the two answer.
    ///        A notice destination is fixed for this process's life, so owning a copy
    ///        of it costs nothing; a credential is the thing an operator rotates, so
    ///        a copy is a value that goes stale in silence and keeps presenting a
    ///        secret the shared cache has stopped accepting (#404). Borrowed; must
    ///        outlive this object.
    /// @param connector How to dial. Injected, and reactor-driven in production:
    ///        this runs inside the node's cache endpoint, so a blocking dial here
    ///        would stall every other connection sharing that loop -- which is
    ///        precisely the defect this class used to cause.
    /// @param reactor Where the per-operation deadline is armed, or **nullptr**
    ///        when the connector is a blocking one -- the same nullable-reactor
    ///        convention `SleepUntil` and `core::net::IAsyncAddressResolver` use. With a
    ///        blocking connector the socket's own `SO_RCVTIMEO` is the bound and
    ///        arming a timer would be a second mechanism for one job; with a
    ///        reactor connector that option is inert and the timer is the only
    ///        thing that bounds anything.
    /// @param resolver How the endpoint's host is turned into an address, at most
    ///        once per `addressRefreshInterval`. Injected like every other ambient
    ///        dependency: a cache with a hidden resolver or a hidden clock is
    ///        untestable by construction.
    /// @param clock What ages the held address and a failed exchange. Injected for
    ///        the same reason.
    /// @param timings The bounds this works to; see UpstreamTimings.
    ///
    ///        connectTimeout: ceiling on the dial, resolution included. Separate
    ///        from `ioTimeout` because they bound different things and neither
    ///        implies the other; collapsing them gave this a five-second
    ///        resolve-plus-connect budget nobody chose.
    ///
    ///        ioTimeout: per-operation ceiling. Bounded rather than generous: a node
    ///        waiting on an unreachable cache is a node not compiling, and the
    ///        fallback costs one local build. Armed as a `core::net::DeadlineTimer`
    ///        that CLOSES the socket, rather than as `SO_RCVTIMEO` which is what it
    ///        used to be. That is strictly more than the socket option gave: the
    ///        option bounds a single call, so a peer dribbling one byte at a time
    ///        could still take forever, while this bounds the whole exchange. It is
    ///        also the only thing that works at all on a reactor socket, whose reads
    ///        suspend rather than block.
    ///
    ///        addressRefreshInterval: how long a resolved address is reused before it
    ///        is looked up again. See `UpstreamAddressRefreshInterval` in
    ///        `CacheTier.cpp` for both directions this trades off.
    ///
    ///        unreachableRetryInterval: how long a failed exchange is believed -- a
    ///        dial that did not connect, or a connection that reached `ioTimeout` or
    ///        lost its peer before it was answered; see UpstreamReachability for
    ///        both staleness directions.
    RemoteUpstream(std::string endpoint,
                   ICredentialSource const& credential,
                   Cc::CredentialNotice::Sink noticeSink,
                   core::net::IConnector& connector,
                   core::net::EventLoop* reactor,
                   core::net::IAsyncAddressResolver& resolver,
                   core::platform::IClock& clock,
                   UpstreamTimings timings);

    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override;
    [[nodiscard]] core::async::Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) override;

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
    [[nodiscard]] core::async::Task<std::string> DialTarget();

    /// Tell the memo how an exchange that CONNECTED ended.
    ///
    /// `Transport` -- a stall the deadline closed, or a peer that went away -- is remembered as a
    /// failed dial is; any answer at all, a `Miss` and a `Rejected` included, says the cache is up.
    /// @param kind How the exchange ended.
    void RecordExchange(Cc::CacheOutcomeKind kind);

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
    std::optional<core::platform::SteadyTimePoint> _lastLookupAt;

    /// Asked once per operation, never copied into a member. The reference IS the
    /// guard: there is no field here for a stale secret to sit in.
    ICredentialSource const& _credential;

    /// Where "your credential went unchecked" is said; the node is a CLIENT here,
    /// and had the same silence the launcher did (#363).
    ///
    /// OWNED, and the sink is what is injected. A reference would have to be
    /// threaded through `CacheTier::Start` and `StartCacheTierOrExplain` purely to
    /// reach here, and neither of them has any other use for it -- so the object
    /// lives with the exchanges it reports on, and only the destination travels.
    Cc::CredentialNotice _notice;
    core::net::IConnector& _connector;
    core::net::EventLoop* _reactor;
    core::net::IAsyncAddressResolver& _resolver;
    core::platform::IClock& _clock;
    std::chrono::milliseconds _connectTimeout;
    std::chrono::milliseconds _ioTimeout;
    std::chrono::milliseconds _addressRefreshInterval;

    /// Whether the last exchange was answered, a failed one -- a dial that did not connect, or a
    /// connection that reached the I/O ceiling or lost its peer -- believed for
    /// `unreachableRetryInterval`; consulted before any lookup or dial. A member rather than a collaborator anybody wires:
    /// every `RemoteUpstream` has one, so there is no construction that could leave it out.
    UpstreamReachability _reachability;
};

} // namespace FastCache::Node
