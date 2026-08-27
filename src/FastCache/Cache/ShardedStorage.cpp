// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
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
    // any value.
    //
    // What it *does* require is that the top bits of its input be well mixed,
    // because those are the only bits that survive the shift — and `std::hash`
    // makes no such promise. libstdc++ and libc++ hash strings with a murmur
    // variant whose whole word is mixed; MSVC uses FNV-1a, which avalanches into
    // the low bits and leaves the high ones correlated. Xor-folding the halves
    // together was not enough to repair that: over 10k keys across 16 shards it
    // put 1300 keys on the busiest shard against a mean of 625 — a 2.08x
    // imbalance on Windows only, which is lost capacity and concentrated lock
    // contention with no symptom that names itself. So mix first. Multiplying by
    // an odd constant is a bijection whose high half depends on every input bit
    // (Fibonacci hashing, the constant being 2^64 / phi), which brings MSVC to
    // 1.03x and leaves libstdc++ unchanged at 1.05x. It costs one more multiply
    // — still nothing beside the divide this replaced — and it buys independence
    // from a hash quality the standard does not specify.
    //
    // The resulting partition differs from the modulo's, and for the in-memory
    // backend that costs nothing — which shard a key lands on is arbitrary and
    // only its stability within a process matters. The *persistent* backend is
    // where this has a price, and it is worth stating rather than discovering:
    // with `--storage` and more than one shard, each shard is its own file
    // (`shard-NN.cow`, see main.cpp), so this function decides which file a key
    // lives in. Repartitioning therefore looks to a restarted daemon exactly
    // like changing `--storage-shards` does: keys written by the old mapping are
    // looked up in a different file and miss, and their bytes stay on disk,
    // unreachable by Get and Delete alike, until the CoW tree evicts them — which
    // it only does when `storage_max_disk_bytes` caps the tier at all. That is a
    // one-time upgrade cost for a cache, not data loss, but it is a cost, so the
    // reduction is a stable part of the on-disk contract and must not be changed
    // casually a second time.
    constexpr std::uint64_t GoldenRatio64 = 0x9E37'79B9'7F4A'7C15ULL;
    auto const hash = static_cast<std::uint64_t>(std::hash<std::string_view> {}(key));
    auto const mixed = (hash * GoldenRatio64) >> 32;
    return static_cast<std::size_t>((mixed * _shards.size()) >> 32);
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
            //
            // What a skipped promotion actually costs is wider than LRU order,
            // because PromoteOnRead is also the only writer of `lastAccess` and
            // `fetched` on this path (InMemoryLruStorage::Get's Approximate
            // branch deliberately writes no node). So a skip also leaves the
            // meta `l`/`h` flags and the `evicted_unfetched` / `expired_unfetched`
            // counters reporting a read that happened as one that did not.
            std::unique_lock const lock { shard.mu, std::try_to_lock };
            if (lock.owns_lock())
                shard.storage->PromoteOnRead(key, now);
            else
            {
                // Contention is exactly when skipping hurts most: the LRU would
                // drift toward insertion order, and those counters would go
                // wrong, in the regime where both matter. Un-consume the sample
                // so the *next* read retries rather than the one sixteen reads
                // from now.
                //
                // This cannot spin and cannot convoy. It never blocks — a failed
                // try_lock is a failed CAS, not a wait — and each retry is a
                // whole further lookup away, so a shard under a sustained writer
                // degrades to "promote when you can", not to a busy loop. The
                // worst case is bounded by who reaches this branch at all: only
                // `InMemoryLruStorage` in Approximate mode reports
                // SupportsSharedRead(), so every exclusive hold competing with
                // the retry is a short in-memory mutation. The persistent
                // LayeredStorage takes the exclusive path above and never gets
                // here, so no fsync can ever be what a retry is failing against.
                //
                // Measured rather than assumed: 9 interleaved reps of the
                // `[scaling]` benchmark (16 shards, 1/2/4/8/16 threads) put the
                // two variants within noise of each other at every thread count
                // — median ratios 0.96-1.04 with overlapping ranges — so the
                // retry buys the accuracy for nothing.
                readSampler = 0;
            }
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

PurgeOutcome ShardedStorage::PurgeExpired(TimePoint now, PurgeBudget budget)
{
    PurgeOutcome total {};
    auto const shardCount = _shards.size();
    auto const start = _sweepShard.load(std::memory_order_relaxed);

    // The budget is spent across shards in order rather than divided among
    // them: dividing would floor to zero the moment the budget is smaller than
    // the shard count, and a sweep that examines nothing at all is worse than
    // one that examines a few shards properly and rotates.
    std::size_t visited = 0;
    for (auto const step: std::views::iota(std::size_t { 0 }, shardCount))
    {
        if (budget.ScanExhausted(total.scanned) || budget.PurgeExhausted(total.purged))
        {
            // Out of budget with shards still unvisited. Their entries have not
            // been examined, so this pass did not cover the keyspace.
            total.completedPass = false;
            break;
        }

        auto& shard = _shards[(start + step) % shardCount];
        std::unique_lock const lock { shard->mu };
        auto const outcome = shard->storage->PurgeExpired(now, budget.Remaining(total.scanned, total.purged));
        total.scanned += outcome.scanned;
        total.purged += outcome.purged;
        total.completedPass = total.completedPass && outcome.completedPass;
        ++visited;
    }

    // Advanced past everything visited, so the next sweep begins where this one
    // ran out. A shard left mid-pass keeps its own cursor, so nothing is lost
    // by moving on -- it resumes exactly where it stopped one rotation later.
    if (shardCount != 0)
        _sweepShard.store((start + visited) % shardCount, std::memory_order_relaxed);
    return total;
}

void ShardedStorage::SetReclaimLog(IReclaimLog* log)
{
    for (auto& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        shard->storage->SetReclaimLog(log);
    }
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
        // Summed by walking the field table rather than by twenty-three `+=`
        // lines, one per field. The hand-written list had already drifted --
        // `writeErrors` was added to `StorageStats` and never added here -- and
        // it only failed to matter because the decorator that populates it wraps
        // this class rather than its shards. A sum that is right by where a
        // wrapper happens to sit is one field's worth of luck.
        AddInto(aggregate, shard->storage->Snapshot());
    }
    return aggregate;
}

TieredStorageStats ShardedStorage::SnapshotTiers() const noexcept
{
    TieredStorageStats aggregate {};
    for (auto const& shard: _shards)
    {
        std::unique_lock const lock { shard->mu };
        AddInto(aggregate, shard->storage->SnapshotTiers());
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
