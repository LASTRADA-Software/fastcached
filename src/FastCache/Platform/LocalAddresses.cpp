// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/NarrowText.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <core/platform/WinsockInit.hpp>

#if defined(_WIN32)
    #include <winsock2.h>
// clang-format off
    // ws2tcpip.h and iphlpapi.h both require winsock2.h to have been seen first,
    // which is what this ordering is; clang-format sorts includes alphabetically
    // and would break the build silently.
    #include <ws2tcpip.h>

    #include <iphlpapi.h>
// clang-format on
#else
    #include <sys/socket.h>

    #include <ifaddrs.h>

    #include <arpa/inet.h>
    #include <net/if.h>
    #include <netinet/in.h>
#endif

namespace FastCache
{

namespace
{
    /// Append one interface address, converted the way a PEER's address is converted.
    ///
    /// `inet_ntop` on the family's address bytes, with no `%scope` suffix, because
    /// `core::net::formatPeerAddress` produces the other side of every comparison this feeds and
    /// does exactly this. See the header for why a divergence here would be a set that
    /// looks populated and matches nothing.
    ///
    /// One function rather than one per platform branch: the two enumerators hand over
    /// the same `sockaddr` and the family dispatch is the same three lines, which is
    /// exactly the copy-paste that diverges on the day a third family is added.
    ///
    /// Anything that is not IPv4 or IPv6 is skipped, and so is an address that would
    /// not convert -- rather than appended as an empty string. `SameHost` refuses the
    /// empty host on purpose, so an empty entry could never match anything and would
    /// only make the set look larger than it is.
    /// @param out Where to append; left alone when there is nothing to append.
    /// @param address The interface's address, or null.
    void AppendAddress(std::vector<std::string>& out, sockaddr const* address)
    {
        if (address == nullptr)
            return;

        // The address pointer differs per family; the textual conversion does not.
        void const* bytes = nullptr;
        switch (address->sa_family)
        {
            case AF_INET:
                bytes = &reinterpret_cast<sockaddr_in const*>(address)->sin_addr;
                break;
            case AF_INET6:
                bytes = &reinterpret_cast<sockaddr_in6 const*>(address)->sin6_addr;
                break;
            default:
                return;
        }

        std::array<char, INET6_ADDRSTRLEN> text {};
        if (::inet_ntop(address->sa_family, bytes, text.data(), text.size()) == nullptr)
            return;

        // No emptiness guard: `inet_ntop` either fails, which returned above, or
        // writes a NUL-terminated address of at least one character.
        out.emplace_back(text.data());
    }
} // namespace

namespace
{
    /// One address one interface carries, as the platform walk hands it over.
    ///
    /// ONE walk per platform feeds both questions this file answers -- every address this host
    /// answers on (`QueryLocalAddresses`), and each IPv4 address with its prefix and link state
    /// (`MakeSystemInterfaceAddresses`) -- so the two can never enumerate different sets, and the
    /// C linked lists each platform answers with are walked in one place each.
    struct InterfaceRecord
    {
        sockaddr const* address { nullptr }; ///< The address; valid only inside the visit.
        std::uint8_t prefixLength {};        ///< The on-link prefix, where the platform says.
        bool up {};                          ///< Whether the link is operationally up.
        bool loopback {};                    ///< Whether the interface is a loopback one.
        bool broadcastLink {};               ///< Whether the link is a broadcast medium.
        std::string name;                    ///< The interface's name.
    };

    /// What a walk hands each address to.
    using InterfaceVisitor = std::function<void(InterfaceRecord const&)>;

