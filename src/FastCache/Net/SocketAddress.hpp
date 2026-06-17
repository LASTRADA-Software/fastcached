// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/BlockingSocket.hpp> // Detail::NativeSocket / InvalidSocket

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// One candidate endpoint produced by an IAddressResolver, ready to hand to
/// ::bind. The raw sockaddr bytes are stored in a fixed, suitably-aligned
/// buffer so this header stays free of platform socket headers (matching the
/// `NativeSocket = uintptr_t` approach in BlockingSocket.hpp). The buffer is
/// sized to hold any `sockaddr_storage`; SocketAddress.cpp static_asserts the
/// fit.
struct ResolvedEndpoint
{
    /// Capacity of `storage`, equal to `sizeof(sockaddr_storage)` on every
    /// supported platform.
    static constexpr std::size_t StorageSize = 128;

    /// Raw sockaddr_in / sockaddr_in6 bytes. Aligned for any sockaddr type.
    alignas(alignof(std::max_align_t)) std::array<std::byte, StorageSize> storage {};
    /// Number of valid bytes in `storage` (a `socklen_t` value).
    std::uint32_t length { 0 };
    /// Address family: AF_INET or AF_INET6.
    int family { 0 };
    /// Transport protocol (e.g. IPPROTO_TCP) reported by the resolver.
    int protocol { 0 };
};

/// Injectable abstraction over name/address resolution — the one I/O seam that
/// the bind path depends on. Production wires SystemAddressResolver (getaddrinfo);
/// tests substitute a deterministic fake.
class IAddressResolver
{
  public:
    IAddressResolver() = default;
    IAddressResolver(IAddressResolver const&) = delete;
    IAddressResolver(IAddressResolver&&) = delete;
    IAddressResolver& operator=(IAddressResolver const&) = delete;
    IAddressResolver& operator=(IAddressResolver&&) = delete;
    virtual ~IAddressResolver() = default;

    /// Resolve a bind host + port into one or more candidate endpoints.
    /// @param host Address or hostname; e.g. "127.0.0.1", "::1", "localhost",
    ///             "0.0.0.0", "::". An empty host means the wildcard address.
    /// @param port TCP port in host byte order.
    /// @return A non-empty list of candidates to try binding (in preference
    ///         order), or an error message on resolution failure.
    [[nodiscard]] virtual std::expected<std::vector<ResolvedEndpoint>, std::string> Resolve(std::string_view host,
                                                                                            std::uint16_t port) = 0;
};

/// getaddrinfo-backed resolver. The only place that issues the resolution
/// syscall; supports IPv4 literals, IPv6 literals, and DNS hostnames.
class SystemAddressResolver final: public IAddressResolver
{
  public:
    /// @copydoc IAddressResolver::Resolve
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> Resolve(std::string_view host,
                                                                                    std::uint16_t port) override;
};

// DefaultAddressResolver() is declared in BlockingSocket.hpp (included above),
// which is where the bind-time resolver DI seam lives; re-declaring it here
// would be redundant.

/// Format the address held in a ResolvedEndpoint as a printable host string —
/// an IPv4 dotted-quad ("203.0.113.7") or an IPv6 textual address ("::1"). The
/// port is intentionally omitted: this feeds the `--log-source` connection
/// prefix, which records the client IP only.
/// @param endpoint Endpoint whose stored sockaddr is rendered.
/// @return The printable host string, or "" for an empty / unknown-family
///         endpoint (e.g. a peer address that was never captured).
[[nodiscard]] std::string FormatPeerAddress(ResolvedEndpoint const& endpoint);

namespace Detail
{

    /// A socket that has been created, bound, and put into the listening state.
    struct BoundListener
    {
        NativeSocket socket { InvalidSocket }; ///< The listening socket handle.
        int family { 0 };                      ///< AF_INET / AF_INET6 of the bound socket.
    };

    /// Resolve `host`/`port`, then create + SO_REUSEADDR + bind + listen the
    /// first resolved candidate that succeeds. This is the single home for the
    /// socket-bind syscall sequence the platform listeners share.
    /// @param resolver Injected resolver (the DI seam over getaddrinfo).
    /// @param host Bind host (IPv4/IPv6 literal or hostname).
    /// @param port TCP port in host byte order.
    /// @param backlog ::listen backlog.
    /// @param extraTypeFlags Flags OR'd into SOCK_STREAM at socket() creation
    ///        (e.g. SOCK_NONBLOCK | SOCK_CLOEXEC on Linux); 0 when unused.
    /// @param reusePort When ReusePort::Yes, set SO_REUSEPORT so several
    ///        listeners can bind the same port and the kernel load-balances new
    ///        connections across them (POSIX only — one listener per reactor
    ///        thread). No effect on platforms without SO_REUSEPORT (e.g. Windows).
    /// @return The bound, listening socket and its family, or an error message
    ///         describing why every candidate failed.
    [[nodiscard]] std::expected<BoundListener, std::string> BindAndListen(IAddressResolver& resolver,
                                                                          std::string_view host,
                                                                          std::uint16_t port,
                                                                          int backlog,
                                                                          int extraTypeFlags,
                                                                          ReusePort reusePort = ReusePort::No);

    /// Copy a raw sockaddr (as filled by accept/accept4/AcceptEx) into a
    /// platform-free ResolvedEndpoint, reading the address family from the
    /// sockaddr itself. The single home for the peer-capture memcpy the
    /// platform listeners share.
    /// @param sockaddr Pointer to a sockaddr / sockaddr_in / sockaddr_in6.
    /// @param length Valid byte count of the sockaddr (a socklen_t value).
    /// @return The captured endpoint; an all-zero endpoint when `length` is 0
    ///         or exceeds ResolvedEndpoint::StorageSize.
    [[nodiscard]] ResolvedEndpoint EndpointFromSockaddr(void const* sockaddr, std::uint32_t length) noexcept;

    /// Query a connected socket's remote peer with ::getpeername and format it
    /// as a printable host string. Used by the Windows multi-reactor acceptor,
    /// whose AcceptRaw hands off a raw connected handle without a captured peer.
    /// @param socket A connected stream-socket handle.
    /// @return The peer host ("203.0.113.7" / "::1"), or "" on failure.
    [[nodiscard]] std::string PeerAddressOf(NativeSocket socket) noexcept;

} // namespace Detail

} // namespace FastCache
