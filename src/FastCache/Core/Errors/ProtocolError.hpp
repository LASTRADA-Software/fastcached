// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of protocol-level errors (memcached text / memcached binary / Redis RESP).
enum class ProtocolErrorCode : std::uint8_t
{
    Ok = 0,             ///< Sentinel — not used as an error.
    MalformedFrame,     ///< The framer could not separate one message from the byte stream.
    LineTooLong,        ///< A delimited line exceeded the configured maximum.
    PayloadTooLarge,    ///< A length-prefixed payload exceeded the configured maximum.
    InvalidCommand,     ///< Command opcode/keyword is not recognised.
    InvalidArguments,   ///< Recognised command with malformed arguments (e.g., non-numeric ttl).
    InvalidKey,         ///< Key violates the protocol's key-shape rules.
    InvalidValue,       ///< Value/payload does not match declared length or encoding.
    UnsupportedFeature, ///< Recognised but unsupported (e.g., RESP3 HELLO before MVP supports it).
    AuthRequired,       ///< Operation requires authentication that is not provided.
    Truncated,          ///< Peer closed mid-frame.
};

/// Structured protocol error.
struct ProtocolError
{
    ProtocolErrorCode code = ProtocolErrorCode::MalformedFrame;
    std::string context;

    /// Render for logs.
    [[nodiscard]] std::string ToString() const
    {
        return std::format("ProtocolError(code={} context={})", static_cast<unsigned>(code), context);
    }
};

/// Convert a ProtocolErrorCode to a stable string name.
[[nodiscard]] constexpr std::string_view ToStringView(ProtocolErrorCode code) noexcept
{
    switch (code)
    {
        case ProtocolErrorCode::Ok:
            return "Ok";
        case ProtocolErrorCode::MalformedFrame:
            return "MalformedFrame";
        case ProtocolErrorCode::LineTooLong:
            return "LineTooLong";
        case ProtocolErrorCode::PayloadTooLarge:
            return "PayloadTooLarge";
        case ProtocolErrorCode::InvalidCommand:
            return "InvalidCommand";
        case ProtocolErrorCode::InvalidArguments:
            return "InvalidArguments";
        case ProtocolErrorCode::InvalidKey:
            return "InvalidKey";
        case ProtocolErrorCode::InvalidValue:
            return "InvalidValue";
        case ProtocolErrorCode::UnsupportedFeature:
            return "UnsupportedFeature";
        case ProtocolErrorCode::AuthRequired:
            return "AuthRequired";
        case ProtocolErrorCode::Truncated:
            return "Truncated";
    }
    return "Unknown";
}

} // namespace FastCache
