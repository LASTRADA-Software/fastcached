// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace CowTree
{

/// Categories of errors produced by the CowTree library.
///
/// Mapped 1:1 from the kind of failure rather than from the underlying OS
/// error so consumers can react without parsing strings. `systemCode`
/// inside CowTreeError carries the platform errno / GetLastError for
/// callers who need the original cause.
enum class CowTreeError : std::uint8_t
{
    Ok = 0,        ///< Sentinel, never returned in an unexpected().
    NotFound,      ///< Key absent / page id never allocated.
    IoError,       ///< Underlying file or store I/O failed.
    Corrupt,       ///< CRC or layout invariant violation on read.
    CorruptMetas,  ///< Both meta pages failed CRC; tree is unrecoverable.
    OutOfRange,    ///< Page id, slot index, or value-size argument out of range.
    NotOpen,       ///< Operation requires Open() to have been called first.
    AlreadyOpen,   ///< Open() called twice.
    InvalidArg,    ///< Caller passed a malformed argument (e.g. wrong page-size buffer).
    ValueTooLarge, ///< Single key+value pair does not fit in a page.
    InjectedFault, ///< Test-only: a failure was injected by the page-store fault harness.
    Unsupported,   ///< Operation not supported by this implementation.
};

/// Stable name for a CowTreeError, suitable for diagnostic logging.
/// @param e Error code.
/// @return Static string view; never empty.
[[nodiscard]] constexpr std::string_view ToStringView(CowTreeError e) noexcept
{
    switch (e)
    {
        case CowTreeError::Ok:
            return "Ok";
        case CowTreeError::NotFound:
            return "NotFound";
        case CowTreeError::IoError:
            return "IoError";
        case CowTreeError::Corrupt:
            return "Corrupt";
        case CowTreeError::CorruptMetas:
            return "CorruptMetas";
        case CowTreeError::OutOfRange:
            return "OutOfRange";
        case CowTreeError::NotOpen:
            return "NotOpen";
        case CowTreeError::AlreadyOpen:
            return "AlreadyOpen";
        case CowTreeError::InvalidArg:
            return "InvalidArg";
        case CowTreeError::ValueTooLarge:
            return "ValueTooLarge";
        case CowTreeError::InjectedFault:
            return "InjectedFault";
        case CowTreeError::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

} // namespace CowTree
