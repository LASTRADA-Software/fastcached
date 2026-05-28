// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
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
    auto& shard = *_shards[ShardIndexFor(key)];
    std::shared_lock const lock { shard.mu };
    return shard.storage->Get(key, now);
}

std::expected<CasToken, StorageError> ShardedStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
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
                                                              TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Append(key, suffix, now);
}

std::expected<CasToken, StorageError> ShardedStorage::Prepend(std::string_view key,
                                                               std::span<std::byte const> prefix,
                                                               TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Prepend(key, prefix, now);
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
                                                                                         std::int64_t delta,
                                                                                         TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->IncrementOrInitialize(key, delta, now);
}

std::expected<void, StorageError> ShardedStorage::Delete(std::string_view key, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
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
    StorageStats aggregate;
    for (auto const& shard: _shards)
    {
        std::shared_lock const lock { shard->mu };
        auto const s = shard->storage->Snapshot();
        aggregate.itemCount += s.itemCount;
        aggregate.bytesUsed += s.bytesUsed;
        aggregate.bytesLimit += s.bytesLimit;
        aggregate.evictions += s.evictions;
        aggregate.getHits += s.getHits;
        aggregate.getMisses += s.getMisses;
        aggregate.cmdGet += s.cmdGet;
        aggregate.cmdSet += s.cmdSet;
    }
    return aggregate;
}

void ShardedStorage::ResizeTotal(std::size_t newTotalBytes)
{
    auto const perShard = newTotalBytes / _shards.size();
    for (auto& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        if (auto* mem = dynamic_cast<InMemoryLruStorage*>(shard->storage.get()); mem != nullptr)
            mem->Resize(perShard);
        else if (auto* disk = dynamic_cast<CowTreeStorage*>(shard->storage.get()); disk != nullptr)
            disk->Resize(perShard);
    }
}

} // namespace FastCache
