// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache
{

/// Why a route probe produced no address.
enum class RouteProbeError : std::uint8_t
{
    InvalidTarget, ///< The target is not an IPv4 or IPv6 address literal.
    NoRoute,       ///< The kernel has no route to the target, or would not say which address it would use.
    Unsupported,   ///< This machine has no stack for the target's address family.
    Last,          ///< Not an answer: the length of a table keyed by one.
};

/// Asks the kernel which local address it would send from to reach a host. Sends nothing.
///
/// A seam, because the answer is whichever network the machine happens to be on: the rules built
/// on it -- when an address is considered to have moved, what a missing route means -- are
/// assertable only against a fake that can change its mind.
class IRouteProbe
{
  public:
    IRouteProbe() = default;
    IRouteProbe(IRouteProbe const&) = delete;
    IRouteProbe& operator=(IRouteProbe const&) = delete;
    IRouteProbe(IRouteProbe&&) = delete;
    IRouteProbe& operator=(IRouteProbe&&) = delete;
    virtual ~IRouteProbe() = default;

    /// @param target An IPv4 or IPv6 LITERAL (no name resolution, no port).
    /// @return The local address in `inet_ntop` spelling without `%scope`, or why none.
    [[nodiscard]] virtual std::expected<std::string, RouteProbeError> SourceFor(std::string_view target) const = 0;
};

/// The real kernel: UDP socket, `connect()` to target:9, `getsockname()`, close. Never sends.
/// @return The probe; never null.
[[nodiscard]] std::unique_ptr<IRouteProbe> MakeSystemRouteProbe();

/// The fixed targets that only consult the default route: TEST-NET-1 and the documentation prefix.
inline constexpr std::array<std::string_view, 2> DefaultRouteProbeTargets { "192.0.2.1", "2001:db8::1" };

} // namespace FastCache
