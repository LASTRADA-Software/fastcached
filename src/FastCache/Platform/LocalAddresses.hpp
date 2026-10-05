// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/HostEvents.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <core/platform/Clock.hpp>

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

    /// @param host The peer's **host**, as `core::net::ISocket::PeerAddress()` reports it --
    ///        never an endpoint, and never with a port. A peer dials from an
    ///        ephemeral source port, which is why nothing here compares a port.
    /// @return True when that address belongs to the machine this process runs on.
    [[nodiscard]] virtual bool IsThisMachine(std::string_view host) const = 0;

    /// The addresses `IsThisMachine` answers from, as it would answer now.
    ///
    /// **The ONE set this machine both reports and checks by.** A worker's heartbeat reports it,
    /// the scheduler derives a grant's dial hint from it, and a machine ticket minted for that
    /// hint is spent here only if `IsThisMachine` accepts the hint's host -- so the two must be one
    /// reading of one value, never a snapshot each. With two, a VPN reconnect lets the heartbeat
    /// report the new address at once while the audience refuses it until its own refresh: every
    /// hinted compile in that window is refused `wrong-audience`, and that refusal is final, so the
    /// build compiles locally rather than falling back to the name.
    /// @return The addresses, as the source spelled them; empty when it would not say.
    [[nodiscard]] virtual std::vector<std::string> Addresses() const = 0;
};

/// Whether a host event makes `CachedLocalityOracle`'s address set due for a re-probe.
struct LocalityExpiryRow
{
    HostEvent event;      ///< The event this row describes.
    bool expires;         ///< Whether the set is re-probed at the next question after it.
    std::string_view why; ///< The reason, in one sentence.
};

/// The row for @p event.
/// @param event A host event.
/// @return Its row.
[[nodiscard]] LocalityExpiryRow const& LocalityExpiryFor(HostEvent event) noexcept;

/// The production oracle: loopback answered outright, everything else against an
/// address set refreshed on an interval -- and at the first question after a host event that
/// can move an address (`LocalityExpiryFor`).
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
/// ## And on a NETWORK CHANGE, which is a host event rather than a miss
///
/// The interval bounds a stale set; a host event that says an address moved ends it. A VPN that
/// re-addresses a laptop would otherwise leave the ticket audience accepting the address it lost
/// for up to one interval -- and a machine ticket minted for that address, now another machine's,
/// spendable once more here. The event only MARKS the set due: the probe still runs on the next
/// question, on the thread that asks, so the event thread returns at once (`IHostEventSink`'s
/// contract). No peer can raise a host event, so this is no amplifier.
///
/// Thread-safe. The lock is taken only past the loopback branch, so the common path
/// never contends -- which is what makes a plain mutex right here rather than a shared
/// one.
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
class CachedLocalityOracle final: public ILocalityOracle, public IHostEventSink
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
                                  core::platform::IClock& clock,
                                  std::chrono::milliseconds refreshInterval = DefaultRefreshInterval):
        _source { source },
        _clock { clock },
        _refreshInterval { refreshInterval },
        _addresses { source.Addresses() },
        _sampledAt { clock.now() }
    {
    }

    /// @copydoc ILocalityOracle::IsThisMachine
    [[nodiscard]] bool IsThisMachine(std::string_view host) const override;

    /// The set `IsThisMachine` answers from, refreshed on the same interval and by the same rule:
    /// asking never forces a probe the interval has not allowed.
    ///
    /// What a stale set does to the dial hints a heartbeat of it produces, both directions:
    ///
    ///   - **An address just GAINED is not reported** until the next refresh, so the scheduler
    ///     derives no hint from it (`NotAReportedInterface`) and the client dials the advertised
    ///     name, as it did before hints existed. Fails safe, self-heals within one interval.
    ///   - **An address just LOST is still reported** for at most one interval. A hint naming it
    ///     reaches nothing, or another worker that refuses the lease `LeaseEndpointMismatch`; both
    ///     send the client on to the name.
    /// @return The current set.
    [[nodiscard]] std::vector<std::string> Addresses() const override;

    /// Mark the set due when @p event can move an address (`LocalityExpiryFor`). Probes nothing:
    /// the next question re-probes, on its own thread.
    /// @param event What the host reported.
    void OnHostEvent(HostEvent event) override;

  private:
    /// Replace the set when the interval has passed since it was taken, or a host event marked it
    /// due. Call with `_mutex` held.
    void RefreshIfDue() const;

    IHostAddressSource const& _source;
    core::platform::IClock& _clock;
    std::chrono::milliseconds _refreshInterval;

    /// Guards the two members below. Mutable because `IsThisMachine` is logically
    /// const and must still take it -- the alternative is a const method reading a
    /// vector it is itself replacing.
    mutable std::mutex _mutex;
    mutable std::vector<std::string> _addresses;
    mutable core::platform::SteadyTimePoint _sampledAt;
    mutable bool _due = false; ///< A host event said an address may have moved since `_sampledAt`.
};

