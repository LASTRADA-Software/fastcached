// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Command-level facade over IStorage. Owns the TTL semantics (translating
/// memcached's seconds-or-unix-timestamp `exptime` into absolute TimePoints
/// via IClock) and the encoding-specific conversions. The protocol handlers
/// call methods on CacheEngine; they never call IStorage directly.
class CacheEngine
{
  public:
    /// Construct over an IStorage and IClock; both must outlive the engine.
    /// @param storage Backing storage.
    /// @param clock Time provider.
    CacheEngine(IStorage& storage, IClock& clock) noexcept;

    // -- memcached-flavoured operations -------------------------------------

    /// memcached `exptime` semantics:
    ///   0           -> never expires (TimePoint::max())
    ///   1..2592000  -> relative seconds from now
    ///   >2592000    -> absolute UNIX timestamp (we approximate by using
    ///                 monotonic now + (timestamp - approx-wall-now), good
    ///                 enough for sccache cache TTLs)
    /// @param exptime Wire-format expiry word.
    /// @return Absolute steady-clock deadline.
    [[nodiscard]] TimePoint ExpiryFromExptime(std::uint32_t exptime) const noexcept;

    [[nodiscard]] std::expected<GetResult, StorageError> Get(std::string_view key);

    /// Non-mutating lookup: like `Get` but does not promote the LRU, advance
    /// `lastAccess`, or bump hit/miss stats. Used by the meta handlers to read
    /// metadata (size / TTL / CAS) and to honour the `u` (no-bump) flag
    /// without recording a client access.
    /// @param key Lookup key.
    /// @return GetResult, or StorageError on I/O failure.
    [[nodiscard]] std::expected<GetResult, StorageError> Peek(std::string_view key);

