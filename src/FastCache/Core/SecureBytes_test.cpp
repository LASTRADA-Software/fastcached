// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/SecureBytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace
{

// The whole difficulty of testing this is that the evidence lives in memory the
// allocator has just handed back. Reading a freed heap block is undefined and ASan
// reports it, so the obvious test cannot be written -- and a test that only checks the
// container's own bytes proves nothing, because a `std::vector` keeps the secret on the
// heap and not in its footprint.
//
// So the test OWNS the storage. `Arena` is an ordinary object with a lifetime the test
// controls; `ArenaAllocator` hands out slices of it and never reuses or returns them.
// `SecureAllocator`'s real `deallocate` then runs against that arena, and afterwards the
// bytes are still a live object this test may read. Nothing is undefined, nothing is
// freed, and the wipe is observable by construction.
//
// This is the dependency-injection rule applied to memory: production substitutes
// `std::allocator` for the upstream and changes nothing else.

/// Test-owned storage that records every region handed out and never recycles one.
struct Arena
{
    /// Backing storage. Large enough for several reallocation steps of a small vector.
    std::array<std::byte, 4096> storage {};

    /// Bump cursor.
    std::size_t used = 0;

    /// Every region handed out, as (offset, byte length), oldest first.
    std::vector<std::pair<std::size_t, std::size_t>> handouts {};

    /// Read one recorded region back.
    /// @param which Index into `handouts`.
    /// @return View over the arena bytes that region occupies.
    [[nodiscard]] BytesView Region(std::size_t which) const
    {
        auto const [offset, length] = handouts.at(which);
        return BytesView { storage.data() + offset, length };
    }

    /// Which recorded region contains @p pointer.
    ///
    /// Regions are identified by the POINTER a container is using, never by their
    /// ordinal. Counting handouts was the first version and it was wrong on exactly one
    /// platform: MSVC's debug `std::vector` allocates a container-proxy object THROUGH
    /// the allocator for its iterator bookkeeping, so every container costs two handouts
    /// there and one everywhere else. That made `handouts.size() == 1` fail under
    /// `_ITERATOR_DEBUG_LEVEL=2` — and because it was a `REQUIRE`, it ABORTED each case
    /// before a single zeroing assertion ran. The property these cases exist to prove
    /// had therefore never been checked on Windows at all, while the suite reported the
    /// failure as an off-by-one in a count.
    ///
    /// Pinning the count to whatever MSVC happens to produce would fix the symptom and
    /// keep the defect: a number right on three platforms by construction and on the
    /// fourth by observation drifts the next time a standard library changes its
    /// bookkeeping. So the count is gone rather than corrected.
    ///
    /// @param pointer Address inside a region this arena handed out.
    /// @return Its index, or nullopt when the address is in no recorded region.
    [[nodiscard]] std::optional<std::size_t> IndexOf(void const* pointer) const
    {
        auto const* const base = reinterpret_cast<std::byte const*>(storage.data());
        auto const* const target = reinterpret_cast<std::byte const*>(pointer);
        for (std::size_t i = 0; i < handouts.size(); ++i)
        {
            auto const [offset, length] = handouts[i];
            if (target >= base + offset && target < base + offset + length)
                return i;
        }
        return std::nullopt;
    }
};

/// A monotonic bump allocator over an `Arena`. Deallocation is deliberately a no-op, so a
/// released region stays readable for the assertion that follows it.
template <typename T>
class ArenaAllocator
{
  public:
    using value_type = T;

    ArenaAllocator() = default;

    /// @param arena Storage to draw from; must outlive every container using this.
    explicit ArenaAllocator(Arena* arena) noexcept:
        _arena { arena }
    {
    }

    /// Allocator converting constructor. @param other Allocator to share the arena with.
    // Implicit by contract, and no suppression: `google-explicit-constructor` is not an
    // enabled check here, so a NOLINT would silence nothing while reading as a decision.
    template <typename U>
    ArenaAllocator(ArenaAllocator<U> const& other) noexcept:
        _arena { other.ArenaOf() }
    {
    }

    /// Bump-allocate @p n elements, recording the region.
    /// @param n Element count.
    /// @return Pointer into the arena.
    [[nodiscard]] T* allocate(std::size_t n)
    {
        auto const bytes = n * sizeof(T);
        auto offset = _arena->used;
        while (offset % alignof(T) != 0)
            ++offset;
        REQUIRE(offset + bytes <= _arena->storage.size());
        _arena->used = offset + bytes;
        _arena->handouts.emplace_back(offset, bytes);
        return reinterpret_cast<T*>(_arena->storage.data() + offset);
    }

    /// No-op: the arena never recycles, which is what keeps a released region readable.
    void deallocate(T* /*p*/, std::size_t /*n*/) noexcept {}

    /// @return The arena this allocator draws from.
    [[nodiscard]] Arena* ArenaOf() const noexcept
    {
        return _arena;
    }

    /// @param other Allocator to compare with.
    /// @return Whether both draw from the same arena.
    [[nodiscard]] bool operator==(ArenaAllocator const& other) const noexcept
    {
        return _arena == other._arena;
    }

  private:
    Arena* _arena = nullptr;
};

using ObservableAllocator = SecureAllocator<std::byte, ArenaAllocator<std::byte>>;
using ObservableBuffer = std::vector<std::byte, ObservableAllocator>;

/// Build a buffer drawing from @p arena. @param arena Storage to use. @return Empty buffer.
[[nodiscard]] ObservableBuffer MakeBuffer(Arena& arena)
{
    return ObservableBuffer { ObservableAllocator { ArenaAllocator<std::byte> { &arena } } };
}

/// A deliberate copy, made where the analyser can see the intent.
/// @param source Buffer to duplicate.
/// @return A second buffer holding the same bytes in storage of its own.
[[nodiscard]] ObservableBuffer CopyOf(ObservableBuffer const& source)
{
    return ObservableBuffer { source };
}

/// A recognisable stand-in for key material: no byte is zero, so "all zero" cannot be
/// satisfied by anything the secret itself contains.
constexpr std::array<std::byte, 32> SecretMaterial = [] {
    std::array<std::byte, 32> out {};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::byte>(0xA5U ^ (i + 1U));
    return out;
}();

/// @param region Bytes to inspect. @return Whether every byte is zero.
[[nodiscard]] bool AllZero(BytesView region)
{
    return std::ranges::all_of(region, [](std::byte b) { return b == std::byte { 0 }; });
}

} // namespace

