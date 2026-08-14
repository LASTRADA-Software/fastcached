// SPDX-License-Identifier: Apache-2.0
/// In-process micro-benchmarks for the storage stack.
///
/// The workload is a deliberate replica of the BenchmarkDotNet suite in
/// jitbit/FastCache (`FastCache.Benchmarks/Program.cs`): 1000 entries keyed
/// `test0`..`test999`, a body that looks up `test123`/`test234`/`test673`/
/// `test987`, and a 10-minute TTL. Matching it exactly is the point — it is the
/// only layer at which fastcached and an in-process .NET cache library can be
/// compared operation-for-operation, so the shapes must not drift.
///
/// Both sides report a four-lookup body; `bench/inproc_bench.py` divides by
/// `LookupsPerIteration` on both to reach ns-per-operation.
///
/// The layer table below decomposes our cost: each row adds one production
/// concern (byte budget, sharding + locking, the CacheEngine facade) so the
/// report can attribute nanoseconds to features rather than guessing.

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/StringHash.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace FastCache;

namespace
{

/// Number of entries pre-loaded into every fixture. Matches the jitbit setup
/// loop, so both sides probe a map of the same population.
constexpr std::size_t EntryCount = 1000;

/// Lookups performed per benchmark iteration. jitbit's benchmark bodies issue
/// four `TryGet` calls, so its published means are per four operations; ours
/// does the same and the runner divides both by this constant.
constexpr std::size_t LookupsPerIteration = 4;

/// The four keys jitbit probes, verbatim. All are present, so every lookup is a
/// hit — the case a cache is optimised for and the one their numbers describe.
constexpr std::array<std::string_view, LookupsPerIteration> ProbeKeys { "test123", "test234", "test673", "test987" };

/// Entry lifetime used at population time. Long enough that nothing expires
/// mid-run, matching jitbit's `TimeSpan.FromMinutes(10)`.
constexpr auto EntryTtl = std::chrono::minutes { 10 };

/// Payload stored under each key: four bytes, the width of the `int` jitbit
/// caches. A cache that serves bytes over a wire cannot store an unboxed `int`,
/// so four bytes is the closest honest equivalent.
constexpr std::size_t ValueBytes = 4;

/// Byte budget for the rows that exercise eviction accounting. Far above what
/// 1000 four-byte values need, so the benchmark measures the bookkeeping rather
/// than eviction churn.
constexpr std::size_t BudgetBytes = std::size_t { 64 } * 1024 * 1024;

/// Compiler-only barrier that stops a lookup being hoisted out of the
/// benchmark's iteration loop.
///
/// The probe keys and the populated map are both loop-invariant, so without
/// this the optimizer is free to compute the result once and reuse it — which
/// it demonstrably did, reporting a bare `unordered_map` probe at ~1 ns. A
/// signal fence orders memory operations for the compiler without emitting a
/// single instruction, so it costs nothing at run time and needs no inline asm
/// or per-compiler intrinsic.
inline void DenyHoisting() noexcept
{
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/// Which clock a row's `CacheEngine` reads. Only meaningful when the row routes
/// through the engine, since that is the only layer that reads a clock at all.
enum class EngineClock : std::uint8_t
{
    Steady, ///< `SteadyClock` — a QueryPerformanceCounter per operation.
    Cached, ///< `CachedClock` — a value the reactor refreshes once per iteration.
};

/// One benchmarkable storage configuration.
///
/// The rows differ only by which production concerns are switched on, so a
/// single builder interprets the table rather than each layer having its own
/// hand-written construction block.
struct LayerSpec
{
    std::string_view name;   ///< Row label carried into the report.
    std::string_view note;   ///< What this row adds over the one above it.
    std::size_t shards;      ///< Shard count; 0 means no `ShardedStorage` wrapper.
    std::size_t maxBytes;    ///< Byte budget; 0 disables eviction (jitbit-matched).
    LruMode lruMode;         ///< Recency policy of the inner LRU storage.
    bool throughEngine;      ///< Route lookups through the `CacheEngine` facade.
    EngineClock engineClock; ///< Clock the engine reads, when it is in the stack.
};

/// The layer table. Read top to bottom, each row costs one more production
/// feature than the last; the first row is the configuration whose semantics
/// match jitbit's (unbounded, TTL-only, non-mutating reads) and is the headline.
constexpr std::array Layers {
    LayerSpec { .name = "lru-unbounded",
                .note = "InMemoryLruStorage, eviction disabled - jitbit-matched semantics",
                .shards = 0,
                .maxBytes = 0,
                .lruMode = LruMode::Approximate,
                .throughEngine = false,
                .engineClock = EngineClock::Steady },
    LayerSpec { .name = "lru-bounded",
                .note = "adds the byte budget and eviction bookkeeping",
                .shards = 0,
                .maxBytes = BudgetBytes,
                .lruMode = LruMode::Approximate,
                .throughEngine = false,
                .engineClock = EngineClock::Steady },
    LayerSpec { .name = "lru-strict",
                .note = "adds exact LRU: every read promotes and so must mutate",
                .shards = 0,
                .maxBytes = BudgetBytes,
                .lruMode = LruMode::Strict,
                .throughEngine = false,
                .engineClock = EngineClock::Steady },
    LayerSpec { .name = "sharded",
                .note = "adds the shard index and the per-shard shared_mutex",
                .shards = 16,
                .maxBytes = BudgetBytes,
                .lruMode = LruMode::Approximate,
                .throughEngine = false,
                .engineClock = EngineClock::Steady },
    LayerSpec { .name = "engine-steadyclock",
                .note = "adds the CacheEngine facade, reading the OS clock per op",
                .shards = 16,
                .maxBytes = BudgetBytes,
                .lruMode = LruMode::Approximate,
                .throughEngine = true,
                .engineClock = EngineClock::Steady },
    LayerSpec { .name = "engine-cachedclock",
                .note = "the same, with the reactor-refreshed CachedClock (shipped)",
                .shards = 16,
                .maxBytes = BudgetBytes,
                .lruMode = LruMode::Approximate,
                .throughEngine = true,
                .engineClock = EngineClock::Cached },
};

/// Owns every object one layer needs, in an order that keeps the references
/// `CacheEngine` holds valid for the fixture's lifetime.
///
/// Non-copyable and non-movable on purpose: `CacheEngine` stores references to
/// the `clock` and `storage` members, so relocating the fixture would dangle
/// them.
class Fixture
{
  public:
    /// Build the stack described by `spec` and pre-load it with `EntryCount`
    /// entries.
    /// @param spec Layer configuration to materialise.
    explicit Fixture(LayerSpec const& spec)
    {
        _storage = MakeStorage(spec);
        if (spec.throughEngine)
        {
            // Nothing refreshes `_cachedClock` during the measured loop, which
            // is exactly its production steady state: the reactor refreshes it
            // once per iteration and every command in between reads the stored
            // value. Entries are populated with a 10-minute TTL, so a frozen
            // clock cannot expire them mid-run either way.
            IClock& engineClock =
                spec.engineClock == EngineClock::Cached ? static_cast<IClock&>(_cachedClock) : static_cast<IClock&>(_clock);
            _engine = std::make_unique<CacheEngine>(*_storage, engineClock);
        }
        Populate();
    }

    Fixture(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;

    /// Look up one key the way the configured layer would serve it.
    ///
    /// The raw-storage rows take `now` as a parameter, so the caller hoists the
    /// clock read out of the measured region and the number describes the map
    /// alone. The engine row deliberately does not: reading the clock is part of
    /// what `CacheEngine::Get` does per call, and on Windows that is a
    /// `QueryPerformanceCounter`, which is exactly the kind of cost this
    /// decomposition exists to expose.
    /// @param key Key to look up.
    /// @param now Current clock value, for the layers that take one.
    /// @return True on a hit.
    [[nodiscard]] bool Lookup(std::string_view key, TimePoint now) const
    {
        auto const result = _engine ? _engine->Get(key) : _storage->Get(key, now);
        return result.has_value() && result->found;
    }

  private:
    /// Construct the storage described by `spec`, wrapping it in a
    /// `ShardedStorage` when the row asks for more than zero shards.
    /// @param spec Layer configuration.
    /// @return The composed storage.
    [[nodiscard]] static std::unique_ptr<IStorage> MakeStorage(LayerSpec const& spec)
    {
        auto makeInner = [&](std::size_t budget) {
            return std::make_unique<InMemoryLruStorage>(budget, 0, spec.lruMode);
        };

        if (spec.shards == 0)
            return makeInner(spec.maxBytes);

        // The daemon splits one budget across its shards (see main.cpp), so the
        // sharded rows must divide rather than multiply the memory ceiling —
        // otherwise they would evict at a different point than production does.
        std::vector<std::unique_ptr<IStorage>> shards;
        shards.reserve(spec.shards);
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, spec.shards))
            shards.push_back(makeInner(spec.maxBytes / spec.shards));
        return std::make_unique<ShardedStorage>(std::move(shards));
    }

    /// Fill the storage with `EntryCount` entries keyed `test0`..`testN-1`.
    void Populate()
    {
        auto const expiry = _clock.Now() + EntryTtl;
        for (auto const index: std::views::iota(std::size_t { 0 }, EntryCount))
        {
            std::vector<std::byte> value(ValueBytes, std::byte { 0 });
            auto const stored = _storage->Set(std::format("test{}", index), std::move(value), 0, expiry);
            if (!stored.has_value())
                throw std::runtime_error { "benchmark fixture failed to populate" };
        }
    }

    SteadyClock _clock {};
    CachedClock _cachedClock { _clock }; // declared after _clock: it holds a reference to it
    std::unique_ptr<IStorage> _storage {};
    std::unique_ptr<CacheEngine> _engine {};
};

} // namespace

