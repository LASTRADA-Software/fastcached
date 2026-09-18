// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Why a cryptographic primitive in `Core/Ed25519`, `Core/X25519` or `Core/Hkdf` refused.
///
/// **Private to this process: never transmitted, never persisted.** A caller maps a refusal
/// to whatever its own wire or its own log says, so the enumerators carry no explicit values
/// and a new one may go anywhere before `Last` -- the wire-and-protocol rule on declaring an
/// enum's kind at its declaration.
///
/// Every value is a statement about the INPUT, never about the primitive having failed: the
/// primitives underneath (Monocypher's curve arithmetic, this tree's HMAC-SHA256) are total
/// functions, so there is no "the library broke" arm to report and none is invented.
enum class CryptoError : std::uint8_t
{
    WrongKeyLength,          ///< A key or seed was not the length its algorithm fixes (32 bytes for all of them here).
    LowOrderPeerKey,         ///< An X25519 exchange produced the all-zero secret: the peer sent a low-order point.
    OutputTooLong,           ///< An HKDF expansion asked for more than 255 × 32 bytes, which RFC 5869 forbids.
    EmptyOutput,             ///< An HKDF expansion asked for zero bytes, which is a key of nothing.
    PseudoRandomKeyTooShort, ///< An HKDF expansion was handed a PRK shorter than one SHA-256 digest.
    Last,                    ///< Not a refusal, and has no row: the length of a table keyed by one.
};

/// One row per `CryptoError`, naming it for a log line or a test failure.
struct CryptoErrorRow
{
    CryptoError code;      ///< The refusal.
    std::string_view name; ///< Its spelling.
};

/// The spelling of every refusal, in enumerator order.
inline constexpr EnumTable<CryptoError, CryptoErrorRow> CryptoErrorNames { {
    { .code = CryptoError::WrongKeyLength, .name = "WrongKeyLength" },
    { .code = CryptoError::LowOrderPeerKey, .name = "LowOrderPeerKey" },
    { .code = CryptoError::OutputTooLong, .name = "OutputTooLong" },
    { .code = CryptoError::EmptyOutput, .name = "EmptyOutput" },
    { .code = CryptoError::PseudoRandomKeyTooShort, .name = "PseudoRandomKeyTooShort" },
} };

static_assert(RowsInEnumeratorOrder(CryptoErrorNames, &CryptoErrorRow::code),
              "CryptoErrorNames must hold one row per CryptoError, in enumerator order");

/// The spelling of @p code.
/// @param code The refusal.
/// @return Its name, or `"Unknown"` for a value outside the enum.
[[nodiscard]] constexpr std::string_view CryptoErrorName(CryptoError code) noexcept
{
    auto const index = static_cast<std::size_t>(code);
    return index < CryptoErrorNames.size() ? CryptoErrorNames[index].name : std::string_view { "Unknown" };
}

} // namespace FastCache
