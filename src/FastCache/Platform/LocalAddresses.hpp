// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Every address this host currently answers on, in the spelling a peer arrives in.
///
/// `getifaddrs` on POSIX, `GetAdaptersAddresses` on Windows, and both are converted
/// with `inet_ntop` **without** a `%scope` suffix -- deliberately, because
/// `Net/SocketAddress::FormatPeerAddress` is what produces the string this set is
/// compared against and it uses the same conversion. A probe that spelled a
/// link-local address `fe80::1%eth0` while the kernel reports the peer as `fe80::1`
/// would be a set that looks populated and matches nothing, which is the failure
/// mode this whole file exists to avoid.
///
/// Both families, every interface, loopback included. Filtering the set is a policy
/// decision, and it belongs to the oracle below rather than to a probe.
///
/// **Costs a syscall, and the two platforms are not comparable.** Measured on one
/// machine, 200 calls after a warm-up, this whole function rather than the bare
/// syscall: **0.0086 ms** mean on Linux (`getifaddrs`, 5 addresses) against
/// **2.04 ms** mean and 5.7 ms worst on Windows (`GetAdaptersAddresses`, 26
/// addresses) -- a factor of **238**. Anything asking this per request is free on
/// the platform it was written on and is the dominant cost of a request on the
/// other, which is why `CachedLocalityOracle` below exists and why it refreshes on
/// an interval rather than on demand.
///
/// Empty when the platform would not say. Callers must read that as "this machine
/// has no addresses it can prove", never as "no addresses exist" -- the oracle turns
/// it into loopback-only, which is the direction it has to fail in.
/// @return The addresses, unordered, possibly with duplicates.
[[nodiscard]] std::vector<std::string> QueryLocalAddresses();

/// The seam over the probe above.
///
/// The project's inject-every-ambient-dependency rule applied to the one call in
/// this file that touches the machine. Everything worth being *wrong* about lives in
/// `CachedLocalityOracle` -- when it refreshes, what it does with an empty answer,
/// how it folds an IPv4-mapped spelling -- and none of that is assertable against a
/// real kernel: a test would be asserting about whichever machine the suite happened
/// to run on, on a question whose answer decides who may read this machine's build
/// output.
class IHostAddressSource
{
  public:
    IHostAddressSource() = default;
    IHostAddressSource(IHostAddressSource const&) = delete;
    IHostAddressSource& operator=(IHostAddressSource const&) = delete;
    IHostAddressSource(IHostAddressSource&&) = delete;
    IHostAddressSource& operator=(IHostAddressSource&&) = delete;
    virtual ~IHostAddressSource() = default;

    /// This host's addresses right now.
    /// @return The addresses, or empty when the platform would not say.
    [[nodiscard]] virtual std::vector<std::string> Addresses() const = 0;
};

/// An address source reading the real machine, through `QueryLocalAddresses`.
/// @return The source; never null.
[[nodiscard]] std::unique_ptr<IHostAddressSource> MakeSystemHostAddresses();

/// Answers "is the caller at this address running on this machine".
///
/// A seam of its own rather than a function, because it is a *decision* surface: the
/// node's cache tier serves this machine and refuses every other one, so a test of
/// that rule has to be able to present a peer that is local and a peer that is not,
/// on a machine that has neither address.
///
/// Narrower than `Core/IsLoopbackHost`, which answers only for `127.0.0.0/8` and
/// `::1`, and narrower than `Distributed::IMembershipOracle`, which answers about a
/// *fleet*. This is the third question and it is the only one the cache surface may
/// ask: an operator's member list names machines that are not this one.
class ILocalityOracle
{
  public:
    ILocalityOracle() = default;
    ILocalityOracle(ILocalityOracle const&) = delete;
    ILocalityOracle& operator=(ILocalityOracle const&) = delete;
    ILocalityOracle(ILocalityOracle&&) = delete;
    ILocalityOracle& operator=(ILocalityOracle&&) = delete;
    virtual ~ILocalityOracle() = default;

    /// @param host The peer's **host**, as `ISocket::PeerAddress()` reports it --
    ///        never an endpoint, and never with a port. A peer dials from an
    ///        ephemeral source port, which is the same reason
    ///        `Distributed::ClusterMembership` keys on hosts.
    /// @return True when that address belongs to the machine this process runs on.
    [[nodiscard]] virtual bool IsThisMachine(std::string_view host) const = 0;
};

