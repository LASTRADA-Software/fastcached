// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
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
    NotANumber,      ///< Value is not a valid integer/float for a numeric op (incr/incrbyfloat).
    WrongType,       ///< Operation against a key holding a different value type (redis WRONGTYPE).
    InfiniteOrNaN,   ///< Numeric op would produce a non-finite result (overflow / NaN); cf. redis incrbyfloat.

    /// On-disk store is intact, but carries a record format this build does
    /// not write.
    ///
    /// Deliberately not `Corrupt`, which is how this used to be spelled. The
    /// two call for opposite responses: `Corrupt` means the bytes are damaged
    /// and there is nothing to recover, while this means a healthy store of a
    /// different vintage — data an operator can still convert, and must not be
    /// told to delete. The code is what monitoring and every programmatic
    /// caller sees, so which of the two happened cannot live only in the
    /// message text.
    ///
    /// Appended rather than slotted in beside `Corrupt` so that no existing
    /// enumerator's value moves. `StorageError::ToString` names the code rather
    /// than numbering it, so a fresh log line no longer carries an ordinal at
    /// all — but one written by an older build does, and an operator alerting on
    /// that number would otherwise silently start matching a different condition
    /// after an upgrade.
    UnsupportedFormatVersion,

    /// Another process holds this store open.
    ///
    /// Deliberately not `IoError`: the file is intact and the remedy is the
    /// other process or a different path, never a repair. An operator sent to
    /// inspect a healthy store is most of what made the silent version of this
    /// bug expensive.
    ///
    /// Appended for the same reason as the enumerator above, and the reason
    /// survives even though `ToString` now names the code rather than
    /// numbering it: a log line written by an OLDER build still carries an
    /// ordinal, so moving an existing enumerator would retroactively change
    /// what those lines appear to say.
    InUse,

    /// A stored value did not decode as the type its flags claim.
    ///
    /// **Deliberately not `Corrupt`, and this one is reachable on demand.** A set or
    /// a stream is an ordinary value blob distinguished only by its `flags` word, and
    /// the memcached text verbs let any client choose that word -- so a client can
    /// plant six bytes, tag them as a set, and read them back with SMEMBERS. Reporting
    /// that as `Corrupt` told an operator their DISK had failed, on a store whose every
    /// record still verified: the rule `.agent/rules/storage.md` records about an old
    /// store applies here with more force, because this is an INPUT rather than a rare
    /// migration edge, and `Corrupt` is what gets a healthy cache deleted.
    ///
    /// The remedies are opposite, which is the test for whether two conditions may
    /// share a code. `Corrupt` means the bytes on disk are damaged and there is
    /// nothing to recover. This means the bytes are exactly what somebody wrote and
    /// they are not a well-formed value of that type -- nothing is wrong with the
    /// store, and the thing to look at is the client.
    ///
    /// **A value codec could never honestly report `Corrupt` in the first place**,
    /// and that is the sentence to keep: integrity is verified BELOW those codecs.
    /// `CowTreeStorage` checks CRC32C and `Core/Compression` checks its own framing,
    /// and each returns `Corrupt` itself before a byte reaches a decoder. So by the
    /// time `SetCodec::Decode` or `StreamCodec::Decode` runs, the bytes are known to
    /// be the bytes somebody stored -- the only remaining question is whether they
    /// are a well-formed value, which is this code and nothing else.
    ///
    /// It is also **not** a persistence failure, and that omission is load-bearing
    /// rather than incidental. `Corrupt` IS one, so a malformed set moved
    /// `fastcached_write_errors_total` and wrote a `storage write failed` warning --
    /// an unprivileged client driving the disk-failure signal at will, which is the
    /// mechanism that would actually walk an operator to deleting a healthy cache.
    /// `WriteErrorReportingStorage::IsPersistenceFailure` therefore leaves this code
    /// out, deliberately. It has a counter of its own (`CacheMalformedValues`)
    /// because an absence of signal would be the other way to get this wrong.
    ///
    /// Distinct from `WrongType`, which is the neighbouring answer and a different
    /// question: `WrongType` is "the flags say string and you asked for a set", while
    /// this is "the flags say set and the bytes are not one".
    ///
    /// Appended rather than slotted in beside `Corrupt`, for the reason the two
    /// enumerators above record: no existing enumerator's value may move, because a log
    /// line written by an older build carries an ordinal.
    MalformedValue,
};

/// Stable name for a StorageErrorCode, suitable for diagnostic logging.
///
/// Declared above `StorageError` so `ToString()` can name the code rather than
/// print its ordinal: an operator reading `code=7` learns nothing, and the
/// number is not even stable — it shifts whenever an enumerator is inserted.
/// @param code The storage error category.
/// @return Static string view; never empty.
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
        case StorageErrorCode::InUse:
            return "InUse";
        case StorageErrorCode::ReadOnly:
            return "ReadOnly";
        case StorageErrorCode::InvalidArgument:
            return "InvalidArgument";
        case StorageErrorCode::NotANumber:
            return "NotANumber";
        case StorageErrorCode::WrongType:
            return "WrongType";
        case StorageErrorCode::InfiniteOrNaN:
            return "InfiniteOrNaN";
        case StorageErrorCode::UnsupportedFormatVersion:
            return "UnsupportedFormatVersion";
        case StorageErrorCode::MalformedValue:
            return "MalformedValue";
    }
    return "Unknown";
}

/// Structured storage error.
struct StorageError
{
    StorageErrorCode code = StorageErrorCode::IoError;

    /// Native OS error code if applicable. Zero otherwise.
    int systemCode = 0;

    std::string context;

    [[nodiscard]] std::string ToString() const
    {
        return std::format("StorageError(code={} system={} context={})", ToStringView(code), systemCode, context);
    }
};

/// Build a `StorageError` carrying only a code (no system code or context).
/// Centralizes the full field initialization in one place so call sites stay
/// terse without tripping `-Wmissing-designated-field-initializers`.
/// @param code The storage error category.
/// @return A `StorageError` with `code` set and the remaining fields defaulted.
[[nodiscard]] inline StorageError MakeStorageError(StorageErrorCode code) noexcept
{
    return StorageError { .code = code, .systemCode = 0, .context = {} };
}

/// A value that did not decode as the type its flags claim.
///
/// Spelled once, here, rather than at each codec: the whole of #296 was four call
/// sites each choosing a code for an outcome a `bool` could not carry, and three of
/// them choosing `Corrupt`. A factory that already knows the code is what stops the
/// fifth site choosing again.
/// @param context What failed to decode and why -- shown to an operator, so name the
///        format and the specific claim the bytes did not honour.
/// @return A `StorageError` carrying `MalformedValue`.
[[nodiscard]] inline StorageError MakeMalformedValueError(std::string context) noexcept
{
    return StorageError { .code = StorageErrorCode::MalformedValue, .systemCode = 0, .context = std::move(context) };
}

} // namespace FastCache
