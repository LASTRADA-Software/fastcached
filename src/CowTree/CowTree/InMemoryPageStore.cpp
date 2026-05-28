// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <expected>
#include <ranges>
#include <utility>

#include <CowTree/InMemoryPageStore.hpp>
#include <CowTree/Meta.hpp>

namespace CowTree
{

InMemoryPageStore::InMemoryPageStore() noexcept:
    InMemoryPageStore { DefaultPageSize }
{
}

InMemoryPageStore::InMemoryPageStore(std::size_t pageSize) noexcept:
    _pageSize { pageSize }
{
    // Bootstrap each meta slot with an encoded blank meta so a freshly
    // constructed store behaves like a fresh FilePageStore on disk: both
    // slots decode to a valid txnId 0 / empty-root meta.
    Meta blank;
    blank.pageSize = static_cast<std::uint32_t>(_pageSize);
    blank.txnId = 0;
    for (auto& m: _meta)
    {
        m.assign(_pageSize, std::byte { 0 });
        (void) EncodeMeta(BytesSpan { m.data(), m.size() }, blank);
    }
}

void InMemoryPageStore::SetFaultPlan(FaultPlan plan) noexcept
{
    _plan = plan;
}

InMemoryPageStore::FaultPlan InMemoryPageStore::GetFaultPlan() const noexcept
{
    return _plan;
}

void InMemoryPageStore::ResetCounters() noexcept
{
    _writeCount = 0;
    _syncDataCount = 0;
    _writeMetaCount = 0;
}

std::size_t InMemoryPageStore::WriteCount() const noexcept
{
    return _writeCount;
}

std::size_t InMemoryPageStore::SyncDataCount() const noexcept
{
    return _syncDataCount;
}

auto InMemoryPageStore::Read(PageId id) const -> std::expected<BytesView, CowTreeError>
{
    if (!id)
        return std::unexpected(CowTreeError::OutOfRange);
    auto const idx = IndexOf(id);
    if (idx >= _pages.size())
        return std::unexpected(CowTreeError::OutOfRange);
    if (!_live.contains(idx))
        return std::unexpected(CowTreeError::OutOfRange);
    auto const& page = _pages[idx];
    return BytesView { page.data(), page.size() };
}

auto InMemoryPageStore::Allocate() -> std::expected<PageId, CowTreeError>
{
    std::size_t idx = 0;
    if (!_freeList.empty())
    {
        idx = _freeList.back();
        _freeList.pop_back();
    }
    else
    {
        idx = _pages.size();
        _pages.emplace_back(_pageSize, std::byte { 0 });
    }
    _live.insert(idx);
    return PageOf(idx);
}

auto InMemoryPageStore::Write(PageId id, BytesView data) -> std::expected<void, CowTreeError>
{
    ++_writeCount;
    if (_plan.failNthWrite != 0 && _writeCount == _plan.failNthWrite)
    {
        _plan.failNthWrite = 0;
        return std::unexpected(CowTreeError::InjectedFault);
    }
    if (!id)
        return std::unexpected(CowTreeError::OutOfRange);
    auto const idx = IndexOf(id);
    if (idx >= _pages.size())
        return std::unexpected(CowTreeError::OutOfRange);
    if (data.size() != _pageSize)
        return std::unexpected(CowTreeError::InvalidArg);
    auto& page = _pages[idx];
    page.assign(data.begin(), data.end());
    _live.insert(idx);
    return {};
}

auto InMemoryPageStore::Free(PageId id) -> std::expected<void, CowTreeError>
{
    if (!id)
        return std::unexpected(CowTreeError::OutOfRange);
    auto const idx = IndexOf(id);
    if (idx >= _pages.size())
        return std::unexpected(CowTreeError::OutOfRange);
    if (!_live.contains(idx))
        return std::unexpected(CowTreeError::OutOfRange);
    _live.erase(idx);
    _freeList.push_back(idx);
    // Zero the freed page so a stale Read on a bug surfaces obviously.
    std::ranges::fill(_pages[idx], std::byte { 0 });
    return {};
}

auto InMemoryPageStore::SyncData() -> std::expected<void, CowTreeError>
{
    ++_syncDataCount;
    if (_plan.failNthSyncData != 0 && _syncDataCount == _plan.failNthSyncData)
    {
        _plan.failNthSyncData = 0;
        return std::unexpected(CowTreeError::InjectedFault);
    }
    return {};
}

auto InMemoryPageStore::ReadMeta(MetaSlot slot) const -> std::expected<Meta, CowTreeError>
{
    auto const& raw = _meta[static_cast<std::size_t>(slot)];
    return DecodeMeta(BytesView { raw.data(), raw.size() });
}

auto InMemoryPageStore::WriteMeta(MetaSlot slot, Meta const& meta) -> std::expected<void, CowTreeError>
{
    ++_writeMetaCount;
    auto& raw = _meta[static_cast<std::size_t>(slot)];

    auto effective = meta;
    effective.pageSize = static_cast<std::uint32_t>(_pageSize);

    if (_plan.tornOnNthWriteMeta != 0 && _writeMetaCount == _plan.tornOnNthWriteMeta)
    {
        _plan.tornOnNthWriteMeta = 0;
        // Zero the destination slot to simulate the "wrote a corrupted
        // page then crashed" scenario. The other slot remains intact so
        // recovery can fall back to it.
        std::ranges::fill(raw, std::byte { 0 });
        return std::unexpected(CowTreeError::InjectedFault);
    }
    if (_plan.failNthWriteMeta != 0 && _writeMetaCount == _plan.failNthWriteMeta)
    {
        _plan.failNthWriteMeta = 0;
        return std::unexpected(CowTreeError::InjectedFault);
    }

    auto const encoded = EncodeMeta(BytesSpan { raw.data(), raw.size() }, effective);
    if (!encoded.has_value())
        return std::unexpected(encoded.error());
    return {};
}

std::size_t InMemoryPageStore::PageSize() const noexcept
{
    return _pageSize;
}

} // namespace CowTree
