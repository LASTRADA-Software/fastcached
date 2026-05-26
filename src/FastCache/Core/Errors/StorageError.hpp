// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of storage-layer errors (in-memory LRU + on-disk log).
enum class StorageErrorCode : std::uint8_t
{
    Ok = 0,          ///< Sentinel.
    KeyNotFound,     ///< Lookup miss — not always an error to callers, but storage reports it.
    KeyExists,       ///< add() failed because the key was present.
    CasMismatch,     ///< CAS token did not match the stored value.
    ValueTooLarge,   ///< Value would exceed the configured per-entry maximum.
    OutOfMemory,     ///< In-memory budget exhausted after eviction.
    Corrupt,         ///< On-disk record failed CRC32C verification.
    IoError,         ///< Underlying file I/O failed (inspect systemCode).
    ReadOnly,        ///< Storage is open in a read-only mode (e.g., crash-recovery replay).
    InvalidArgument, ///< Caller passed nonsensical arguments (e.g., negative ttl).
};

/// Structured storage error.
struct StorageError
{
    StorageErrorCode code = StorageErrorCode::IoError;

    /// Native OS error code if applicable. Zero otherwise.
    int systemCode = 0;

    std::string context;

    [[nodiscard]] std::string ToString() const
    {
        return std::format("StorageError(code={} system={} context={})", static_cast<unsigned>(code), systemCode, context);
    }
};

[[nodiscard]] constexpr std::string_view ToStringView(StorageErrorCode code) noexcept
{
    switch (code)
    {
        case StorageErrorCode::Ok:
            return "Ok";
        case StorageErrorCode::KeyNotFound:
            return "KeyNotFound";
        case StorageErrorCode::KeyExists:
            return "KeyExists";
        case StorageErrorCode::CasMismatch:
            return "CasMismatch";
        case StorageErrorCode::ValueTooLarge:
            return "ValueTooLarge";
        case StorageErrorCode::OutOfMemory:
            return "OutOfMemory";
        case StorageErrorCode::Corrupt:
            return "Corrupt";
        case StorageErrorCode::IoError:
            return "IoError";
        case StorageErrorCode::ReadOnly:
            return "ReadOnly";
        case StorageErrorCode::InvalidArgument:
            return "InvalidArgument";
    }
    return "Unknown";
}

} // namespace FastCache
