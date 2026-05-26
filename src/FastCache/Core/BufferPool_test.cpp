// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BufferPool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

TEST_CASE("BufferPool::Acquire allocates a buffer of at least the requested capacity", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    auto buffer = pool->Acquire(64);
    REQUIRE(buffer.IsValid());
    REQUIRE(buffer.Capacity() >= 64);
    REQUIRE(buffer.Size() == buffer.Capacity());
}

TEST_CASE("BufferPool::Acquire(0) yields the default capacity", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    auto buffer = pool->Acquire(0);
    REQUIRE(buffer.IsValid());
    REQUIRE(buffer.Capacity() >= 1024);
}

TEST_CASE("Buffers returned to a pool are reused on the next acquire", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    std::byte* firstAddress = nullptr;
    {
        auto first = pool->Acquire(1024);
        firstAddress = first.Data();
    }
    REQUIRE(pool->RetainedCount() == 1);

    auto second = pool->Acquire(1024);
    REQUIRE(second.Data() == firstAddress);
    REQUIRE(pool->RetainedCount() == 0);
}

TEST_CASE("BufferPool honours its retention cap", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create(2);
    {
        auto a = pool->Acquire(64);
        auto b = pool->Acquire(64);
        auto c = pool->Acquire(64);
        REQUIRE(pool->RetainedCount() == 0);
    }
    REQUIRE(pool->RetainedCount() == 2);
}

TEST_CASE("Pool with zero retention never grows its free list", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create(0);
    for ([[maybe_unused]] auto const i : std::views::iota(0, 4))
    {
        auto buffer = pool->Acquire(128);
        REQUIRE(buffer.IsValid());
    }
    REQUIRE(pool->RetainedCount() == 0);
}

TEST_CASE("PooledBuffer::Resize clamps to capacity", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    auto buffer = pool->Acquire(256);
    REQUIRE(buffer.Capacity() >= 256);

    buffer.Resize(10);
    REQUIRE(buffer.Size() == 10);

    buffer.Resize(buffer.Capacity() * 4);
    REQUIRE(buffer.Size() == buffer.Capacity());
}

TEST_CASE("PooledBuffer move-construction transfers ownership", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    std::byte* heldAddress = nullptr;
    {
        auto source = pool->Acquire(64);
        heldAddress = source.Data();
        auto sink = std::move(source);
        REQUIRE(sink.IsValid());
        REQUIRE(sink.Data() == heldAddress);
    }
    // Both handles went out of scope; exactly one buffer was returned to the pool.
    REQUIRE(pool->RetainedCount() == 1);
}

TEST_CASE("PooledBuffer move-assignment returns the destination's previous buffer", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    {
        auto a = pool->Acquire(64);
        auto b = pool->Acquire(64);
        // Move-assign b into a: a's old buffer is released to the pool immediately,
        // b's buffer becomes a's. When the scope ends, the (now single) live buffer
        // is also returned.
        a = std::move(b);
        REQUIRE(a.IsValid());
        REQUIRE(pool->RetainedCount() == 1);
    }
    REQUIRE(pool->RetainedCount() == 2);
}

TEST_CASE("Acquire picks the first buffer that satisfies the minimum capacity", "[bufferpool]")
{
    auto const pool = FastCache::BufferPool::Create();
    // Seed the pool with two buffers of different sizes by acquiring then releasing.
    {
        auto small = pool->Acquire(64);
        auto large = pool->Acquire(8192);
    }
    REQUIRE(pool->RetainedCount() == 2);

    auto buffer = pool->Acquire(4096);
    REQUIRE(buffer.Capacity() >= 4096);
    REQUIRE(pool->RetainedCount() == 1);
}
