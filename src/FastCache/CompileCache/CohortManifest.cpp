// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CohortManifest.hpp>
#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>

namespace FastCache
{
namespace
{

    /// Decode a length-prefixed key list (`[u32 count]{ [u32 len][bytes] }*`) from
    /// the manifest value bytes. A malformed/short buffer yields the keys decoded
    /// so far (best-effort; the manifest is metadata, not client data).
    /// @param bytes Encoded manifest value.
    /// @return The decoded keys.
    [[nodiscard]] std::vector<std::string> DecodeKeyList(std::span<std::byte const> bytes)
    {
        std::vector<std::string> keys;
        std::size_t pos = 0;
        auto const readU32 = [&](std::uint32_t& out) -> bool {
            if (bytes.size() - pos < sizeof(std::uint32_t))
                return false;
            out = ReadBigEndian<std::uint32_t>(bytes.subspan(pos, sizeof(std::uint32_t)));
            pos += sizeof(std::uint32_t);
            return true;
        };

        std::uint32_t count {};
        if (!readU32(count))
            return keys;
        keys.reserve(count);
        for ([[maybe_unused]] auto const _: std::views::iota(std::uint32_t { 0 }, count))
        {
            std::uint32_t len {};
            if (!readU32(len))
                break;
            if (bytes.size() - pos < len)
                break;
            auto const chunk = bytes.subspan(pos, len);
            keys.emplace_back(reinterpret_cast<char const*>(chunk.data()), len);
            pos += len;
        }
        return keys;
    }

    /// Encode a key list to manifest value bytes (inverse of DecodeKeyList).
    /// @param keys Keys to encode.
    /// @return Encoded bytes.
    [[nodiscard]] std::vector<std::byte> EncodeKeyList(std::vector<std::string> const& keys)
    {
        auto const appendU32 = [](std::vector<std::byte>& out, std::uint32_t n) {
            std::array<std::byte, sizeof(std::uint32_t)> buf {};
            WriteBigEndian<std::uint32_t>(buf, n);
            out.insert(out.end(), buf.begin(), buf.end());
        };

        std::vector<std::byte> out;
        appendU32(out, static_cast<std::uint32_t>(keys.size()));
        for (auto const& key: keys)
        {
            appendU32(out, static_cast<std::uint32_t>(key.size()));
            auto const* p = reinterpret_cast<std::byte const*>(key.data());
            out.insert(out.end(), p, p + key.size());
        }
        return out;
    }

} // namespace

CohortManifest::CohortManifest(IStorage& storage) noexcept:
    _storage { storage }
{
}

std::string CohortManifest::ManifestKey(std::string_view cohortId)
{
    // Control-byte prefix keeps the manifest out of the user keyspace (no user
    // key begins with 0x01 in the compile-cache / redis / memcached protocols).
    std::string key;
    key.reserve(cohortId.size() + 8);
    key.push_back('\x01');
    key += "cohort:";
    key += cohortId;
    return key;
}

std::string CohortManifest::ReverseKey(std::string_view key)
{
    // A distinct control byte (0x02) separates the reverse index from both
    // user keys and forward manifest keys.
    std::string out;
    out.reserve(key.size() + 8);
    out.push_back('\x02');
    out += "member:";
    out += key;
    return out;
}

std::expected<bool, StorageError> CohortManifest::AddKey(std::string_view cohortId, std::string_view key, TimePoint now)
{
    std::string const manifestKey = ManifestKey(cohortId);
    bool added = false;

    auto const result = _storage.Update(
        manifestKey,
        [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
            std::vector<std::string> keys;
            if (current.found)
                keys = DecodeKeyList(current.entry.ValueBytes());

            // Idempotent: do not duplicate a key already recorded.
            if (std::ranges::find(keys, key) != keys.end())
            {
                added = true; // Already present counts as "in the cohort".
                return IStorage::UpdateOutcome { .value = {}, .action = IStorage::UpdateAction::Unchanged };
            }
            if (keys.size() >= MaxKeysPerCohort)
            {
                added = false; // At cap — drop; caller logs.
                return IStorage::UpdateOutcome { .value = {}, .action = IStorage::UpdateAction::Unchanged };
            }

            keys.emplace_back(key);
            added = true;
            return IStorage::UpdateOutcome { .value = EncodeKeyList(keys),
                                             .flags = 0,
                                             .action = IStorage::UpdateAction::Store };
        },
        now);

    if (!result.has_value())
        return std::unexpected(result.error());

    // Maintain the reverse (key→cohort) index so a FETCH can discover which
    // cohort to prefetch. Store the cohort id as the value. Best-effort: a
    // reverse-index write failure does not fail AddKey (the forward manifest,
    // which drives correctness, already succeeded).
    if (added)
    {
        std::vector<std::byte> cohortBytes;
        auto const* p = reinterpret_cast<std::byte const*>(cohortId.data());
        cohortBytes.assign(p, p + cohortId.size());
        (void) _storage.Set(ReverseKey(key), std::move(cohortBytes), /*flags=*/0, TimePoint::max());
    }
    return added;
}

std::expected<std::optional<std::string>, StorageError> CohortManifest::CohortOf(std::string_view key, TimePoint now)
{
    auto const got = _storage.Peek(ReverseKey(key), now);
    if (!got.has_value())
        return std::unexpected(got.error());
    if (!got->found)
        return std::optional<std::string> {};
    auto const bytes = got->entry.ValueBytes();
    return std::optional<std::string> { std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() } };
}

std::expected<std::vector<std::string>, StorageError> CohortManifest::Keys(std::string_view cohortId, TimePoint now)
{
    auto const got = _storage.Peek(ManifestKey(cohortId), now);
    if (!got.has_value())
        return std::unexpected(got.error());
    if (!got->found)
        return std::vector<std::string> {};
    return DecodeKeyList(got->entry.ValueBytes());
}

} // namespace FastCache