/// One IPv4 address configured on one of this machine's interfaces, with what its link says.
///
/// What `QueryLocalAddresses` cannot answer: it reports addresses as text, every one, up or down,
/// because it decides LOCALITY; this reports the prefix length and the link state, because it
/// decides where a directed broadcast reaches. Both come from ONE platform walk per platform, so
/// they never enumerate different sets.
struct Ipv4InterfaceAddress
{
    std::string interfaceName;              ///< The interface's name, for a log line; decides nothing.
    std::array<std::uint8_t, 4> address {}; ///< The address, network byte order.
    std::uint8_t prefixLength {};           ///< The on-link prefix, 0..32.
    bool up {};                             ///< Whether the link is operationally up.
    bool loopback {};                       ///< Whether the interface is a loopback one.
    /// Whether the link is a broadcast medium: on POSIX, `IFF_BROADCAST` set and `IFF_POINTOPOINT`
    /// clear. A tunnel -- a WireGuard `wg0` at `10.0.0.2/24`, a `tun`, a `ppp` -- has a subnet on
    /// paper and no broadcast domain behind it, so its prefix alone would call it eligible. Windows
    /// reports no such flag, and its walk says `true`, leaving the prefix to decide there. False by
    /// default, so a record nobody filled in is beaconed from nowhere rather than everywhere.
    bool broadcastLink {};

    [[nodiscard]] friend bool operator==(Ipv4InterfaceAddress const&, Ipv4InterfaceAddress const&) = default;
};

/// Where this machine's interface addresses come from.
///
/// A seam for `IHostAddressSource`'s reason: the rules worth being wrong about -- what is eligible, when
/// the set is refreshed, what none means -- are unassertable against a real kernel.
class IInterfaceAddressSource
{
  public:
    IInterfaceAddressSource() = default;
    IInterfaceAddressSource(IInterfaceAddressSource const&) = delete;
    IInterfaceAddressSource& operator=(IInterfaceAddressSource const&) = delete;
    IInterfaceAddressSource(IInterfaceAddressSource&&) = delete;
    IInterfaceAddressSource& operator=(IInterfaceAddressSource&&) = delete;
    virtual ~IInterfaceAddressSource() = default;

    /// A walk that FAILED is not a machine with no interfaces, so it is its own answer: an empty
    /// list is what the platform reported, and the error is what it could not.
    /// @return Every IPv4 address on every interface, eligible or not; or the platform's error when
    ///         it could not be asked -- `getifaddrs`' errno, or `GetAdaptersAddresses`' status.
    [[nodiscard]] virtual std::expected<std::vector<Ipv4InterfaceAddress>, std::error_code> Ipv4Addresses() const = 0;
};

/// The platform's own answer: `GetAdaptersAddresses` on Windows, `getifaddrs` elsewhere.
/// @return The source.
[[nodiscard]] std::unique_ptr<IInterfaceAddressSource> MakeSystemInterfaceAddresses();