TEST_CASE("a released credential buffer leaves zeroes in the storage it held", "[secure][secureallocator]")
{
    Arena arena;

    // The observation that makes this test mean something is a PAIR. "All zero after" on
    // its own is what an arena nobody ever wrote to also reports, so it would pass under
    // an allocator that was never reached, under a container that stored the secret
    // somewhere else, and under a fixture whose arena was the wrong object. The secret
    // has to be seen present in the storage FIRST, at the exact offsets the assertion
    // later reads, or the assertion is not about this credential at all.
    std::size_t held = 0;
    {
        auto buffer = MakeBuffer(arena);
        buffer.assign(SecretMaterial.begin(), SecretMaterial.end());

        // The region this buffer is USING, found by its pointer. Never `Region(0)`:
        // which ordinal a container's bytes land on is a property of the standard
        // library's own bookkeeping, not of this test.
        auto const found = arena.IndexOf(buffer.data());
        REQUIRE(found.has_value());
        held = Unwrap(found);

        auto const region = arena.Region(held);
        REQUIRE(region.size() >= SecretMaterial.size());

        // Present, at these offsets, before the buffer dies.
        CHECK(std::ranges::equal(region.first(SecretMaterial.size()), BytesView { SecretMaterial }));
        CHECK_FALSE(AllZero(region));
    }

    CHECK(AllZero(arena.Region(held)));
}

TEST_CASE("a reallocation zeroes the block it abandons", "[secure][secureallocator]")
{
    // The case a destructor-wiping wrapper cannot reach: the abandoned block is freed
    // while the credential is still very much alive, so a design that wipes only at the
    // end leaves a copy behind and nothing reports it.
    //
    // No holder in this tree grows a key buffer today -- `ReadClusterKey` sizes its one
    // allocation from `file_size` -- so this case is asserting cover for a change rather
    // than reproducing a live defect. That is worth having and worth being honest about:
    // the reason it is cheap to add now is exactly the reason nobody would think to add
    // it later, when some holder starts appending.
    Arena arena;
    auto buffer = MakeBuffer(arena);

    buffer.assign(SecretMaterial.begin(), SecretMaterial.end());
    auto const before = arena.IndexOf(buffer.data());
    REQUIRE(before.has_value());
    auto const abandoned = Unwrap(before);
    REQUIRE_FALSE(AllZero(arena.Region(abandoned)));

    // Force a move to a new block. `reserve` past capacity is the honest spelling: it
    // reallocates for the same reason an append does, without depending on any
    // implementation's growth factor.
    buffer.reserve(buffer.capacity() + SecretMaterial.size());

    // That it MOVED is asserted by the pointer changing region, which is the fact this
    // case needs -- not by a handout count, which counts allocations the standard library
    // makes for its own reasons.
    auto const after = arena.IndexOf(buffer.data());
    REQUIRE(after.has_value());
    REQUIRE(Unwrap(after) != abandoned);

    // The abandoned block is wiped, while the live one still holds the secret. Asserting
    // both is what separates "the old block was zeroed" from "everything was zeroed",
    // which is a fixture that would also pass with the credential destroyed early.
    CHECK(AllZero(arena.Region(abandoned)));
    CHECK(std::ranges::equal(BytesView { buffer.data(), buffer.size() }, BytesView { SecretMaterial }));
}