    /// What a walk came to: every address visited, or the platform's error. A machine with no
    /// address at all is a SUCCESS that visited nothing, never this error.
    using WalkOutcome = std::expected<void, std::error_code>;
} // namespace

#if defined(_WIN32)

namespace
{
    /// Walk every adapter's unicast addresses, both families.
    /// @param visit Handed each address.
    /// @return The `GetAdaptersAddresses` status when the platform would not answer.
    WalkOutcome WalkInterfaces(InterfaceVisitor const& visit)
    {
        // The documented call shape: ask with a buffer, grow when told it was too
        // small. 15 KiB is what Microsoft's own sample starts at, and a machine with
        // enough adapters to exceed it gets the retry rather than a truncated set --
        // a truncated set here is a local address silently classified as foreign.
        constexpr ULONG InitialBufferBytes = 15UL * 1024UL;
        constexpr int MaxAttempts = 4;
        constexpr ULONG Flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

        // `inet_ntop` lives in `ws2_32`, and Winsock in this tree is started LAZILY by
        // whoever first makes a socket. Nothing here makes one, so a process that only
        // asks this question -- a Catch2 case in its own process, which is every case
        // here -- would be the first caller into the library. Measured, the call does
        // answer without it on Windows 11; the guard is here because "it happens to work
        // on this build" is not the contract, and the failure it would buy is silent:
        // every conversion returns null, every address is dropped, and this reports a
        // machine with no addresses of its own -- which the oracle turns into refusing
        // every local client that is not on loopback.
        core::platform::ensureWinsockInitialized();

        // A vector of pointer-sized words rather than of bytes, so the storage carries
        // the alignment `IP_ADAPTER_ADDRESSES` is read back at. A `std::byte` buffer is
        // 1-aligned, and casting one to a struct full of pointers is undefined behaviour
        // that a sanitizer reports and a release build silently gets away with.
        auto const wordsFor = [](ULONG bytes) {
            return (static_cast<std::size_t>(bytes) + sizeof(std::uintptr_t) - 1) / sizeof(std::uintptr_t);
        };

        std::vector<std::uintptr_t> buffer;
        auto sizeBytes = InitialBufferBytes;
        auto status = ULONG { ERROR_BUFFER_OVERFLOW };
        // The retry CONDITION becomes a `break` and the retry CAP stays the range, which is what
        // the two clauses always meant apart. Equivalent rather than nearly so: `status` is
        // ERROR_BUFFER_OVERFLOW on the line above, so the old entry test was always true and the
        // first ask always happened -- a bottom `break` that changed whether the body runs once
        // would be the zero-trip hazard, and it cannot arise here.
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, MaxAttempts))
        {
            buffer.assign(wordsFor(sizeBytes), 0);
            status = ::GetAdaptersAddresses(
                AF_UNSPEC, Flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &sizeBytes);
            if (status != ERROR_BUFFER_OVERFLOW)
                break;
        }
        // ERROR_NO_DATA is the documented answer for a machine with no addresses: a walk that
        // visited nothing, not one that failed.
        if (status == ERROR_NO_DATA)
            return {};
        if (status != ERROR_SUCCESS)
            return std::unexpected { std::error_code { static_cast<int>(status), std::system_category() } };

        auto const* head = reinterpret_cast<IP_ADAPTER_ADDRESSES const*>(buffer.data());
        for (auto const* adapter = head; adapter != nullptr; adapter = adapter->Next)
        {
            // The friendly name is for a log line and decides nothing, so one that will not
            // convert is left empty rather than failing the walk.
            auto const name = adapter->FriendlyName != nullptr
                                  ? Utf8FromWideText(std::wstring_view { adapter->FriendlyName }).value_or(std::string {})
                                  : std::string {};
            for (auto const* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
                visit(InterfaceRecord { .address = unicast->Address.lpSockaddr,
                                        .prefixLength = unicast->OnLinkPrefixLength,
                                        .up = adapter->OperStatus == IfOperStatusUp,
                                        .loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK,
                                        // No flag says so here; the prefix decides on Windows.
                                        .broadcastLink = true,
                                        .name = name });
        }
        return {};
    }
} // namespace

#else

bool BroadcastLinkFrom(unsigned int flags) noexcept
{
    return (flags & IFF_BROADCAST) != 0U && (flags & IFF_POINTOPOINT) == 0U;
}

namespace
{
    /// The prefix length a netmask spells: its set bits, counted.
    /// @param netmask The netmask, or null.
    /// @return The prefix; 0 for none.
    [[nodiscard]] std::uint8_t PrefixOf(sockaddr const* netmask) noexcept
    {
        if (netmask == nullptr || netmask->sa_family != AF_INET)
            return 0;
        auto const mask = ntohl(reinterpret_cast<sockaddr_in const*>(netmask)->sin_addr.s_addr);
        return static_cast<std::uint8_t>(std::popcount(mask));
    }

