// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace FastCache
{

class BufferPool;

/// RAII handle for a buffer borrowed from a BufferPool. On destruction the
/// buffer is returned to the pool (or freed if the pool has gone away). The
/// caller may write to .Data() / read from .Data() up to .Capacity() bytes;
/// the size reported via .Size() is whatever the caller has set with .Resize().
///
/// The handle is move-only — copying a buffer would defeat the pooling.
class PooledBuffer
{
  public:
    PooledBuffer() = default;
    PooledBuffer(PooledBuffer const&) = delete;
    PooledBuffer& operator=(PooledBuffer const&) = delete;

    PooledBuffer(PooledBuffer&& other) noexcept;
    PooledBuffer& operator=(PooledBuffer&& other) noexcept;

    ~PooledBuffer();

    /// @return Mutable byte pointer; nullptr only on a moved-from / default-constructed handle.
    [[nodiscard]] std::byte* Data() noexcept;

    /// @return Const byte pointer; nullptr only on a moved-from handle.
    [[nodiscard]] std::byte const* Data() const noexcept;

    /// @return Current logical size as set by Resize(). Initially equal to Capacity().
    [[nodiscard]] std::size_t Size() const noexcept;

    /// @return Allocation capacity. Always >= Size().
    [[nodiscard]] std::size_t Capacity() const noexcept;

    /// Update the logical size. Must be <= Capacity(); does not reallocate.
    /// @param newSize New logical size in bytes.
    void Resize(std::size_t newSize) noexcept;

    /// @return Mutable byte span covering [Data(), Data()+Size()).
    [[nodiscard]] std::span<std::byte> AsSpan() noexcept;

    /// @return Read-only byte span covering [Data(), Data()+Size()).
    [[nodiscard]] std::span<std::byte const> AsSpan() const noexcept;

    /// @return true if this handle owns a live allocation.
    [[nodiscard]] bool IsValid() const noexcept;

  private:
    friend class BufferPool;

    PooledBuffer(std::shared_ptr<BufferPool> pool, std::vector<std::byte> storage) noexcept;

    void Release() noexcept;

    std::shared_ptr<BufferPool> _pool;
    std::vector<std::byte> _storage;
    std::size_t _size { 0 };
    bool _hasStorage { false };
};

/// Pool of reusable byte buffers, intended to be held per-Connection (one
/// per accepted client) so request parsing / response building does not hit
/// the allocator on the hot path.
///
/// The pool stores buffers of any size — they are returned to the pool with
/// their full capacity preserved. On Acquire(minCapacity), the pool returns
/// the first buffer with capacity >= minCapacity, or allocates a fresh one
/// when none qualify.
///
/// Thread safety: all public methods are safe to call concurrently. The
/// expected usage is single-threaded per Connection, but a global pool
/// shared between Connections is also valid.
class BufferPool: public std::enable_shared_from_this<BufferPool>
{
  public:
    /// Construct a pool with the given soft cap on retained buffer count.
    /// Returned buffers in excess of maxRetainedBuffers are simply freed.
    /// @param maxRetainedBuffers Upper bound on resident buffers. Zero disables retention.
    /// @return Shared owner of the new pool.
    [[nodiscard]] static std::shared_ptr<BufferPool> Create(std::size_t maxRetainedBuffers = 32);

    BufferPool(BufferPool const&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool const&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;
    ~BufferPool() = default;

    /// Acquire a buffer with at least minCapacity bytes of capacity. The
    /// returned handle's Size() equals its Capacity() on return; the caller
    /// can Resize() it down as needed.
    /// @param minCapacity Minimum capacity in bytes (may be 0 — yields a default-size buffer).
    /// @return Owning handle for a usable buffer.
    [[nodiscard]] PooledBuffer Acquire(std::size_t minCapacity);

    /// @return Number of buffers currently retained in the free list.
    [[nodiscard]] std::size_t RetainedCount() const noexcept;

    /// @return Maximum number of buffers that will be retained on return.
    [[nodiscard]] std::size_t MaxRetained() const noexcept
    {
        return _maxRetained;
    }

  private:
    friend class PooledBuffer;

    /// Constructor is private — use Create().
    explicit BufferPool(std::size_t maxRetainedBuffers) noexcept;

    /// Return a buffer to the pool. Called by PooledBuffer's destructor.
    /// @param storage Buffer to return; capacity preserved, contents not zeroed.
    void Return(std::vector<std::byte> storage) noexcept;

    std::size_t _maxRetained;
    mutable std::mutex _mutex;
    std::vector<std::vector<std::byte>> _free;
};

} // namespace FastCache