    [[nodiscard]] std::expected<CasToken, StorageError> Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            std::uint32_t exptime);

    [[nodiscard]] std::expected<CasToken, StorageError> Add(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            std::uint32_t exptime);

    [[nodiscard]] std::expected<CasToken, StorageError> Replace(std::string_view key,
                                                                std::vector<std::byte> value,
                                                                std::uint32_t flags,
                                                                std::uint32_t exptime);

    /// Append `suffix` to the existing value. `expected` is an optional CAS
    /// precondition (0 = unconditional; drives meta `ms ... MA C(token)`).
    [[nodiscard]] std::expected<CasToken, StorageError> Append(std::string_view key,
                                                               std::span<std::byte const> suffix,
                                                               CasToken expected = 0);
    /// Prepend `prefix` to the existing value. `expected` is an optional CAS
    /// precondition (0 = unconditional; drives meta `ms ... MP C(token)`).
    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                CasToken expected = 0);

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(
        std::string_view key, CasToken expected, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime);

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> Increment(std::string_view key, std::uint64_t delta);
    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> Decrement(std::string_view key, std::uint64_t delta);

    /// Guarded atomic read-modify-write over the entry under `key`. Forwards to
    /// `IStorage::Update`, supplying the current clock; the backend runs `fn`
    /// within its per-key atomicity boundary. Used for compound mutations such
    /// as the redis set commands and `INCRBYFLOAT`.
    /// @param key Lookup key.
    /// @param fn  Callback given the current GetResult; returns the new outcome
    ///            or a StorageError to abort without mutating.
    /// @return New CAS token, or the callback's/storage's StorageError.
    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<IStorage::UpdateOutcome, StorageError>(GetResult const&)> const& fn);

    // -- redis set type (SADD/SREM/SMEMBERS/SISMEMBER/SMISMEMBER/SCARD/SPOP) --
    //
    // A set is stored as an ordinary value blob tagged with FcTypeSet in the
    // entry flags; the members are sorted and length-prefixed (see SetCodec).
    // The mutating ops go through Update so decode-modify-encode is atomic.

    /// Add `members` to the set at `key` (creating it if absent).
    /// @return Number of members newly added, or WrongType if `key` is a string.
    [[nodiscard]] std::expected<std::int64_t, StorageError> SetAdd(std::string_view key,
                                                                   std::span<std::string const> members);

    /// Remove `members` from the set at `key`; deletes the key if it empties.
    /// @return Number of members actually removed, or WrongType on a non-set key.
    [[nodiscard]] std::expected<std::int64_t, StorageError> SetRemove(std::string_view key,
                                                                      std::span<std::string const> members);

    /// @return All members of the set at `key` (empty if absent), or WrongType.
    [[nodiscard]] std::expected<std::vector<std::string>, StorageError> SetMembers(std::string_view key);

    /// @return True if `member` is in the set at `key`, or WrongType.
    [[nodiscard]] std::expected<bool, StorageError> SetIsMember(std::string_view key, std::string_view member);

    /// @return One bool per `members` entry (membership), or WrongType.
    [[nodiscard]] std::expected<std::vector<bool>, StorageError> SetMIsMember(std::string_view key,
                                                                              std::span<std::string const> members);

    /// @return Cardinality of the set at `key` (0 if absent), or WrongType.
    [[nodiscard]] std::expected<std::int64_t, StorageError> SetCard(std::string_view key);

    /// Remove and return up to `count` members from the set at `key`.
    /// @return The popped members (empty if absent), or WrongType.
    [[nodiscard]] std::expected<std::vector<std::string>, StorageError> SetPop(std::string_view key, std::size_t count);

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key);

    /// Refresh the entry's expiry without rewriting its value. Wraps
    /// `IStorage::Touch` and translates `exptime` via `ExpiryFromExptime`.
    /// @param key     Lookup key.
    /// @param exptime Wire-format expiry word (see `ExpiryFromExptime`).
    /// @return New CAS token, or StorageError(KeyNotFound).
    [[nodiscard]] std::expected<CasToken, StorageError> Touch(std::string_view key, std::uint32_t exptime);

    /// Atomic get-and-touch: refresh the entry's expiry and return the
    /// refreshed entry in one critical section (memcached `gat`/`gats` and
    /// meta `mg ... T`). Closes the TOCTOU window of a separate Touch+Get.
    /// @param key     Lookup key.
    /// @param exptime Wire-format expiry word (see `ExpiryFromExptime`).
    /// @return The refreshed GetResult, or StorageError(KeyNotFound) on miss.
    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key, std::uint32_t exptime);

    /// Atomic compare-and-delete: delete `key` only if its CAS equals
    /// `expected`, as a single critical section (meta `md C(token)`).
    /// @param key      Lookup key.
    /// @param expected CAS token the caller believes is current.
    /// @return Empty on success, KeyNotFound if absent, CasMismatch on CAS mismatch.
    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key, CasToken expected);

    /// Mark the entry stale without removing it (meta `md I` / `ms I`),
    /// optionally refreshing its TTL at the same time (`md I T(token)`).
    /// @param key        Lookup key.
    /// @param newExptime Wire-format expiry word to apply, or std::nullopt
    ///                   to leave the existing expiry untouched.
    /// @return New CAS token, or StorageError(KeyNotFound) if absent.
    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<std::uint32_t> newExptime);

    /// Flush all entries; `delaySeconds == 0` is immediate.
    void FlushAll(std::uint32_t delaySeconds);

    /// Reconfigure the storage byte budget at runtime. Wraps
    /// `IStorage::Resize` for use by `cache_memlimit`.
    /// @param newMaxBytes Total byte budget across all shards.
    void Resize(std::size_t newMaxBytes)
    {
        _storage.Resize(newMaxBytes);
    }

    [[nodiscard]] StorageStats Snapshot() const noexcept
    {
        return _storage.Snapshot();
    }

    [[nodiscard]] IClock& Clock() noexcept
    {
        return _clock;
    }

  private:
    IStorage& _storage;
    IClock& _clock;
};

} // namespace FastCache
