// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

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
