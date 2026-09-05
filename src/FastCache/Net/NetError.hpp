// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

/// @file NetError.hpp
/// `Net`'s own error taxonomy, and it lives in `Net/` rather than beside the
/// other four in `Core/Errors/` for one reason: `Net` is meant to be lifted out
/// of this codebase and upstreamed, so it may not reach into `Core/`. Grouping
/// the taxonomies by the fact that they are taxonomies put the widest edge in
/// the tree — thirteen `Net` sources — across the very boundary that has to be
/// severable. Every other error type stays where it is; each of those belongs to
/// a layer that is not going anywhere.

/// Categories of network-layer errors surfaced through std::expected.
/// Kept intentionally small — protocol-level failures use ProtocolError.
enum class NetErrorCode : std::uint8_t
{
    Ok = 0,           ///< Sentinel — not used as an error, but useful for converting from int returns.
    Eof,              ///< Peer closed the connection cleanly.
    Cancelled,        ///< Operation was cancelled (e.g., shutdown, IOCP CancelIoEx).
    Timeout,          ///< Deadline elapsed before completion.
    WouldBlock,       ///< Non-blocking I/O reported no progress and is not coupled to a reactor wakeup.
    BadFileHandle,    ///< Underlying socket/descriptor was closed or invalid.
    AddressInUse,     ///< Bind failed because the endpoint is taken.
    AddressNotAvail,  ///< Bind failed because the address is not available locally.
    ConnRefused,      ///< Peer actively refused the connection.
    ConnReset,        ///< Peer reset the connection mid-flight.
    HostUnreach,      ///< Network reports the destination is unreachable.
    PermissionDenied, ///< OS denied the operation (e.g., low-numbered port without privileges).
    SystemError,      ///< Catch-all for OS errors we do not categorise further. Inspect systemCode.
};

/// Structured network error suitable for use as the E in std::expected<T, NetError>.
struct NetError
{
    NetErrorCode code = NetErrorCode::SystemError;

    /// Native OS error code (errno / GetLastError / WSAGetLastError). Zero if not OS-derived.
    int systemCode = 0;

    /// Free-form context, kept short. Avoid embedding payload data.
    std::string context;

    /// Render a single-line, human-readable description for logs.
    /// @return formatted "code=Eof system=0 context=<...>" style string.
    [[nodiscard]] std::string ToString() const
    {
        return std::format("NetError(code={} system={} context={})", static_cast<unsigned>(code), systemCode, context);
    }
};

/// Whether a failed operation failed because its deadline expired.
///
/// **Two codes, one fact, and which one arrives is the platform's choice.** A
/// receive or send deadline armed with `SO_RCVTIMEO`/`SO_SNDTIMEO` expires as
/// `EAGAIN`/`EWOULDBLOCK` on POSIX and as `WSAETIMEDOUT` on Winsock, so a caller
/// asking "did I run out of time" has to accept both -- and a caller that spells
/// only the obvious one is correct on one platform and silently wrong on the other.
///
/// It lives here, beside the enum, because the question was open-coded
/// ([#824](https://github.com/LASTRADA-Software/fastcached/issues/824)) at TWO sites
/// in two subsystems -- `Server/AdminHttpServer.cpp` and
/// `Consensus/RaftPeerServer.cpp`. A third,
/// `apps/fastcache-compile-node/FrameEndpoint.cpp`, tests `WouldBlock` alone and is
/// RIGHT to -- on an ACCEPT, `WouldBlock` means *no connection is ready yet, retry*,
/// which is not a deadline expiring. **That reason is recorded at the site, not only
/// here**: while it lived only here, three reviewers in a row filed the narrow test
/// as a defect without opening the file. A note the next editor must already know
/// about is a note in the wrong place. It is also the SEMANTIC reason rather than
/// the reachability one -- "that listener has no poll timeout" is a fact about
/// today's wiring and would stop being true the day one is injected.
///
/// The census travels with the pattern that produced it, because a number nobody can
/// reproduce is one the next person re-derives differently. Re-run it rather than
/// trusting the count, which describes the tree at the commit that wrote it:
///
///     git grep -nE '(==|!=) *(NetErrorCode::)?(WouldBlock|Timeout)\b' -- src/
///
/// A dependency-free leaf in `Net/`, so it costs nothing at the `net-boundary` line.
///
/// It says nothing about *whose* deadline: a caller that must tell "I gave up" from
/// "the peer went away" asks its own timer, which is what `SocketDeadlineTarget`
/// is for.
/// @param code The code an operation failed with.
/// @return Whether that code is this platform's spelling of a deadline expiry.
[[nodiscard]] constexpr bool IsDeadlineExpiry(NetErrorCode code) noexcept
{
    return code == NetErrorCode::Timeout || code == NetErrorCode::WouldBlock;
}

/// Convert a NetErrorCode to a stable string name for diagnostics.
/// @param code Code to translate.
/// @return Static string view; never empty.
[[nodiscard]] constexpr std::string_view ToStringView(NetErrorCode code) noexcept
{
    switch (code)
    {
        case NetErrorCode::Ok:
            return "Ok";
        case NetErrorCode::Eof:
            return "Eof";
        case NetErrorCode::Cancelled:
            return "Cancelled";
        case NetErrorCode::Timeout:
            return "Timeout";
        case NetErrorCode::WouldBlock:
            return "WouldBlock";
        case NetErrorCode::BadFileHandle:
            return "BadFileHandle";
        case NetErrorCode::AddressInUse:
            return "AddressInUse";
        case NetErrorCode::AddressNotAvail:
            return "AddressNotAvail";
        case NetErrorCode::ConnRefused:
            return "ConnRefused";
        case NetErrorCode::ConnReset:
            return "ConnReset";
        case NetErrorCode::HostUnreach:
            return "HostUnreach";
        case NetErrorCode::PermissionDenied:
            return "PermissionDenied";
        case NetErrorCode::SystemError:
            return "SystemError";
    }
    return "Unknown";
}

} // namespace FastCache
