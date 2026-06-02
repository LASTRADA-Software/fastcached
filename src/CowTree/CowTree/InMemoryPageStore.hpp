// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <unordered_set>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/IPageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

namespace CowTree
{

/// Deterministic in-memory `IPageStore` used by tests and by anyone who
/// wants a CowTree without touching the filesystem.
///
/// All operations are synchronous. The store tracks every Read / Write /
/// Allocate / Free / SyncData / WriteMeta call so failure-injection
/// scenarios can target precise offsets in the commit protocol.
///
/// **Failure injection** is the central feature: tests configure
/// `FailNthWrite`, `FailNthSync`, `TornOnNthSync`, or `FailNthWriteMeta`
/// to make the next-but-N call return `CowTreeError::InjectedFault`
/// (or, for "torn" variants, succeed at returning the data but leave the
/// underlying storage half-written before failing). The CowTree library
/// itself never observes the difference between a real OS failure and
/// an injected one, which is what lets the crash-consistency suite work.
class InMemoryPageStore final: public IPageStore
{
  public:
    /// Failure-injection knobs. Each `*NthFail` counter starts at 0
    /// (disabled); set it to N > 0 to make the Nth occurrence of the
    /// corresponding operation fail.
    struct FaultPlan
    {
        /// Make the Nth `Write()` call return InjectedFault (N >= 1).
        std::size_t failNthWrite { 0 };

        /// Make the Nth `SyncData()` call return InjectedFault.
        std::size_t failNthSyncData { 0 };

        /// Make the Nth `WriteMeta()` call return InjectedFault.
        std::size_t failNthWriteMeta { 0 };

        /// For the Nth `WriteMeta()` call, write a corrupted (zeroed)
        /// page first, then return InjectedFault. Simulates a torn meta
        /// write whose data did land on disk before the crash.
        std::size_t tornOnNthWriteMeta { 0 };
    };

    /// Construct with the default page size (`DefaultPageSize`).
    InMemoryPageStore() noexcept;

    /// Construct with an explicit page size.
    /// @param pageSize Bytes per data and meta page; must be in
    ///                 `[MinPageSize, MaxPageSize]`.
    explicit InMemoryPageStore(std::size_t pageSize) noexcept;

    /// Configure failure injection. May be called at any time; the next
    /// matching operation observes the updated plan.
    /// @param plan Fault-injection plan.
    void SetFaultPlan(FaultPlan plan) noexcept;

    /// @return The fault plan in effect (live counters tick down to 0
    ///         once the targeted failure has been triggered).
    [[nodiscard]] FaultPlan GetFaultPlan() const noexcept;

    /// Reset every Write counter so allocation history can be inspected
    /// from a known baseline. Does not clear stored pages.
    void ResetCounters() noexcept;

    /// @return Total number of successful Write() calls since construction.
    [[nodiscard]] std::size_t WriteCount() const noexcept;

    /// @return Total number of SyncData() calls (including failed ones).
    [[nodiscard]] std::size_t SyncDataCount() const noexcept;

    // IPageStore -----------------------------------------------------

    [[nodiscard]] auto Read(PageId id) const -> std::expected<BytesView, CowTreeError> override;

    [[nodiscard]] auto Allocate() -> std::expected<PageId, CowTreeError> override;

    [[nodiscard]] auto Write(PageId id, BytesView data) -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto Free(PageId id) -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto SyncData() -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto ReadMeta(MetaSlot slot) const -> std::expected<Meta, CowTreeError> override;

    [[nodiscard]] auto WriteMeta(MetaSlot slot, Meta const& meta) -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto PageSize() const noexcept -> std::size_t override;

  private:
    std::size_t _pageSize;

    /// Storage for data pages. `_pages[i]` corresponds to PageId(i+1);
    /// freed pages are tracked separately and PageId(0) is reserved as
    /// the None sentinel.
    std::vector<std::vector<std::byte>> _pages;

    /// Free-list of recyclable page indices (0-based into `_pages`).
    std::vector<std::size_t> _freeList;

    /// Set of page indices that are currently allocated (i.e. valid for
    /// Read). Used to reject reads of freed pages.
    std::unordered_set<std::size_t> _live;

    /// The two meta-page raw byte buffers. Each is exactly _pageSize
    /// long; zero-initialised until first write.
    std::array<std::vector<std::byte>, 2> _meta;

    FaultPlan _plan;
    mutable std::size_t _writeCount { 0 };
    mutable std::size_t _syncDataCount { 0 };
    mutable std::size_t _writeMetaCount { 0 };

    /// Translate a PageId into the 0-based index into `_pages`.
    [[nodiscard]] static constexpr std::size_t IndexOf(PageId id) noexcept
    {
        return static_cast<std::size_t>(id.value - 1);
    }

    /// Translate a 0-based index back into a PageId.
    [[nodiscard]] static constexpr PageId PageOf(std::size_t index) noexcept
    {
        return PageId { static_cast<std::uint64_t>(index + 1) };
    }
};

} // namespace CowTree
