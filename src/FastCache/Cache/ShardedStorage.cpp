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
    // Map the hash onto [0, shardCount) with a multiply-shift rather than a
    // modulo. `%` compiles to a hardware divide — ~14 cycles on this class of
    // CPU, which is a measurable slice of a ~20 ns lookup and was being paid on
    // every single storage operation.
    //
    // Taking the top 32 bits of `hash32 * shardCount` is the standard
    // alternative (Lemire): one multiply and one shift, and unlike a mask it
    // needs no power-of-two shard count, so `--storage-shards` keeps accepting
    // any value. The resulting partition differs from the modulo's, which is
    // fine — which shard a key lands on is arbitrary, only its stability within
    // a process matters, and that is preserved.
    auto const hash = static_cast<std::uint64_t>(std::hash<std::string_view> {}(key));
    auto const folded = (hash >> 32) ^ (hash & 0xFFFF'FFFFULL);
    return static_cast<std::size_t>((folded * _shards.size()) >> 32);
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
        constexpr unsigned PromoteEveryNthRead = 16;
        // The sampler is per *thread*, not per shard. It used to be a shared
        // atomic on the shard, which put a locked read-modify-write — and a
        // cache line every reader on that shard wrote to — on the hot read
        // path, to drive nothing but a 1-in-16 sampling decision. Nothing here
        // needs a globally consistent count: the promotion is best-effort by
        // construction, and a per-thread counter still promotes one read in
        // sixteen while spreading promotions more evenly across reactors.
        static thread_local unsigned readSampler = 0;
        if (readSampler++ % PromoteEveryNthRead == 0)
        {
            // Try for the exclusive lock, never wait for it. Promotion is
            // best-effort by contract — the entry may already have been evicted,
            // and the comment on IStorage::PromoteOnRead says a miss is fine —
            // so blocking for it trades throughput for recency bookkeeping that
            // is allowed to be wrong.
            //
            // Blocking here was measurably catastrophic. `std::shared_mutex` is
            // an SRWLOCK on Windows, where a waiting writer blocks *every*
            // subsequent reader on that shard, so one promotion in sixteen
            // reads was enough to convoy all readers behind it: 16-thread read
            // throughput peaked at 4 threads and then fell to half of what a
            // single thread managed. With try_lock the same benchmark scales to
            // 8 threads and holds ~3.3x the 16-thread figure.
            std::unique_lock const lock { shard.mu, std::try_to_lock };
            if (lock.owns_lock())
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

std::expected<bool, StorageError> ShardedStorage::Prefetch(std::string_view key, TimePoint now)
{
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    return shard.storage->Prefetch(key, now);
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

std::expected<bool, StorageError> ShardedStorage::ClearExpiry(std::string_view key, TimePoint now)
{
    // Peek + Touch under one shard-lock acquisition so a concurrent
    // SETEX cannot slip a new TTL in between (the TOCTOU window the
    // previous protocol-layer `Ttl + TouchAt` decomposition left open).
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    auto const peeked = shard.storage->Peek(key, now);
    if (!peeked.has_value())
        return std::unexpected(peeked.error());
    if (!peeked->found)
        return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
    if (peeked->entry.expiry == TimePoint::max())
        return false; // present but had no TTL
    auto const touched = shard.storage->Touch(key, TimePoint::max(), now);
    if (!touched.has_value())
        return std::unexpected(touched.error());
    return true;
}

std::expected<CasToken, StorageError> ShardedStorage::Update(
    std::string_view key,
    std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
    TimePoint now)
{
    // Hold the shard's exclusive lock across the whole read-modify-write so the
    // decode → mutate → re-encode → store sequence is one atomic critical
    // section; without this two concurrent SADDs (or an SADD racing an SREM)
    // would each read the pre-image and the later write would clobber the other.
    auto& shard = *_shards[ShardIndexFor(key)];
    std::unique_lock const lock { shard.mu };
    auto const current = shard.storage->Peek(key, now);
    if (!current.has_value())
        return std::unexpected(current.error());
    auto outcome = fn(*current);
    if (!outcome.has_value())
        return std::unexpected(outcome.error());
    switch (outcome->action)
    {
        case UpdateAction::Store: {
            // Preserve the prior entry's expiry unless the callback
            // explicitly overrides via outcome->newExpiry (redis INCR/
            // SADD semantics: TTL survives the read-modify-write).
            auto const expiry = outcome->newExpiry.value_or(current->found ? current->entry.expiry : TimePoint::max());
            return shard.storage->Set(key, std::move(outcome->value), outcome->flags, expiry);
        }
        case UpdateAction::Delete:
            if (current->found)
                (void) shard.storage->Delete(key, now);
            return CasToken { 0 };
        case UpdateAction::Unchanged:
            return current->found ? current->entry.cas : CasToken { 0 };
    }
    return CasToken { 0 };
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
