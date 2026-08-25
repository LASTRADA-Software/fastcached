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

    /// Resolve `host`/`port`, then create + claim-exclusively + bind + listen
    /// the first resolved candidate that succeeds. This is the single home for
    /// the socket-bind syscall sequence the platform listeners share.
    ///
    /// The listener owns its address: on every platform, a second socket asking
    /// for the same {address, port} is refused. The one way to share one is for
    /// both to ask for `ReusePort::Yes`, and only where the platform has
    /// SO_REUSEPORT (see the parameter below). Saying "mine" takes a different
    /// socket option on each platform -- SocketAddress.cpp's
    /// `ExclusiveBindOption` carries which, and why the obvious one is a hole on
    /// Windows.
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

    /// Is this host text a literal address rather than a name to be looked up?
    ///
    /// The question exists so a dial to a literal never reaches a resolver
    /// thread, and that is load-bearing rather than an optimisation: every
    /// internal dial in this codebase is to a literal -- Raft peers, the
    /// launcher's `127.0.0.1:6674`, an endpoint discovery proved -- and the
    /// launcher makes one per translation unit, thousands of times per build. It
    /// is also what lets the whole connect path be tested without a thread
    /// existing.
    ///
    /// Answered with `inet_pton` for both families rather than by inspecting the
    /// characters, so there is one definition of "literal" and it is the
    /// platform's own.
    ///
    /// Which means a **scoped** literal (`fe80::1%eth0`) is platform-dependent, and
    /// that is fine: glibc's `inet_pton` rejects the zone suffix and macOS's accepts
    /// it, so the same text is a name on one host and a literal on the next. Either
    /// answer produces a working dial -- one goes straight to `connect`, the other
    /// through the resolver, which also understands zones -- so this deliberately
    /// does not force them to agree. What must never happen is the other direction:
    /// a NAME reported as a literal would be handed to `connect` as an address and
    /// could not resolve at all, which is why the tests pin that half strictly.
    ///
    /// @param host Host text, unbracketed.
    /// @return true when `host` parses as an IPv4 or IPv6 literal.
    [[nodiscard]] bool IsNumericHost(std::string_view host) noexcept;

    /// Read the port out of a raw sockaddr.
    ///
    /// The one implementation of "which port is this", spelled once because the
    /// two sockaddr layouts put it at different offsets and reading the wrong one
    /// yields a plausible-looking number rather than an error -- so a second copy
    /// of the family switch would be a second chance to get that silently wrong.
    /// Takes the bytes rather than a socket because the two callers have
    /// different things in hand: a listener has a handle to ask `getsockname`,
    /// while a datagram receive is handed the sender's address by `recvfrom` and
    /// has no socket to ask about it at all.
    /// @param sockaddr Pointer to a sockaddr / sockaddr_in / sockaddr_in6.
    /// @param length Valid byte count of the sockaddr (a socklen_t value).
    /// @return The port in host byte order, or 0 for a family that has no port.
    [[nodiscard]] std::uint16_t PortOfSockaddr(void const* sockaddr, std::uint32_t length) noexcept;

    /// Query a bound socket's local port with ::getsockname.
    ///
    /// The one implementation of "which port did I actually get", which is a
    /// question every listener has to answer and only `BlockingListener` could:
    /// a bind to port 0 means "pick a free one", so the port an operator, a log
    /// line or a test needs is the one the kernel chose and not the one that was
    /// asked for.
    ///
    /// @param socket A bound socket handle.
    /// @return The local port in host byte order, or 0 when the handle is
    ///         invalid, unbound, or of a family that has no port.
    [[nodiscard]] std::uint16_t BoundPortOf(NativeSocket socket) noexcept;

    /// Query a connected socket's remote peer with ::getpeername and format it
    /// as a printable host string. Used by the Windows multi-reactor acceptor,
    /// whose AcceptRaw hands off a raw connected handle without a captured peer.
    /// @param socket A connected stream-socket handle.
    /// @return The peer host ("203.0.113.7" / "::1"), or "" on failure.
    [[nodiscard]] std::string PeerAddressOf(NativeSocket socket) noexcept;

} // namespace Detail

} // namespace FastCache
