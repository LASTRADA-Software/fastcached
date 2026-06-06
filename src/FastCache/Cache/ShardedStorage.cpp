// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace FastCache
{

ShardedStorage::ShardedStorage(std::vector<std::unique_ptr<IStorage>> shards)
{
    if (shards.empty())
        throw std::invalid_argument { "ShardedStorage requires at least one shard" };
    _shards.reserve(shards.size());
    for (auto& s: shards)
    {
        auto shard = std::make_unique<Shard>();
        shard->storage = std::move(s);
        _shards.push_back(std::move(shard));
    }
}

std::size_t ShardedStorage::ShardIndexFor(std::string_view key) const noexcept
{
    return std::hash<std::string_view> {}(key) % _shards.size();
}

std::expected<GetResult, StorageError> ShardedStorage::Get(std::string_view key, TimePoint now)
{
    FC_ZONE_SCOPED_N("ShardedStorage::Get");
    auto& shard = *_shards[ShardIndexFor(key)];

    // Backends whose Get mutates shared state on read (Strict-mode LRU splice,
    // CowTree page touch) need an exclusive lock. Backends that report
    // SupportsSharedRead() perform a race-free read under a shared lock, so
    // concurrent same-shard reads run in parallel — the multi-connection win.
    if (!shard.storage->SupportsSharedRead())
    {
        std::unique_lock const lock { shard.mu };
        return shard.storage->Get(key, now);
    }

    auto result = [&] {
        std::shared_lock const lock { shard.mu };
        return shard.storage->Get(key, now);
    }();

    // Sampled, deferred LRU promotion: on a fraction of hits, take a brief
    // exclusive lock to splice the entry to MRU and advance its access time —
    // the work the shared read skipped. Most reads pay nothing; the LRU stays
    // approximately correct (memcached-style). The shared lock is released
    // before the exclusive one is taken (std::shared_mutex has no upgrade).
    if (result.has_value() && result->found)
    {
        constexpr unsigned kPromoteEveryN = 16;
        if (shard.readSampler.fetch_add(1, std::memory_order_relaxed) % kPromoteEveryN == 0)
        {
            std::unique_lock const lock { shard.mu };
            shard.storage->PromoteOnRead(key, now);
        }
    }
    return result;
}

std::expected<CasToken, StorageError> ShardedStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
    FC_ZONE_SCOPED_N("ShardedStorage::Set");
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> ShardedStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Add(key, std::move(value), flags, expiry, now);
}

std::expected<CasToken, StorageError> ShardedStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Replace(key, std::move(value), flags, expiry, now);
}

std::expected<CasToken, StorageError> ShardedStorage::Append(std::string_view key,
                                                             std::span<std::byte const> suffix,
                                                             CasToken expected,
                                                             TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Append(key, suffix, expected, now);
}

std::expected<CasToken, StorageError> ShardedStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              CasToken expected,
                                                              TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Prepend(key, prefix, expected, now);
}

std::expected<CasToken, StorageError> ShardedStorage::CompareAndSwap(std::string_view key,
                                                                     CasToken expected,
                                                                     std::vector<std::byte> value,
                                                                     std::uint32_t flags,
                                                                     TimePoint expiry,
                                                                     TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
}

std::expected<IStorage::IncrResult, StorageError> ShardedStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::uint64_t magnitude,
                                                                                        bool decrement,
                                                                                        TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->IncrementOrInitialize(key, magnitude, decrement, now);
}

std::expected<void, StorageError> ShardedStorage::Delete(std::string_view key, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Delete(key, now);
}

std::expected<CasToken, StorageError> ShardedStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Touch(key, newExpiry, now);
}

std::expected<GetResult, StorageError> ShardedStorage::Peek(std::string_view key, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Peek(key, now);
}

std::expected<CasToken, StorageError> ShardedStorage::MarkStale(std::string_view key,
                                                                std::optional<TimePoint> newExpiry,
                                                                TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->MarkStale(key, newExpiry, now);
}

std::expected<GetResult, StorageError> ShardedStorage::GetAndTouch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    // Hold the shard lock across both inner calls so the touch and the
    // read form a single atomic critical section — no concurrent writer
    // can mutate or delete the key between them.
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    auto const touched = shard.storage->Touch(key, newExpiry, now);
    if (!touched.has_value())
        return std::unexpected(touched.error());
    return shard.storage->Get(key, now);
}

std::expected<void, StorageError> ShardedStorage::CompareAndDelete(std::string_view key, CasToken expected, TimePoint now)
{
    // Compare and delete under one shard-lock acquisition so a concurrent
    // writer cannot replace the value between the CAS check and the erase.
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    auto const got = shard.storage->Peek(key, now);
    if (!got.has_value())
        return std::unexpected(got.error());
    if (!got->found)
        return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
    if (got->entry.cas != expected)
        return std::unexpected(MakeStorageError(StorageErrorCode::CasMismatch));
    return shard.storage->Delete(key, now);
}

void ShardedStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    // Hold every shard's exclusive lock so the generation bump is
    // atomic from any observer's perspective.
    for (auto& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        shard->storage->FlushWithGeneration(effectiveAt);
    }
}

std::size_t ShardedStorage::PurgeExpired(TimePoint now)
{
    std::size_t total = 0;
    for (auto& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        total += shard->storage->PurgeExpired(now);
    }
    return total;
}

StorageStats ShardedStorage::Snapshot() const noexcept
{
    // CowTreeStorage::Snapshot writes its `mutable _stats` member, so
    // a shared_lock here would race two concurrent Snapshot calls and
    // race a concurrent Get's bump of cmdGet/getHits/getMisses on the
    // same shard. unique_lock serialises Snapshot per shard; cross-
    // shard parallelism still holds.
    StorageStats aggregate;
    for (auto const& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        auto const s = shard->storage->Snapshot();
        aggregate.itemCount += s.itemCount;
        aggregate.bytesUsed += s.bytesUsed;
        aggregate.bytesLimit += s.bytesLimit;
        aggregate.evictions += s.evictions;
        aggregate.cmdGet += s.cmdGet;
        aggregate.cmdSet += s.cmdSet;
        aggregate.cmdTouch += s.cmdTouch;
        aggregate.cmdFlush += s.cmdFlush;
        aggregate.getHits += s.getHits;
        aggregate.getMisses += s.getMisses;
        aggregate.deleteHits += s.deleteHits;
        aggregate.deleteMisses += s.deleteMisses;
        aggregate.incrHits += s.incrHits;
        aggregate.incrMisses += s.incrMisses;
        aggregate.decrHits += s.decrHits;
        aggregate.decrMisses += s.decrMisses;
        aggregate.touchHits += s.touchHits;
        aggregate.touchMisses += s.touchMisses;
        aggregate.casHits += s.casHits;
        aggregate.casMisses += s.casMisses;
        aggregate.casBadval += s.casBadval;
        aggregate.evictedUnfetched += s.evictedUnfetched;
        aggregate.expiredUnfetched += s.expiredUnfetched;
    }
    return aggregate;
}

void ShardedStorage::Resize(std::size_t newTotalBytes)
{
    auto const perShard = newTotalBytes / _shards.size();
    for (auto& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        shard->storage->Resize(perShard);
    }
}

} // namespace FastCache