TEST_CASE("storage lookup, jitbit-equivalent workload", "[bench][lookup]")
{
    // The control: what a bare std::unordered_map costs on this machine with
    // this key set. Every layer above it is measured against this floor, so a
    // reader can tell our overhead from the container's.
    {
        // Heterogeneous lookup, exactly as InMemoryLruStorage's index declares
        // it — without it the control would build a temporary std::string per
        // probe and understate the floor it is supposed to establish.
        std::unordered_map<std::string, std::uint32_t, TransparentStringHash, std::equal_to<>> control;
        for (auto const index: std::views::iota(std::size_t { 0 }, EntryCount))
            control.emplace(std::format("test{}", index), static_cast<std::uint32_t>(index));

        BENCHMARK("control-unordered-map")
        {
            std::uint32_t found = 0;
            for (auto const key: ProbeKeys)
            {
                found += static_cast<std::uint32_t>(control.find(key) != control.end());
                DenyHoisting();
            }
            return found;
        };
    }

    // Attribution aid, not a cache measurement: what one injected clock read
    // costs. `CacheEngine::Get` performs exactly one per operation, so this is
    // a floor on the engine row's overhead over the sharded row — and on
    // Windows `steady_clock::now()` is a QueryPerformanceCounter, which is far
    // from free. Without this row the engine row's cost would be guesswork.
    {
        SteadyClock clock;
        IClock const& injected = clock; // reached through the seam, as production does

        BENCHMARK("clock-now")
        {
            TimePoint latest {};
            for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, LookupsPerIteration))
            {
                latest = injected.Now();
                DenyHoisting();
            }
            return latest;
        };
    }

    for (auto const& layer: Layers)
    {
        Fixture fixture { layer };
        auto const now = std::chrono::steady_clock::now();

        BENCHMARK(std::string { layer.name })
        {
            std::uint32_t hits = 0;
            for (auto const key: ProbeKeys)
            {
                hits += static_cast<std::uint32_t>(fixture.Lookup(key, now));
                DenyHoisting();
            }
            return hits;
        };
    }
}

