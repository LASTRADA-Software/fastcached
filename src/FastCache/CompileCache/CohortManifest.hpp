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

/// Records which cache keys belong to a build "cohort" — the set of compile
/// results produced from one environment/build — so the compile-cache executor
/// can prefetch the rest of a cohort disk→memory when one of its members is
/// fetched. The mapping (cohort-id → key-set) is stored *in the cache itself*,
/// under a control-byte-prefixed key that cannot collide with a user key, so no
/// new storage interface is required.
///
/// Not a value cache: the manifest is small metadata. Appends are performed via
/// `IStorage::Update` so concurrent writers under a ShardedStorage shard lock
/// accumulate keys atomically.
class CohortManifest
{
  public:
    /// Maximum keys tracked per cohort. A build cohort is thousands of entries;
    /// this bound guards against unbounded growth from a misused cohort id. On
    /// reaching it, further AddKey calls for that cohort are dropped and logged
    /// by the caller — never silently (see AddKey's return).
    static constexpr std::size_t MaxKeysPerCohort = 100'000;

    /// Construct over the storage the manifest lives in.
    /// @param storage Backing storage; non-owning reference, must outlive *this.
    explicit CohortManifest(IStorage& storage) noexcept;

    /// Record that `key` belongs to `cohortId`. Idempotent: a key already in the
    /// cohort is not duplicated. Atomic under the backing storage's per-key
    /// critical section.
    /// @param cohortId Opaque cohort identifier (e.g. an environment tag).
    /// @param key      The cache key to associate with the cohort.
    /// @param now      Current clock value.
    /// @return true if the key was added (or already present), false if the
    ///         cohort is at MaxKeysPerCohort and the key was dropped, or
    ///         StorageError on an I/O failure.
    [[nodiscard]] std::expected<bool, StorageError> AddKey(std::string_view cohortId, std::string_view key, TimePoint now);

    /// List the keys recorded for `cohortId`, in insertion order.
    /// @param cohortId Cohort identifier.
    /// @param now      Current clock value.
    /// @return The recorded keys (empty if the cohort is unknown), or
    ///         StorageError on an I/O failure.
    [[nodiscard]] std::expected<std::vector<std::string>, StorageError> Keys(std::string_view cohortId, TimePoint now);

    /// Reverse lookup: the cohort a key was recorded under, if any. Populated
    /// by AddKey so a FETCH can discover which cohort to prefetch. A key may
    /// belong to at most one cohort (the last AddKey wins on re-store).
    /// @param key The cache key.
    /// @param now Current clock value.
    /// @return The cohort id, or std::nullopt if the key is not in any cohort,
    ///         or StorageError on an I/O failure.
    [[nodiscard]] std::expected<std::optional<std::string>, StorageError> CohortOf(std::string_view key, TimePoint now);

  private:
    /// The storage key the manifest for `cohortId` is stored under. Prefixed
    /// with a control byte (0x01) so it cannot collide with a user key.
    /// @param cohortId Cohort identifier.
    /// @return The internal manifest key.
    [[nodiscard]] static std::string ManifestKey(std::string_view cohortId);

    /// The storage key the reverse (key→cohort) entry is stored under. Prefixed
    /// with a distinct control byte (0x02) so it collides with neither user
    /// keys nor forward manifest keys.
    /// @param key The cache key.
    /// @return The internal reverse-index key.
    [[nodiscard]] static std::string ReverseKey(std::string_view key);

    IStorage& _storage;
};

} // namespace FastCache