    /// Walk every interface address, both families.
    /// @param visit Handed each address.
    /// @return `getifaddrs`' errno when the platform would not answer.
    WalkOutcome WalkInterfaces(InterfaceVisitor const& visit)
    {
        // A no-op here; see the Windows branch for why it is called at all.
        core::platform::ensureWinsockInitialized();

        ifaddrs* head = nullptr;
        if (::getifaddrs(&head) != 0)
            return std::unexpected { std::error_code { errno, std::generic_category() } };
        // A success with no list is a machine with no interface at all: visited nothing.
        if (head == nullptr)
            return {};

        // Freed on every path out, including the throwing one a visitor can take:
        // `getifaddrs` allocates and `freeifaddrs` is the only way back.
        auto const owned = std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> { head, &::freeifaddrs };

        // Operationally up is IFF_UP AND IFF_RUNNING: an interface configured up with no
        // carrier is up to the kernel and nobody is on the other end of it. A broadcast medium
        // is IFF_BROADCAST without IFF_POINTOPOINT: a tunnel carries a netmask and no broadcast.
        for (auto const* entry = head; entry != nullptr; entry = entry->ifa_next)
            visit(InterfaceRecord { .address = entry->ifa_addr,
                                    .prefixLength = PrefixOf(entry->ifa_netmask),
                                    .up = (entry->ifa_flags & IFF_UP) != 0U && (entry->ifa_flags & IFF_RUNNING) != 0U,
                                    .loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0U,
                                    .broadcastLink = BroadcastLinkFrom(entry->ifa_flags),
                                    .name = entry->ifa_name != nullptr ? std::string { entry->ifa_name } : std::string {} });
        return {};
    }
} // namespace

#endif

std::vector<std::string> QueryLocalAddresses()
{
    // Every address, down interfaces included. An address configured on this machine is this
    // machine's whether or not the link is currently up, and classifying it by link state would
    // make locality flap with a cable. A tunnel or a `ppp` interface legitimately has no address,
    // which `AppendAddress` skips.
    std::vector<std::string> addresses;
    if (!WalkInterfaces([&addresses](InterfaceRecord const& record) {
             AppendAddress(addresses, record.address);
         }).has_value())
        return {};
    return addresses;
}

namespace
{
    /// `IInterfaceAddressSource` over the platform walk: the IPv4 addresses only.
    class SystemInterfaceAddresses final: public IInterfaceAddressSource
    {
      public:
        [[nodiscard]] std::expected<std::vector<Ipv4InterfaceAddress>, std::error_code> Ipv4Addresses() const override
        {
            std::vector<Ipv4InterfaceAddress> out;
            auto const walked = WalkInterfaces([&out](InterfaceRecord const& record) {
                if (record.address == nullptr || record.address->sa_family != AF_INET)
                    return;
                auto entry = Ipv4InterfaceAddress { .interfaceName = record.name,
                                                    .address = {},
                                                    .prefixLength = record.prefixLength,
                                                    .up = record.up,
                                                    .loopback = record.loopback,
                                                    .broadcastLink = record.broadcastLink };
                auto const& in = reinterpret_cast<sockaddr_in const*>(record.address)->sin_addr;
                std::memcpy(entry.address.data(), &in, entry.address.size());
                out.push_back(std::move(entry));
            });
            if (!walked.has_value())
                return std::unexpected { walked.error() };
            return out;
        }
    };
} // namespace

std::unique_ptr<IInterfaceAddressSource> MakeSystemInterfaceAddresses()
{
    return std::make_unique<SystemInterfaceAddresses>();
}

BroadcastEligibility EligibilityOf(Ipv4InterfaceAddress const& address) noexcept
{
    // The first octet of 127/8, and the two of 169.254/16; and the widest prefix that still
    // leaves a subnet broadcast address -- a /31 and a /32 have none.
    constexpr std::uint8_t LoopbackOctet = 127;
    constexpr std::array<std::uint8_t, 2> LinkLocalOctets { 169, 254 };
    constexpr std::uint8_t WidestBroadcastPrefix = 30;

    auto const& octets = address.address;
    if (!address.up)
        return BroadcastEligibility::Down;
    if (address.loopback || octets[0] == LoopbackOctet)
        return BroadcastEligibility::Loopback;
    if (octets == std::array<std::uint8_t, 4> {})
        return BroadcastEligibility::Unassigned;
    if (octets[0] == LinkLocalOctets[0] && octets[1] == LinkLocalOctets[1])
        return BroadcastEligibility::LinkLocal;
    if (!address.broadcastLink || address.prefixLength == 0 || address.prefixLength > WidestBroadcastPrefix)
        return BroadcastEligibility::NoBroadcast;
    return BroadcastEligibility::Eligible;
}

