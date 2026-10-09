// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/RouteProbe.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <core/Ranges.hpp>
#include <core/platform/WinsockInit.hpp>

#if defined(_WIN32)
    #include <winsock2.h>
// clang-format off
    // ws2tcpip.h requires winsock2.h to have been seen first, which is what this ordering is;
    // clang-format sorts includes alphabetically and would break the build silently.
    #include <ws2tcpip.h>
// clang-format on
#else
    #include <sys/socket.h>

    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

namespace FastCache
{

namespace
{
    /// The discard port. Nothing is ever sent to it: `connect()` on a datagram socket only asks the
    /// kernel to choose a route and a source address.
    constexpr std::uint16_t DiscardPort = 9;

#if defined(_WIN32)
    using NativeSocket = SOCKET;
    constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
    constexpr auto NetUnreachable = WSAENETUNREACH;
    constexpr auto HostUnreachable = WSAEHOSTUNREACH;
    constexpr auto AddressNotAvailable = WSAEADDRNOTAVAIL;
    constexpr auto FamilyUnsupported = WSAEAFNOSUPPORT;

    [[nodiscard]] int LastSocketError() noexcept
    {
        return ::WSAGetLastError();
    }

    void CloseSocket(NativeSocket handle) noexcept
    {
        ::closesocket(handle);
    }
#else
    using NativeSocket = int;
    constexpr NativeSocket InvalidSocket = -1;
    constexpr auto NetUnreachable = ENETUNREACH;
    constexpr auto HostUnreachable = EHOSTUNREACH;
    constexpr auto AddressNotAvailable = EADDRNOTAVAIL;
    constexpr auto FamilyUnsupported = EAFNOSUPPORT;

    [[nodiscard]] int LastSocketError() noexcept
    {
        return errno;
    }

    void CloseSocket(NativeSocket handle) noexcept
    {
        ::close(handle);
    }
#endif

    /// What one platform error code means to a route probe.
    struct SocketErrorRow
    {
        int code;              ///< The platform's error code.
        RouteProbeError error; ///< The verdict it maps to.
    };

    constexpr std::array<SocketErrorRow, 4> SocketErrorRows { {
        { .code = NetUnreachable, .error = RouteProbeError::NoRoute },
        { .code = HostUnreachable, .error = RouteProbeError::NoRoute },
        { .code = AddressNotAvailable, .error = RouteProbeError::NoRoute },
        { .code = FamilyUnsupported, .error = RouteProbeError::Unsupported },
    } };

    /// @param code A platform socket error code.
    /// @return Its verdict; a code with no row is `NoRoute`, because whatever it was, no address
    ///         came out of it.
    [[nodiscard]] RouteProbeError ErrorFrom(int code) noexcept
    {
        auto const* const row = core::findOrNull(SocketErrorRows, code, &SocketErrorRow::code);
        return row != nullptr ? row->error : RouteProbeError::NoRoute;
    }

    /// Owns a socket handle and closes it on every path out.
    class SocketGuard
    {
      public:
        /// @param handle The handle to own; may be invalid.
        explicit SocketGuard(NativeSocket handle) noexcept:
            _handle { handle }
        {
        }
        SocketGuard(SocketGuard const&) = delete;
        SocketGuard& operator=(SocketGuard const&) = delete;
        SocketGuard(SocketGuard&&) = delete;
        SocketGuard& operator=(SocketGuard&&) = delete;
        ~SocketGuard()
        {
            if (_handle != InvalidSocket)
                CloseSocket(_handle);
        }

        /// @return The owned handle.
        [[nodiscard]] NativeSocket Get() const noexcept
        {
            return _handle;
        }

      private:
        NativeSocket _handle;
    };

    /// A parsed target, in whichever family its literal is.
    using ParsedAddress = std::variant<sockaddr_in, sockaddr_in6>;

    /// @param target Text that may be an address literal.
    /// @return The address with the discard port set, or nothing when it is not a literal.
    [[nodiscard]] std::optional<ParsedAddress> ParseTarget(std::string_view target)
    {
        // A zone (`fe80::1%lo`) is not part of a literal this probe takes, and whether `inet_pton`
        // accepts one is the platform's choice -- macOS does, Linux does not -- so it is refused here.
        if (target.contains('%'))
            return std::nullopt;

        // `inet_pton` wants a NUL-terminated string; a view is not one.
        auto const text = std::string { target };

        sockaddr_in v4 {};
        v4.sin_family = AF_INET;
        v4.sin_port = htons(DiscardPort);
        if (::inet_pton(AF_INET, text.c_str(), &v4.sin_addr) == 1)
            return v4;

        sockaddr_in6 v6 {};
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(DiscardPort);
        if (::inet_pton(AF_INET6, text.c_str(), &v6.sin6_addr) == 1)
            return v6;

        return std::nullopt;
    }

    /// @param address The sockaddr of a datagram socket's local end.
    /// @return Its address in `inet_ntop` spelling, no `%scope`, like `QueryLocalAddresses`.
    [[nodiscard]] std::optional<std::string> FormatLocal(ParsedAddress const& family, sockaddr_storage const& local)
    {
        std::array<char, INET6_ADDRSTRLEN> text {};
        auto const* bytes = std::holds_alternative<sockaddr_in>(family)
                                ? static_cast<void const*>(&reinterpret_cast<sockaddr_in const*>(&local)->sin_addr)
                                : static_cast<void const*>(&reinterpret_cast<sockaddr_in6 const*>(&local)->sin6_addr);
        auto const afamily = std::holds_alternative<sockaddr_in>(family) ? AF_INET : AF_INET6;
        if (::inet_ntop(afamily, bytes, text.data(), static_cast<socklen_t>(text.size())) == nullptr)
            return std::nullopt;
        return std::string { text.data() };
    }

    /// `IRouteProbe` over the real kernel.
    class SystemRouteProbe final: public IRouteProbe
    {
      public:
        [[nodiscard]] std::expected<std::string, RouteProbeError> SourceFor(std::string_view target) const override
        {
            auto const parsed = ParseTarget(target);
            if (!parsed.has_value())
                return std::unexpected { RouteProbeError::InvalidTarget };

            // `inet_pton` needs no Winsock state, but the socket below does, and it is started lazily.
            core::platform::ensureWinsockInitialized();

            auto const* destination =
                std::visit([](auto const& address) { return reinterpret_cast<sockaddr const*>(&address); }, *parsed);
            auto const destinationSize =
                std::visit([](auto const& address) { return static_cast<socklen_t>(sizeof(address)); }, *parsed);
            auto const family = std::holds_alternative<sockaddr_in>(*parsed) ? AF_INET : AF_INET6;

            SocketGuard const guard { ::socket(family, SOCK_DGRAM, IPPROTO_UDP) };
            if (guard.Get() == InvalidSocket)
                return std::unexpected { ErrorFrom(LastSocketError()) };

            if (::connect(guard.Get(), destination, destinationSize) != 0)
                return std::unexpected { ErrorFrom(LastSocketError()) };

            sockaddr_storage local {};
            auto localSize = static_cast<socklen_t>(sizeof(local));
            if (::getsockname(guard.Get(), reinterpret_cast<sockaddr*>(&local), &localSize) != 0)
                return std::unexpected { ErrorFrom(LastSocketError()) };

            auto text = FormatLocal(*parsed, local);
            if (!text.has_value())
                return std::unexpected { RouteProbeError::NoRoute };
            return *std::move(text);
        }
    };
} // namespace

std::unique_ptr<IRouteProbe> MakeSystemRouteProbe()
{
    return std::make_unique<SystemRouteProbe>();
}

} // namespace FastCache
