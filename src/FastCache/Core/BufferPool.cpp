// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BufferPool.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace FastCache
{

// Default minimum capacity when caller passes 0. Sized to comfortably hold a
// memcached text line or a Redis RESP small array without an immediate
// reallocation. Not a hard limit — Acquire() honours minCapacity.
constexpr std::size_t kDefaultBufferCapacity = 4096;

// -- PooledBuffer ----------------------------------------------------------

PooledBuffer::PooledBuffer(std::shared_ptr<BufferPool> pool, std::vector<std::byte> storage) noexcept:
    _pool { std::move(pool) },
    _storage { std::move(storage) },
    _size { _storage.size() },
    _hasStorage { true }
{
}

PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept:
    _pool { std::move(other._pool) },
    _storage { std::move(other._storage) },
    _size { other._size },
    _hasStorage { other._hasStorage }
{
    other._size = 0;
    other._hasStorage = false;
}

PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept
{
    if (this != &other)
    {
        Release();
        _pool = std::move(other._pool);
        _storage = std::move(other._storage);
        _size = other._size;
        _hasStorage = other._hasStorage;
        other._size = 0;
        other._hasStorage = false;
    }
    return *this;
}

PooledBuffer::~PooledBuffer()
{
    Release();
}

void PooledBuffer::Release() noexcept
{
    if (!_hasStorage)
        return;

    if (_pool)
        _pool->Return(std::move(_storage));

    _storage.clear();
    _size = 0;
    _hasStorage = false;
}

std::byte* PooledBuffer::Data() noexcept
{
    return _hasStorage ? _storage.data() : nullptr;
}

std::byte const* PooledBuffer::Data() const noexcept
{
    return _hasStorage ? _storage.data() : nullptr;
}

std::size_t PooledBuffer::Size() const noexcept
{
    return _size;
}

std::size_t PooledBuffer::Capacity() const noexcept
{
    return _hasStorage ? _storage.size() : 0;
}

void PooledBuffer::Resize(std::size_t newSize) noexcept
{
    _size = std::min(newSize, Capacity());
}

std::span<std::byte> PooledBuffer::AsSpan() noexcept
{
    return std::span<std::byte> { Data(), _size };
}

std::span<std::byte const> PooledBuffer::AsSpan() const noexcept
{
    return std::span<std::byte const> { Data(), _size };
}

bool PooledBuffer::IsValid() const noexcept
{
    return _hasStorage;
}

// -- BufferPool ------------------------------------------------------------

BufferPool::BufferPool(std::size_t maxRetainedBuffers) noexcept: _maxRetained { maxRetainedBuffers } {}

std::shared_ptr<BufferPool> BufferPool::Create(std::size_t maxRetainedBuffers)
{
    // std::make_shared cannot reach a private ctor; wrap manually.
    return std::shared_ptr<BufferPool> { new BufferPool { maxRetainedBuffers } };
}

PooledBuffer BufferPool::Acquire(std::size_t minCapacity)
{
    auto const requested = minCapacity == 0 ? kDefaultBufferCapacity : minCapacity;

    std::vector<std::byte> picked;
    bool found = false;
    {
        std::lock_guard const lock { _mutex };
        auto const it = std::ranges::find_if(_free,
                                             [requested](auto const& slot) noexcept { return slot.size() >= requested; });
        if (it != _free.end())
        {
            picked = std::move(*it);
            _free.erase(it);
            found = true;
        }
    }

    if (!found)
        picked.resize(requested);

    return PooledBuffer { shared_from_this(), std::move(picked) };
}

std::size_t BufferPool::RetainedCount() const noexcept
{
    std::lock_guard const lock { _mutex };
    return _free.size();
}

void BufferPool::Return(std::vector<std::byte> storage) noexcept
{
    if (storage.empty() || _maxRetained == 0)
        return;

    std::lock_guard const lock { _mutex };
    if (_free.size() >= _maxRetained)
        return;
    _free.push_back(std::move(storage));
}

} // namespace FastCache