std::array<std::uint8_t, 4> DirectedBroadcastOf(Ipv4InterfaceAddress const& address) noexcept
{
    // Octet by octet: each keeps the bits the prefix covers and sets every bit it does not.
    constexpr auto OctetBits = std::numeric_limits<std::uint8_t>::digits;
    auto broadcast = address.address;
    for (auto const index: std::views::iota(std::size_t { 0 }, broadcast.size()))
    {
        auto const covered =
            std::clamp(static_cast<int>(address.prefixLength) - (static_cast<int>(index) * OctetBits), 0, OctetBits);
        auto const hostBits = covered == OctetBits ? 0U : (std::numeric_limits<std::uint8_t>::max() >> covered);
        broadcast.at(index) = static_cast<std::uint8_t>(broadcast.at(index) | hostBits);
    }
    return broadcast;
}

std::string FormatIpv4(std::array<std::uint8_t, 4> const& address)
{
    return std::format("{}.{}.{}.{}", address[0], address[1], address[2], address[3]);
}

namespace
{
    /// `IHostAddressSource` over the free function above.
    class SystemHostAddresses final: public IHostAddressSource
    {
      public:
        [[nodiscard]] std::vector<std::string> Addresses() const override
        {
            return QueryLocalAddresses();
        }
    };
} // namespace

std::unique_ptr<IHostAddressSource> MakeSystemHostAddresses()
{
    return std::make_unique<SystemHostAddresses>();
}

namespace
{
    constexpr auto LocalityExpiryRows = EnumTable<HostEvent, LocalityExpiryRow> { {
        { .event = HostEvent::Suspending,
          .expires = false,
          .why = "nothing has moved yet, and the machine is about to stop asking" },
        { .event = HostEvent::Resumed,
          .expires = true,
          .why = "a machine that slept may wake on another network, holding other addresses" },
        { .event = HostEvent::NetworkChanged,
          .expires = true,
          .why = "an interface or an address came or went: the set may name an address another machine now holds" },
    } };

    static_assert(RowsInEnumeratorOrder(LocalityExpiryRows, &LocalityExpiryRow::event),
                  "every HostEvent needs a locality expiry row, at its own index");
} // namespace

LocalityExpiryRow const& LocalityExpiryFor(HostEvent event) noexcept
{
    return LocalityExpiryRows[static_cast<std::size_t>(event)];
}

void CachedLocalityOracle::OnHostEvent(HostEvent event)
{
    if (!LocalityExpiryFor(event).expires)
        return;
    std::scoped_lock const guard { _mutex };
    _due = true;
}

void CachedLocalityOracle::RefreshIfDue() const
{
    // On an interval or after a host event, never because a call did not find the
    // address. A refresh a stranger can provoke is a probe a stranger can bill this
    // machine for, once per request, and on Windows that probe costs milliseconds.
    auto const now = _clock.now();
    if (_due || now - _sampledAt >= _refreshInterval)
    {
        _addresses = _source.Addresses();
        _sampledAt = now;
        _due = false;
    }
}

std::vector<std::string> CachedLocalityOracle::Addresses() const
{
    std::scoped_lock const guard { _mutex };
    RefreshIfDue();
    return _addresses;
}

bool CachedLocalityOracle::IsThisMachine(std::string_view host) const
{
    // First, and without the lock: this is what the cache surface sees for
    // essentially every real caller, because the surface binds loopback. See the
    // class note -- being fast by construction is what keeps the cache below a
    // rare path rather than a hot one.
    if (IsLoopbackHost(host))
        return true;

    std::scoped_lock const guard { _mutex };
    RefreshIfDue();

    // `SameHost` rather than a string compare, and that is load-bearing: a surface
    // bound to `::` reports an IPv4 caller as `::ffff:10.0.0.1` while the probe
    // reports the interface as `10.0.0.1`, so a raw compare would refuse this
    // machine's own clients on exactly the dual-stack bind this rule was written
    // for. It also refuses an unnameable peer -- `core::net::formatPeerAddress` answers empty
    // for a family it does not know -- against an empty entry, which a raw compare
    // would have admitted.
    return std::ranges::any_of(_addresses, [host](std::string_view mine) { return SameHost(host, mine); });
}

} // namespace FastCache
