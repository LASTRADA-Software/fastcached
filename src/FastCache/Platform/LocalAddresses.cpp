// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/BlockingSocket.hpp> // Detail::EnsureNetworkInitialised
#include <FastCache/Platform/LocalAddresses.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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
    #include <netinet/in.h>
#endif

namespace FastCache
{

namespace
{
    /// Append one interface address, converted the way a PEER's address is converted.
    ///
    /// `inet_ntop` on the family's address bytes, with no `%scope` suffix, because
    /// `FormatPeerAddress` produces the other side of every comparison this feeds and
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

#if defined(_WIN32)

std::vector<std::string> QueryLocalAddresses()
{
    // The documented call shape: ask with a buffer, grow when told it was too
    // small. 15 KiB is what Microsoft's own sample starts at, and a machine with
    // enough adapters to exceed it gets the retry rather than a truncated set --
    // a truncated set here is a local address silently classified as foreign.
    constexpr ULONG InitialBufferBytes = 15UL * 1024UL;
    constexpr int MaxAttempts = 4;
    constexpr ULONG Flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;

    // `inet_ntop` lives in `ws2_32`, and Winsock in this tree is started LAZILY by
    // whoever first makes a socket. Nothing here makes one, so a process that only
    // asks this question -- a Catch2 case in its own process, which is every case
    // here -- would be the first caller into the library. Measured, the call does
    // answer without it on Windows 11; the guard is here because "it happens to work
    // on this build" is not the contract, and the failure it would buy is silent:
    // every conversion returns null, every address is dropped, and this function
    // reports a machine with no addresses of its own -- which the oracle above turns
    // into refusing every local client that is not on loopback.
    //
    // A no-op away from Windows, which is why it is spelled once rather than behind
    // a second `#if` somebody has to keep in step.
    Detail::EnsureNetworkInitialised();

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
    for (auto attempt = 0; attempt < MaxAttempts && status == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.assign(wordsFor(sizeBytes), 0);
        status = ::GetAdaptersAddresses(
            AF_UNSPEC, Flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &sizeBytes);
    }
    if (status != ERROR_SUCCESS)
        return {};

    std::vector<std::string> addresses;
    auto const* head = reinterpret_cast<IP_ADAPTER_ADDRESSES const*>(buffer.data());
    for (auto const* adapter = head; adapter != nullptr; adapter = adapter->Next)
    {
        // Every adapter, including one that is down. An address configured on this
        // machine is this machine's whether or not the link is currently up, and
        // classifying it by link state would make locality flap with a cable.
        for (auto const* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
            AppendAddress(addresses, unicast->Address.lpSockaddr);
    }
    return addresses;
}

#else

std::vector<std::string> QueryLocalAddresses()
{
    // A no-op here; see the Windows branch for why it is called at all.
    Detail::EnsureNetworkInitialised();

    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0 || head == nullptr)
        return {};

    // Freed on every path out, including the throwing one an `emplace_back` can
    // take: `getifaddrs` allocates and `freeifaddrs` is the only way back.
    auto const owned = std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> { head, &::freeifaddrs };

    // A tunnel or a `ppp` interface legitimately has no address, which `AppendAddress`
    // skips. Interfaces that are DOWN are kept, for the reason the Windows branch
    // states: an address configured on this machine is this machine's whether or not
    // the link is up, and classifying it by link state would make locality flap with
    // a cable.
    std::vector<std::string> addresses;
    for (auto const* entry = head; entry != nullptr; entry = entry->ifa_next)
        AppendAddress(addresses, entry->ifa_addr);
    return addresses;
}

#endif

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

bool CachedLocalityOracle::IsThisMachine(std::string_view host) const
{
    // First, and without the lock: this is what the cache surface sees for
    // essentially every real caller, because the surface binds loopback. See the
    // class note -- being fast by construction is what keeps the cache below a
    // rare path rather than a hot one.
    if (IsLoopbackHost(host))
        return true;

    std::scoped_lock const guard { _mutex };

    // On an interval, never because this call did not find the address. A refresh a
    // stranger can provoke is a probe a stranger can bill this machine for, once per
    // request, and on Windows that probe costs milliseconds.
    auto const now = _clock.Now();
    if (now - _sampledAt >= _refreshInterval)
    {
        _addresses = _source.Addresses();
        _sampledAt = now;
    }

    // `SameHost` rather than a string compare, and that is load-bearing: a surface
    // bound to `::` reports an IPv4 caller as `::ffff:10.0.0.1` while the probe
    // reports the interface as `10.0.0.1`, so a raw compare would refuse this
    // machine's own clients on exactly the dual-stack bind this rule was written
    // for. It also refuses an unnameable peer -- `FormatPeerAddress` answers empty
    // for a family it does not know -- against an empty entry, which a raw compare
    // would have admitted.
    return std::ranges::any_of(_addresses, [host](std::string_view mine) { return SameHost(host, mine); });
}

} // namespace FastCache