TEST_CASE("a moved-from credential buffer leaves no second copy", "[secure][secureallocator]")
{
    // A move steals the block rather than copying it, so there is nothing to wipe and the
    // one region stays live. Worth pinning: `SchedulerService` takes its signing key from
    // a span and `SignedLeaseValidator` takes one by value, so buffers here are moved and
    // copied, and a design that wiped on every transfer would be wrong in the other
    // direction -- it would clear a key that is still in use.
    Arena arena;
    auto source = MakeBuffer(arena);
    source.assign(SecretMaterial.begin(), SecretMaterial.end());
    auto const* const block = source.data();
    auto const found = arena.IndexOf(block);
    REQUIRE(found.has_value());
    auto const region = Unwrap(found);

    auto moved = std::move(source);

    // The block is STOLEN, not copied: the moved-to buffer is using the same address.
    // That is the property, and it is what "no second copy" means -- asserting a handout
    // count would instead assert how many allocations the standard library made, which is
    // two on MSVC debug for bookkeeping that has nothing to do with the secret.
    CHECK(moved.data() == block);
    CHECK(std::ranges::equal(BytesView { moved.data(), moved.size() }, BytesView { SecretMaterial }));
    CHECK_FALSE(AllZero(arena.Region(region)));
}

TEST_CASE("a copied credential buffer wipes each copy independently", "[secure][secureallocator]")
{
    Arena arena;
    auto original = MakeBuffer(arena);
    original.assign(SecretMaterial.begin(), SecretMaterial.end());
    auto const originalFound = arena.IndexOf(original.data());
    REQUIRE(originalFound.has_value());
    auto const originalRegion = Unwrap(originalFound);

    std::size_t copyRegion = 0;
    {
        // Through `CopyOf` rather than `auto copy = original;`. The copy IS the subject
        // here -- `SignedLeaseValidator` takes the key BY VALUE, so a second buffer with
        // its own block is the production shape -- but a local copy that is never
        // modified is what `performance-unnecessary-copy-initialization` exists to
        // refuse, and it cannot see that the point is the ALLOCATION rather than the
        // value. Returning it from a function states the intent where the check can read
        // it, which is the repair; a NOLINT would have been the suppression this project
        // forbids.
        auto copy = CopyOf(original);
        CHECK(copy.size() == original.size());

        auto const copyFound = arena.IndexOf(copy.data());
        REQUIRE(copyFound.has_value());
        copyRegion = Unwrap(copyFound);

        // A genuinely separate block, which is what makes the two wipes independent.
        REQUIRE(copyRegion != originalRegion);
        REQUIRE_FALSE(AllZero(arena.Region(copyRegion)));
    }

    // The copy's block is wiped and the original's is untouched. A fixture that only
    // looked at "some region is zero" would pass with these the other way round.
    CHECK(AllZero(arena.Region(copyRegion)));
    CHECK_FALSE(AllZero(arena.Region(originalRegion)));
}

TEST_CASE("SecureZero clears a region the caller owns", "[secure][securezero]")
{
    // The primitive on its own, over storage with an ordinary lifetime. What this cannot
    // observe is the property the primitive exists for -- that the write survives an
    // optimiser which can see the storage is dead -- because a test that reads the bytes
    // back is precisely a test in which the storage is NOT dead. That guarantee comes
    // from the platform primitive being specified to provide it, and the selection table
    // is in SecureBytes.cpp. Stating the limit here rather than implying coverage.
    std::array<std::byte, 64> region {};
    std::ranges::fill(region, std::byte { 0xC3 });
    REQUIRE_FALSE(AllZero(BytesView { region }));

    SecureZero(region.data(), region.size());

    CHECK(AllZero(BytesView { region }));
}

TEST_CASE("SecureZero accepts an empty region", "[secure][securezero]")
{
    // An empty credential is ordinary -- no `--cluster-key-file` is a supported
    // configuration -- and an empty `std::vector` may hold no allocation at all, so a
    // null pointer with a zero length reaches this on a perfectly healthy path.
    SecureZero(nullptr, 0);
    SUCCEED("a zero-length wipe is a no-op rather than a null dereference");
}

TEST_CASE("the production credential buffer is a drop-in for a byte vector", "[secure][secureallocator]")
{
    // `SecureByteBuffer` earns its keep only if adopting it at a holder is a type change
    // rather than a rewrite, so the span-taking shape every consumer uses is pinned here.
    SecureByteBuffer key;
    key.assign(SecretMaterial.begin(), SecretMaterial.end());

    BytesView const view { key };
    CHECK(view.size() == SecretMaterial.size());
    CHECK(std::ranges::equal(view, BytesView { SecretMaterial }));

    static_assert(std::is_same_v<SecureByteBuffer::value_type, std::byte>);
}