/// The production oracle: loopback answered outright, everything else against an
/// address set refreshed on an interval.
///
/// ## Fast by construction before it is fast by cache
///
/// `IsLoopbackHost` is asked **first** and answers without touching the set or its
/// lock. That is not an optimization of the cache -- it is what keeps essentially
/// all real traffic away from it. The cache surface binds loopback by default, so
/// every ordinary `fastcache-cc` on this machine is answered by a string compare,
/// and the address set bounds only the rare path: a caller arriving over a widened
/// bind. A cache that only the unusual case reaches is a far weaker thing to have to
/// get right.
///
/// ## Why a stale answer is safe here, in both directions
///
/// Naming both is the point; one of them alone is how a longer interval gets talked
/// into being fine.
///
///   - **An address this machine has just GAINED is refused** until the next
///     refresh -- at most one interval. It fails *closed* and self-heals with no
///     operator action. What it costs is one local client, reaching its own node
///     over a freshly-assigned address on a widened bind, falling back to a local
///     compile: a miss and a retry, never a wrong answer.
///   - **An address this machine has just LOST is admitted** for at most one
///     interval. Exploiting it requires another machine to be handed that exact
///     address inside the window -- a DHCP reassignment racing the refresh -- and
///     the machine that gains it is on the same L2 segment the address came from.
///
/// Neither direction produces a *wrong answer that looks right*, which is the line
/// the project's caching rule draws: an answer whose staleness is served confidently
/// (a compiler's target triple, say) is not cacheable at any price, and this one is
/// not that shape.
///
/// ## Refreshed on an interval, never on a miss
///
/// A refresh triggered by an unrecognised address would hand a remote peer a free
/// amplifier: it could force the expensive probe once per request simply by asking,
/// and on Windows that probe is milliseconds. Interval-guarded, a peer sending a
/// million refusals still costs this machine one probe per interval.
///
/// Thread-safe. The lock is taken only past the loopback branch, so the common path
/// never contends -- which is what makes a plain mutex right here rather than the
/// shared one `ClusterMembership` needs.
///
/// ## The refresh runs INLINE, on whatever thread asked
///
/// Stated rather than left to be discovered, because that thread is a reactor: the
/// node's cache surface answers on its event loop, so one caller in thirty seconds
/// pays the probe's ~2 ms (worst measured 5.7 ms) and every connection multiplexed
/// on that loop waits behind it, holding this lock. That is deliberate and it is
/// the reason the two numbers above are in this header at all -- a probe on a pool
/// thread would need the answer before it could decide anything, so it would be a
/// suspension in the middle of an admission check rather than a saving.
///
/// It is bounded by the interval and reached only past the loopback branch, so on
/// the default bind it never runs at all. If a future surface asks this question at
/// a rate where that stops being true, the fix is to refresh it from somewhere else
/// and publish -- not to shorten the interval.
class CachedLocalityOracle final: public ILocalityOracle
{
  public:
    /// How long an address set is served before the machine is asked again.
    ///
    /// Thirty seconds: long enough that the Windows probe's ~2 ms is invisible at
    /// any request rate, short enough that both failure directions above are bounded
    /// by something an operator would describe as "immediately". It is a constant
    /// rather than a flag because neither direction has an operator-visible cost
    /// worth a decision -- see the two bullets above.
    static constexpr std::chrono::seconds DefaultRefreshInterval { 30 };

    /// @param source Where the machine's addresses come from; must outlive this.
    /// @param clock Time source deciding when the set is refreshed; must outlive this.
    /// @param refreshInterval How long a set is served for. Zero means every call
    ///        past the loopback branch re-probes, which is for tests only -- in
    ///        production it is the amplifier the class note refuses.
    explicit CachedLocalityOracle(IHostAddressSource const& source,
                                  IClock& clock,
                                  std::chrono::milliseconds refreshInterval = DefaultRefreshInterval):
        _source { source },
        _clock { clock },
        _refreshInterval { refreshInterval },
        _addresses { source.Addresses() },
        _sampledAt { clock.Now() }
    {
    }

    /// @copydoc ILocalityOracle::IsThisMachine
    [[nodiscard]] bool IsThisMachine(std::string_view host) const override;

  private:
    IHostAddressSource const& _source;
    IClock& _clock;
    std::chrono::milliseconds _refreshInterval;

    /// Guards the two members below. Mutable because `IsThisMachine` is logically
    /// const and must still take it -- the alternative is a const method reading a
    /// vector it is itself replacing.
    mutable std::mutex _mutex;
    mutable std::vector<std::string> _addresses;
    mutable TimePoint _sampledAt;
};

} // namespace FastCache
