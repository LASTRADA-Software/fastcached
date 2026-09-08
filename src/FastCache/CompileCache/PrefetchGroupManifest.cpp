// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PrefetchGroupManifest.hpp>
#include <FastCache/Core/ByteCursor.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache
{
namespace
{

    /// The fewest wire bytes one encoded key can occupy: its length prefix, with an
    /// empty key. Read off `EncodeKeyList` below, and spelled with `FieldPrefixSize`
    /// rather than a literal 4, which would restate the framing contract beside the
    /// one place it is defined.
    constexpr std::size_t MinKeyBytes = WireFields::FieldPrefixSize;

    /// Decode a length-prefixed key list (`[u32 count]{ [u32 len][bytes] }*`) from
    /// the manifest value bytes.
    ///
    /// A buffer that runs out *mid-list* yields the keys decoded so far -- best
    /// effort, because a manifest is a prefetch hint and a truncated one merely
    /// prefetches less. Bytes that are **not a key list at all** are different in kind
    /// and yield nothing: a value too short to hold the count field, or one declaring
    /// a count its remaining bytes cannot supply. Those are not bytes this build
    /// wrote, and the caller has to be able to tell that apart from an empty group,
    /// because on the write path the two lead to opposite actions (issue #267).
    /// @param bytes Encoded manifest value.
    /// @return The decoded keys, or nullopt when the value is not a manifest.
    [[nodiscard]] std::optional<std::vector<std::string>> DecodeKeyList(std::span<std::byte const> bytes)
    {
        std::vector<std::string> keys;
        ByteCursor cursor { bytes };

        // Both refusals are one call now. Either there is not even a count field --
        // `EncodeKeyList` always writes one, so such bytes are no more this build's
        // than an impossible count is, and answering with an empty list would leave
        // `AddKey` overwriting them -- or the count is a claim about bytes this value
        // does not carry (issue #267). `ReadCount` cannot be called without stating
        // what one key costs, which is what makes the second check unforgettable.
        std::uint32_t count {};
        if (!cursor.ReadCount(count, MinKeyBytes))
            return std::nullopt;

        // Reserved against THIS BUILD's own cap rather than the peer's number: a group
        // legitimately runs to `MaxKeysPerGroup`, and this decode happens inside every
        // STORE's read-modify-write, so growing a hundred thousand strings one
        // reallocation at a time is the one cost here big enough to measure.
        keys.reserve(std::min<std::size_t>(count, PrefetchGroupManifest::MaxKeysPerGroup));
        for ([[maybe_unused]] auto const _: std::views::iota(std::uint32_t { 0 }, count))
        {
            // A short tail stops the walk and keeps what was read, which is this
            // decoder's existing tolerance rather than something the conversion
            // introduces: `ReadField` fails the cursor, and breaking here is what
            // turns that into "no more keys" instead of "no keys at all".
            std::string key;
            if (!cursor.ReadField(key))
                break;
            keys.emplace_back(std::move(key));
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

PrefetchGroupManifest::PrefetchGroupManifest(IStorage& storage) noexcept:
    _storage { storage }
{
}

std::string PrefetchGroupManifest::ManifestKey(std::string_view groupId)
{
    // Control-byte prefix keeps the manifest out of the user keyspace (no user
    // key begins with 0x01 in the compile-cache / redis / memcached protocols).
    //
    // The "cohort:" infix is deliberately *not* renamed with the rest of the
    // vocabulary: it is part of the on-disk key, so respelling it would orphan
    // every manifest a warm cache already holds. That degrades rather than
    // corrupts -- a manifest that cannot be found simply means no prefetch, and
    // the next STORE writes it back under the same name -- but it is a cold
    // prefetch window bought for a string no user and no caller ever sees.
    std::string key;
    key.reserve(groupId.size() + 8);
    key.push_back('\x01');
    key += "cohort:";
    key += groupId;
    return key;
}

std::string PrefetchGroupManifest::ReverseKey(std::string_view key)
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

std::expected<bool, StorageError> PrefetchGroupManifest::AddKey(std::string_view groupId,
                                                                std::string_view key,
                                                                TimePoint now)
{
    std::string const manifestKey = ManifestKey(groupId);
    bool added = false;

    auto const result = _storage.Update(
        manifestKey,
        [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
            std::vector<std::string> keys;
            if (current.found)
            {
                auto decoded = DecodeKeyList(current.entry.ValueBytes());
                if (!decoded.has_value())
                    // Refused, never overwritten. Treating an undecodable manifest as
                    // an empty one would have this STORE replace the whole group with
                    // a one-key list -- destroying data on the strength of bytes we
                    // just decided we do not understand. The read path answers the
                    // same way, so one value cannot be a refusal on FETCH and a fresh
                    // start on STORE.
                    return std::unexpected(StorageError { .code = StorageErrorCode::Corrupt,
                                                          .systemCode = 0,
                                                          .context = "prefetch-group manifest is not decodable" });
                keys = *std::move(decoded);
            }

            // Idempotent: do not duplicate a key already recorded.
            if (std::ranges::find(keys, key) != keys.end())
            {
                added = true; // Already present counts as "in the prefetch group".
                return IStorage::UpdateOutcome { .value = {}, .action = IStorage::UpdateAction::Unchanged };
            }
            if (keys.size() >= MaxKeysPerGroup)
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

    // Maintain the reverse (key→prefetch group) index so a FETCH can discover which
    // prefetch group to prefetch. Store the prefetch group id as the value. Best-effort: a
    // reverse-index write failure does not fail AddKey (the forward manifest,
    // which drives correctness, already succeeded).
    if (added)
    {
        std::vector<std::byte> groupBytes;
        auto const* p = reinterpret_cast<std::byte const*>(groupId.data());
        groupBytes.assign(p, p + groupId.size());
        (void) _storage.Set(ReverseKey(key), std::move(groupBytes), /*flags=*/0, TimePoint::max());
    }
    return added;
}

std::expected<std::optional<std::string>, StorageError> PrefetchGroupManifest::GroupOf(std::string_view key, TimePoint now)
{
    auto const got = _storage.Peek(ReverseKey(key), now);
    if (!got.has_value())
        return std::unexpected(got.error());
    if (!got->found)
        return std::optional<std::string> {};
    auto const bytes = got->entry.ValueBytes();
    return std::optional<std::string> { std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() } };
}

std::expected<std::vector<std::string>, StorageError> PrefetchGroupManifest::Keys(std::string_view groupId, TimePoint now)
{
    auto const got = _storage.Peek(ManifestKey(groupId), now);
    if (!got.has_value())
        return std::unexpected(got.error());
    if (!got->found)
        return std::vector<std::string> {};

    auto decoded = DecodeKeyList(got->entry.ValueBytes());
    if (!decoded.has_value())
        // `Corrupt` is exactly the enumerator for "this store holds bytes this build
        // did not write", and it is a distinct answer from the empty list an unknown
        // group returns -- which is the distinction that makes the refusal visible
        // rather than looking like a cold prefetch group.
        return std::unexpected(StorageError {
            .code = StorageErrorCode::Corrupt, .systemCode = 0, .context = "prefetch-group manifest is not decodable" });
    return *std::move(decoded);
}

} // namespace FastCache
