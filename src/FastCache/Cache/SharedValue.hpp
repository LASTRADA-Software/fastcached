// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace FastCache
{

/// Immutable, reference-counted byte payload stored in a **single heap
/// allocation**: the reference-count header and the value bytes are carved out
/// of one block, so constructing a value costs one `operator new` rather than
/// the two an `std::shared_ptr<std::vector<std::byte>>` would (control block +
/// the vector's separate buffer). This matters on the store hot path, where
/// every Set/Add/Replace allocates exactly one value.
///
/// The bytes are immutable once constructed. Sharing is via `SharedValue`
/// (below), a custom intrusive smart pointer; copying it is a single atomic
/// increment, and a GET hands one out instead of copying the payload — the
/// zero-copy read path's core saving.
class ImmutableBytes
{
  public:
    ImmutableBytes(ImmutableBytes const&) = delete;
    ImmutableBytes(ImmutableBytes&&) = delete;
    ImmutableBytes& operator=(ImmutableBytes const&) = delete;
    ImmutableBytes& operator=(ImmutableBytes&&) = delete;
    ~ImmutableBytes() = default;

    /// @return The payload size in bytes.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return _size;
    }

    /// @return Pointer to the immutable payload bytes (valid for `size()`).
    [[nodiscard]] std::byte const* data() const noexcept
    {
        // The bytes live immediately after this object in the same block.
        return reinterpret_cast<std::byte const*>(this) + sizeof(ImmutableBytes);
    }

    /// @return A read-only span over the payload.
    [[nodiscard]] std::span<std::byte const> AsSpan() const noexcept
    {
        return std::span<std::byte const> { data(), _size };
    }

    /// Allocate a buffer of `n` bytes and copy `src` into it (one allocation).
    /// @param src Source bytes to copy.
    /// @return An owning intrusive pointer to the new immutable buffer.
    [[nodiscard]] static class SharedValue Make(std::span<std::byte const> src);

  private:
    friend class SharedValue;

    explicit ImmutableBytes(std::size_t size) noexcept:
        _size { size }
    {
    }

    /// Mutable byte pointer — used only by the factory before the buffer is
    /// published (and thus still exclusively owned), never after.
    [[nodiscard]] std::byte* MutableData() noexcept
    {
        return reinterpret_cast<std::byte*>(this) + sizeof(ImmutableBytes);
    }

    void AddRef() const noexcept
    {
        _refs.fetch_add(1, std::memory_order_relaxed);
    }

    void Release() const noexcept
    {
        if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            this->~ImmutableBytes();
            ::operator delete(const_cast<ImmutableBytes*>(this), std::align_val_t { alignof(ImmutableBytes) });
        }
    }

    mutable std::atomic<std::size_t> _refs { 1 };
    std::size_t _size;
};

/// Reference-counted handle to an `ImmutableBytes` payload — the storage
/// representation of a cached value. Copying is one atomic increment; a GET
/// hands out a copy instead of duplicating the bytes. The buffer is immutable,
/// so a reader holding a handle keeps a stable, valid payload even if a
/// concurrent writer rebinds the entry to a new value (copy-on-write).
///
/// Null-state (default-constructed) represents "no value", e.g. the entry in a
/// miss `GetResult`; use the null-safe accessors on `CacheEntry`.
class SharedValue
{
  public:
    SharedValue() noexcept = default;

    SharedValue(SharedValue const& other) noexcept:
        _ptr { other._ptr }
    {
        if (_ptr)
            _ptr->AddRef();
    }

    SharedValue(SharedValue&& other) noexcept:
        _ptr { std::exchange(other._ptr, nullptr) }
    {
    }

    SharedValue& operator=(SharedValue const& other) noexcept
    {
        if (this != &other)
        {
            if (other._ptr)
                other._ptr->AddRef();
            if (_ptr)
                _ptr->Release();
            _ptr = other._ptr;
        }
        return *this;
    }

    SharedValue& operator=(SharedValue&& other) noexcept
    {
        if (this != &other)
        {
            if (_ptr)
                _ptr->Release();
            _ptr = std::exchange(other._ptr, nullptr);
        }
        return *this;
    }

    ~SharedValue()
    {
        if (_ptr)
            _ptr->Release();
    }

    /// @return True if this handle owns a payload (non-null).
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return _ptr != nullptr;
    }

    /// @return The payload size in bytes, or 0 when null.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return _ptr ? _ptr->size() : 0;
    }

    /// @return A read-only span over the payload, or an empty span when null.
    [[nodiscard]] std::span<std::byte const> Bytes() const noexcept
    {
        return _ptr ? _ptr->AsSpan() : std::span<std::byte const> {};
    }

    /// Type-erased owner suitable for an I/O keep-alive: a `shared_ptr<void
    /// const>` whose deleter releases this handle's reference. The returned
    /// shared_ptr keeps the payload alive independently of this handle.
    /// @return A shared_ptr that pins the payload for as long as it lives.
    [[nodiscard]] std::shared_ptr<void const> AsKeepAlive() const
    {
        if (!_ptr)
            return {};
        _ptr->AddRef();
        ImmutableBytes const* raw = _ptr;
        return std::shared_ptr<void const> { static_cast<void const*>(raw->data()), [raw](void const*) noexcept {
                                                raw->Release();
                                            } };
    }

  private:
    friend class ImmutableBytes;

    /// Take ownership of a freshly-allocated block of at least
    /// `sizeof(ImmutableBytes) + size` bytes, constructing the header in place.
    /// Refcount starts at 1; this handle is the sole owner. Keeping the
    /// placement-new here means the factory never holds a typed owner pointer.
    SharedValue(void* block, std::size_t size) noexcept:
        _ptr { ::new(block) ImmutableBytes { size } }
    {
    }

    ImmutableBytes* _ptr { nullptr };
};

inline SharedValue ImmutableBytes::Make(std::span<std::byte const> src)
{
    // One allocation for the header + the payload bytes. The block is handed
    // straight to an intrusive SharedValue, which owns it from here on (refcount
    // starts at 1) and frees it via Release() — so this function never leaks and
    // the raw pointer never escapes as an unmanaged owner. The construction is
    // funnelled through SharedValue's block ctor so no typed owner pointer is
    // materialised in this scope.
    auto const total = sizeof(ImmutableBytes) + src.size();
    void* const block = ::operator new(total, std::align_val_t { alignof(ImmutableBytes) });
    SharedValue handle { block, src.size() };
    if (!src.empty())
        std::memcpy(handle._ptr->MutableData(), src.data(), src.size());
    return handle;
}

/// Construct a `SharedValue` owning a copy of `bytes` (single allocation).
/// @param bytes Payload to store.
/// @return A reference-counted handle to the immutable payload.
[[nodiscard]] inline SharedValue MakeSharedValue(std::vector<std::byte> const& bytes)
{
    return ImmutableBytes::Make(std::span<std::byte const> { bytes.data(), bytes.size() });
}

/// Construct a `SharedValue` from a raw byte span (single allocation).
/// @param bytes Payload to copy.
/// @return A reference-counted handle to the immutable payload.
[[nodiscard]] inline SharedValue MakeSharedValue(std::span<std::byte const> bytes)
{
    return ImmutableBytes::Make(bytes);
}

} // namespace FastCache