TEST_CASE("storage lookup scaling across threads", "[bench][scaling]")
{
    // Tier 3: what a sharded cache does with more cores. A single-threaded
    // ns/op figure says nothing about this, and it is the axis on which a
    // daemon's storage layer is actually designed to win.
    constexpr std::array<std::size_t, 5> ThreadCounts { 1, 2, 4, 8, 16 };
    constexpr auto MeasureFor = std::chrono::milliseconds { 300 };

    LayerSpec const spec { .name = "sharded",
                           .note = "read scaling",
                           .shards = 16,
                           .maxBytes = BudgetBytes,
                           .lruMode = LruMode::Approximate,
                           .throughEngine = false,
                           .engineClock = EngineClock::Steady };

    // Spread reads over the whole key set rather than the four hot keys. Four
    // keys reach at most four of the sixteen shards, which would measure lock
    // contention on a pathological access pattern instead of read scaling.
    std::vector<std::string> keys;
    keys.reserve(EntryCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, EntryCount))
        keys.push_back(std::format("test{}", index));

    for (auto const threadCount: ThreadCounts)
    {
        Fixture fixture { spec };
        auto const now = std::chrono::steady_clock::now();

        std::atomic<bool> go { false };
        std::atomic<bool> stop { false };
        std::atomic<std::uint64_t> total { 0 };

        std::vector<std::jthread> workers;
        workers.reserve(threadCount);
        for (auto const worker: std::views::iota(std::size_t { 0 }, threadCount))
        {
            workers.emplace_back([&, worker] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();

                // Each thread starts at a different offset so they do not march
                // through the shards in lockstep.
                auto cursor = worker * (EntryCount / threadCount);
                std::uint64_t local = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    for ([[maybe_unused]] auto const step: std::views::iota(0, 64))
                    {
                        cursor = (cursor + 1) % keys.size();
                        local += static_cast<std::uint64_t>(fixture.Lookup(keys[cursor], now));
                    }
                }
                total.fetch_add(local, std::memory_order_relaxed);
            });
        }

        go.store(true, std::memory_order_release);
        auto const started = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(MeasureFor);
        stop.store(true, std::memory_order_relaxed);
        // Stop the clock before joining: the join itself is not measured work,
        // and the threads only overrun by at most one 64-lookup batch.
        auto const elapsed = std::chrono::steady_clock::now() - started;
        workers.clear(); // jthread joins on destruction

        auto const seconds = std::chrono::duration<double>(elapsed).count();
        auto const opsPerSecond = static_cast<double>(total.load()) / seconds;

        // Printed, not asserted: this is a measurement, not a contract.
        // bench/inproc_bench.py scrapes these lines from stdout.
        std::cout << std::format("SCALING threads={} ops_per_sec={:.0f}\n", threadCount, opsPerSecond);
    }
}
