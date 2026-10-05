// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/SharedCacheTier.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Testing
{

/// @file SharedTierFakes.hpp
/// What every case about the fleet's shared tier builds it from: a tier over memory, the state
/// that names a machine, and a value the tier accepts. Each was once copied into three test files;
/// a copy that drifts makes its cases pass over a tier no other case builds.

/// A tier's store that runs a case's hook INSIDE the next read or write that reaches it, then forwards
/// every call to the store it owns.
///
/// Inside is the point: a call reaches the store only after its caller took the tier, so whatever the
/// hook does -- move the setting, reconcile the host -- happens to a tier some call is still using.
/// One-shot, so a hook that calls back into the tier does not run again.
class HookedStorage final: public IStorage
{
  public:
    /// @param inner The store; owned.
    /// @param hook Run once, at the next `Get` or `Set`, and then cleared; must outlive this.
    HookedStorage(std::unique_ptr<IStorage> inner, std::function<void()>& hook) noexcept:
        _inner { std::move(inner) },
        _hook { hook }
    {
    }

    [[nodiscard]] std::expected<GetResult, StorageError> Get(std::string_view key,
                                                             core::platform::SteadyTimePoint now) override
    {
        RunHook();
        return _inner->Get(key, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            core::platform::SteadyTimePoint expiry) override
    {
        RunHook();
        return _inner->Set(key, std::move(value), flags, expiry);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Add(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            core::platform::SteadyTimePoint expiry,
                                                            core::platform::SteadyTimePoint now) override
    {
        return _inner->Add(key, std::move(value), flags, expiry, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Replace(std::string_view key,
                                                                std::vector<std::byte> value,
                                                                std::uint32_t flags,
                                                                core::platform::SteadyTimePoint expiry,
                                                                core::platform::SteadyTimePoint now) override
    {
        return _inner->Replace(key, std::move(value), flags, expiry, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Append(std::string_view key,
                                                               std::span<std::byte const> suffix,
                                                               CasToken expected,
                                                               core::platform::SteadyTimePoint now) override
    {
        return _inner->Append(key, suffix, expected, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                CasToken expected,
                                                                core::platform::SteadyTimePoint now) override
    {
        return _inner->Prepend(key, prefix, expected, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                       CasToken expected,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       core::platform::SteadyTimePoint expiry,
                                                                       core::platform::SteadyTimePoint now) override
    {
        return _inner->CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
    }

    [[nodiscard]] std::expected<IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                std::uint64_t magnitude,
                                                                                bool decrement,
                                                                                core::platform::SteadyTimePoint now) override
    {
        return _inner->IncrementOrInitialize(key, magnitude, decrement, now);
    }

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key,
                                                           core::platform::SteadyTimePoint now) override
    {
        return _inner->Delete(key, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Touch(std::string_view key,
                                                              core::platform::SteadyTimePoint newExpiry,
                                                              core::platform::SteadyTimePoint now) override
    {
        return _inner->Touch(key, newExpiry, now);
    }

    [[nodiscard]] std::expected<GetResult, StorageError> Peek(std::string_view key,
                                                              core::platform::SteadyTimePoint now) override
    {
        return _inner->Peek(key, now);
    }

    [[nodiscard]] std::expected<std::optional<core::platform::SteadyTimePoint>, StorageError> PeekExpiry(
        std::string_view key, core::platform::SteadyTimePoint now) override
    {
        return _inner->PeekExpiry(key, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<core::platform::SteadyTimePoint> newExpiry,
                                                                  core::platform::SteadyTimePoint now) override
    {
        return _inner->MarkStale(key, newExpiry, now);
    }

    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        core::platform::SteadyTimePoint now) override
    {
        return _inner->Update(key, fn, now);
    }

    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     core::platform::SteadyTimePoint newExpiry,
                                                                     core::platform::SteadyTimePoint now) override
    {
        return _inner->GetAndTouch(key, newExpiry, now);
    }

    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key,
                                                                core::platform::SteadyTimePoint now) override
    {
        return _inner->ClearExpiry(key, now);
    }

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     core::platform::SteadyTimePoint now) override
    {
        return _inner->CompareAndDelete(key, expected, now);
    }

    void FlushWithGeneration(core::platform::SteadyTimePoint effectiveAt) override
    {
        _inner->FlushWithGeneration(effectiveAt);
    }

    PurgeOutcome PurgeExpired(core::platform::SteadyTimePoint now, PurgeBudget budget) override
    {
        return _inner->PurgeExpired(now, budget);
    }

    void Resize(std::size_t newMaxBytes) override
    {
        _inner->Resize(newMaxBytes);
    }

    [[nodiscard]] StorageStats Snapshot() const noexcept override
    {
        return _inner->Snapshot();
    }

    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override
    {
        return _inner->SnapshotTiers();
    }

    [[nodiscard]] bool SupportsSharedRead() const noexcept override
    {
        return _inner->SupportsSharedRead();
    }

    [[nodiscard]] std::expected<bool, StorageError> Prefetch(std::string_view key,
                                                             core::platform::SteadyTimePoint now) override
    {
        return _inner->Prefetch(key, now);
    }

    void PromoteOnRead(std::string_view key, core::platform::SteadyTimePoint now) override
    {
        _inner->PromoteOnRead(key, now);
    }

    void SetReclaimLog(IReclaimLog* log) override
    {
        _inner->SetReclaimLog(log);
    }

  private:
    /// Take the hook out of its slot before running it, so it runs once however it re-enters. An empty
    /// slot is left untouched: the default path writes nothing.
    void RunHook()
    {
        if (!_hook)
            return;
        auto hook = std::exchange(_hook, {});
        hook();
    }

    std::unique_ptr<IStorage> _inner;
    std::function<void()>& _hook;
};

/// Deletes a tier and records which thread did it, so a case can assert WHERE a store closes.
struct RecordClosingThread
{
    std::thread::id* closedOn;

    /// @param tier The tier to delete.
    void operator()(Node::SharedCacheTier* tier) const
    {
        *closedOn = std::this_thread::get_id();
        std::default_delete<Node::SharedCacheTier> {}(tier);
    }
};

/// Opens the shared tier over memory, counting its opens and on which thread; `fail` scripts a
/// store that will not open. The tier counts on `metrics`, its own: what a case reads there is the
/// fleet's series, never the reading node's. Its store runs `storageHook` inside the next read or
/// write that reaches it (`HookedStorage`); left empty, it forwards and nothing more.
class MemoryOpener final: public Node::ISharedTierOpener
{
  public:
    [[nodiscard]] std::expected<std::shared_ptr<Node::SharedCacheTier>, std::string> Open() override
    {
        ++opens;
        openedOn = std::this_thread::get_id();
        if (fail)
            return std::unexpected { std::string {
                "cannot open D:/cd/shared-cache/objects.cow: another process already has it open" } };
        auto owned = std::make_unique<Node::SharedCacheTier>(
            std::make_unique<HookedStorage>(std::make_unique<InMemoryLruStorage>(64 * 1024 * 1024), storageHook),
            clock,
            metrics);
        return std::shared_ptr<Node::SharedCacheTier> { owned.release(), RecordClosingThread { &closedOn } };
    }

    int opens { 0 };                   ///< How many times `Open` was asked.
    bool fail { false };               ///< Whether `Open` refuses.
    std::thread::id closedOn {};       ///< The thread the last tier was freed on.
    std::thread::id openedOn {};       ///< The thread the last `Open` ran on.
    core::platform::ManualClock clock; ///< The tier's clock.
    AtomicMetricsSink metrics;         ///< The tier's own counters.
    std::function<void()> storageHook; ///< Run inside the next storage call of whichever tier makes one.
};

/// A state whose `shared-cache` setting names @p machine, and nothing else.
/// @param machine The id, or empty for none.
/// @return The state.
[[nodiscard]] inline Cluster::ClusterState NamingSharedCache(std::string const& machine)
{
    Cluster::ClusterState state;
    state.settings.push_back(Cluster::Setting { .name = std::string { Cluster::SharedCacheSetting }, .value = machine });
    return state;
}

/// A stored compile result the canonicalization accepts: an object and nothing else.
/// @return The encoded value.
[[nodiscard]] inline std::vector<std::byte> AStoredObject()
{
    CompileValue compiled;
    compiled.objectBlob = std::vector<std::byte> { std::byte { 'O' }, std::byte { 'B' }, std::byte { 'J' } };
    return EncodeCompileValue(compiled);
}

} // namespace FastCache::Testing
