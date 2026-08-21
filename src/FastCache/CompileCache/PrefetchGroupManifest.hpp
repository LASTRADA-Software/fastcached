// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Records which cache keys belong to a build "prefetch group" — the set of compile
/// results produced from one environment/build — so the compile-cache executor
/// can prefetch the rest of a prefetch group disk→memory when one of its members is
/// fetched. The mapping (prefetch-group id → key-set) is stored *in the cache itself*,
/// under a control-byte-prefixed key that cannot collide with a user key, so no
/// new storage interface is required.
///
/// Not a value cache: the manifest is small metadata. Appends are performed via
/// `IStorage::Update` so concurrent writers under a ShardedStorage shard lock
/// accumulate keys atomically.
class PrefetchGroupManifest
{
  public:
    /// Maximum keys tracked per group. A build group is thousands of entries;
    /// this bound guards against unbounded growth from a misused group id. On
    /// reaching it, further AddKey calls for that group are dropped and logged
    /// by the caller — never silently (see AddKey's return).
    static constexpr std::size_t MaxKeysPerGroup = 100'000;

    /// Construct over the storage the manifest lives in.
    /// @param storage Backing storage; non-owning reference, must outlive *this.
    explicit PrefetchGroupManifest(IStorage& storage) noexcept;

    /// Record that `key` belongs to `groupId`. Idempotent: a key already in the
    /// prefetch group is not duplicated. Atomic under the backing storage's per-key
    /// critical section.
    /// @param groupId Opaque prefetch group identifier (e.g. an environment tag).
    /// @param key     The cache key to associate with the prefetch group.
    /// @param now     Current clock value.
    /// @return true if the key was added (or already present), false if the
    ///         prefetch group is at MaxKeysPerGroup and the key was dropped, or
    ///         StorageError on an I/O failure.
    [[nodiscard]] std::expected<bool, StorageError> AddKey(std::string_view groupId, std::string_view key, TimePoint now);

    /// List the keys recorded for `groupId`, in insertion order.
    /// @param groupId Prefetch group identifier.
    /// @param now     Current clock value.
    /// @return The recorded keys (empty if the prefetch group is unknown), or
    ///         StorageError on an I/O failure.
    [[nodiscard]] std::expected<std::vector<std::string>, StorageError> Keys(std::string_view groupId, TimePoint now);

    /// Reverse lookup: the prefetch group a key was recorded under, if any. Populated
    /// by AddKey so a FETCH can discover which prefetch group to prefetch. A key may
    /// belong to at most one prefetch group (the last AddKey wins on re-store).
    /// @param key The cache key.
    /// @param now Current clock value.
    /// @return The prefetch group id, or std::nullopt if the key is not in any prefetch group,
    ///         or StorageError on an I/O failure.
    [[nodiscard]] std::expected<std::optional<std::string>, StorageError> GroupOf(std::string_view key, TimePoint now);

  private:
    /// The storage key the manifest for `groupId` is stored under. Prefixed
    /// with a control byte (0x01) so it cannot collide with a user key.
    /// @param groupId Prefetch group identifier.
    /// @return The internal manifest key.
    [[nodiscard]] static std::string ManifestKey(std::string_view groupId);

    /// The storage key the reverse (key→prefetch group) entry is stored under. Prefixed
    /// with a distinct control byte (0x02) so it collides with neither user
    /// keys nor forward manifest keys.
    /// @param key The cache key.
    /// @return The internal reverse-index key.
    [[nodiscard]] static std::string ReverseKey(std::string_view key);

    IStorage& _storage;
};

} // namespace FastCache
