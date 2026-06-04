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
    /// @return The bound, listening socket and its family, or an error message
    ///         describing why every candidate failed.
    [[nodiscard]] std::expected<BoundListener, std::string> BindAndListen(
        IAddressResolver& resolver, std::string_view host, std::uint16_t port, int backlog, int extraTypeFlags);

} // namespace Detail

} // namespace FastCache