#if !defined(_WIN32)
/// Whether a POSIX interface's flags describe a broadcast medium: `IFF_BROADCAST` set and
/// `IFF_POINTOPOINT` clear. Every other bit, `IFF_LOOPBACK` included, is someone else's question.
///
/// Its own function so the rule is assertable: the walk reads a real kernel, and the fake source
/// hands `Ipv4InterfaceAddress::broadcastLink` over already decided, so neither reaches this.
/// @param flags The interface's `ifa_flags`.
/// @return Whether a directed broadcast on this link reaches anyone.
[[nodiscard]] bool BroadcastLinkFrom(unsigned int flags) noexcept;
#endif

/// Why an interface address is, or is not, one a directed broadcast is sent from.
///
/// **Private: never transmitted, never persisted**; its word appears in a log line only.
enum class BroadcastEligibility : std::uint8_t
{
    Eligible,    ///< An up, non-loopback, routable address on a subnet with a broadcast address.
    Down,        ///< Its link is not up, so nothing is on the other end.
    Loopback,    ///< This machine only: no peer is there.
    LinkLocal,   ///< 169.254/16, which an adapter takes when it got no real address.
    NoBroadcast, ///< A point-to-point or non-broadcast link, or a /31, /32 or /0: no broadcast reaches a peer.
    Unassigned,  ///< 0.0.0.0: configured with nothing.
    Last,        ///< Not an answer, and has no row: the length of a table keyed by one.
};

/// What one eligibility answer is called, and why it is excluded.
struct BroadcastEligibilityRow
{
    BroadcastEligibility answer; ///< The answer this row describes.
    std::string_view word;       ///< Its name in a log line.
    std::string_view why;        ///< Why an address with this answer is not beaconed from.
};

/// One row per `BroadcastEligibility`, in enumerator order.
///
/// **Link-local is excluded, and deliberately.** A 169.254 address is what an adapter configures
/// when it got no real one -- a DHCP failure, or a virtual or tunnel adapter with nothing behind
/// it -- and a beacon there reaches at most other machines in the same failed state. The cost
/// runs the visible way: a segment of nothing but link-local hosts has NO eligible interface,
/// which is said by name with the remedy (`--discovery=169.254.255.255:<port>`), never beaconed
/// into silently.
inline constexpr EnumTable<BroadcastEligibility, BroadcastEligibilityRow> BroadcastEligibilityRows { {
    { .answer = BroadcastEligibility::Eligible, .word = "eligible", .why = "" },
    { .answer = BroadcastEligibility::Down, .word = "down", .why = "its link is not up" },
    { .answer = BroadcastEligibility::Loopback, .word = "loopback", .why = "it reaches only this machine" },
    { .answer = BroadcastEligibility::LinkLocal,
      .word = "link-local",
      .why = "169.254/16 is what an adapter takes when it got no real address" },
    { .answer = BroadcastEligibility::NoBroadcast,
      .word = "no-broadcast",
      .why = "its link is point-to-point or carries no broadcast (a tunnel), or a /31, /32 or /0 has no subnet "
             "broadcast address" },
    { .answer = BroadcastEligibility::Unassigned, .word = "unassigned", .why = "0.0.0.0 is no address" },
} };

static_assert(RowsInEnumeratorOrder(BroadcastEligibilityRows, &BroadcastEligibilityRow::answer),
              "BroadcastEligibilityRows must hold one row per BroadcastEligibility, in enumerator order");

/// Whether a directed broadcast is sent from @p address, and if not, why.
///
/// The checks are asked in one order and the first that excludes answers, so a down loopback is
/// `Down` -- the answer an operator can act on first.
/// @param address The interface address.
/// @return The answer.
[[nodiscard]] BroadcastEligibility EligibilityOf(Ipv4InterfaceAddress const& address) noexcept;

/// The directed broadcast address of @p address's subnet: every host bit set.
/// @param address The interface address; its prefix must be 1..30 for the answer to mean anything.
/// @return The broadcast address, network byte order.
[[nodiscard]] std::array<std::uint8_t, 4> DirectedBroadcastOf(Ipv4InterfaceAddress const& address) noexcept;

/// @p address in dotted-quad text.
/// @param address The address, network byte order.
/// @return Its text.
[[nodiscard]] std::string FormatIpv4(std::array<std::uint8_t, 4> const& address);

} // namespace FastCache
