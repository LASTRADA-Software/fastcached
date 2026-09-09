// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

namespace FastCache
{

/// Overwrite a region with zeroes, in a way the optimiser is not permitted to remove.
///
/// A plain `std::memset` over storage that is about to die is a dead store, and every
/// compiler this project builds with is entitled to delete it. That is not a theoretical
/// entitlement: it is why the platforms provide a primitive at all. The implementation
/// selects one per host; the table and what each guarantee rests on are in the .cpp.
///
/// @param data  Start of the region. May be null only when @p bytes is zero.
/// @param bytes Length of the region in bytes.
void SecureZero(void* data, std::size_t bytes) noexcept;

/// An allocator that zeroes every region before handing it back to @p Upstream.
///
/// **Why an allocator rather than a wrapper that wipes in its destructor.** Every release
/// goes through one door, so no holder has a line it can forget to write. That is the
/// `Refuse`/`ClaimReadSlot` argument: a guard folded INTO the operation is self-enforcing,
/// a guard called alongside one needs a scan to stay true.
///
/// It is also what makes COPIES safe without anybody thinking about them, which is not
/// hypothetical here — `SignedLeaseValidator` takes the key **by value** and
/// `SchedulerService` builds its own from a span, so the one secret exists in several
/// buffers with independent lifetimes, and each one's block is wiped when that buffer
/// dies. A destructor-wiping wrapper gets this right too, but only for holders somebody
/// remembered to wrap.
///
/// And it covers REALLOCATION, where a destructor-based design cannot: a vector that grows
/// moves its elements to a new block and frees the old one long before it dies, leaving a
/// copy the destructor will never see. **No holder in this tree grows one today** —
/// `ReadClusterKey` sizes its buffer once from `file_size` and `SchedulerService` builds
/// from a pair of iterators — so this is cover for a change nobody would connect to the
/// secret, rather than a defect being fixed. Said plainly because the tempting version of
/// this sentence cites a growing holder, and there isn't one.
///
/// **What it does not cover, stated rather than left to be discovered.** A container that
/// stores small values inside its own footprint never calls an allocator for them, so this
/// seam cannot reach them. `std::vector` has no such optimisation and is fully covered.
/// `std::basic_string` does, and the threshold is a property of the STANDARD LIBRARY rather
/// than of the language: measured, an inline capacity of **15 on libstdc++ 14** and **22 on
/// libc++ 22**, so a 16-to-22-character secret is on the heap on Linux and inline on macOS.
/// One secret, one build, two answers. A secret string therefore needs this allocator AND
/// an inline wipe, and that is
/// [#1125](https://github.com/LASTRADA-Software/fastcached/issues/1125) rather than a gap
/// somebody has to rediscover here.
///
/// **Why not `std::pmr`.** A `memory_resource` would inject the arena without a template
/// parameter, and it was considered. It loses on two counts that matter here rather than
/// on taste: the whole point of the alias below is that adopting it at a holder is a type
/// change and nothing else, and `std::pmr::vector` puts a `memory_resource*` inside every
/// `DiscoveryConfig` and `SchedulerService` that has to be pointed somewhere; and pmr's
/// default resource is process-global mutable state, which is what the injection rule in
/// AGENT.md exists to keep out of this tree.
///
/// @tparam T        Element type.
/// @tparam Upstream Allocator the storage actually comes from. It is a parameter so a test
///                  can supply an arena it OWNS: the bytes stay readable after the
///                  container is destroyed, which is what makes the wipe observable at all
///                  without reading freed memory. That is the injection rule applied to
///                  memory — production substitutes `std::allocator` and changes nothing.
template <typename T, typename Upstream = std::allocator<T>>
class SecureAllocator
{
  public:
    using value_type = T;

    /// The upstream, rebound to this allocator's element type.
    ///
    /// No `typename`: it is optional in an alias-declaration since C++20 (P0634R3), and
    /// `readability-redundant-typename` is an error here under `WarningsAsErrors`. The
    /// `template` disambiguator is a different question and is still required.
    using UpstreamAllocator = std::allocator_traits<Upstream>::template rebind_alloc<T>;

    /// Interchangeable whenever the upstream carries no state — which is the production
    /// case, `std::allocator` being empty. Left to be DERIVED rather than asserted: the
    /// arena a test injects holds a pointer, so this is correctly false there and the
    /// container does the comparison it should.
    using is_always_equal = std::is_empty<UpstreamAllocator>;

    /// Rebinding keeps the upstream selection and changes only the element type.
    template <typename U>
    struct rebind
    {
        using other = SecureAllocator<U, Upstream>;
    };

    SecureAllocator() = default;

    /// Adopt a specific upstream instance (an arena in a test, a stateful allocator
    /// in principle). @param upstream The allocator storage is taken from.
    explicit SecureAllocator(UpstreamAllocator upstream) noexcept:
        _upstream { std::move(upstream) }
    {
    }

    /// Converting constructor, required of every allocator: a container rebinds to
    /// allocate its own node or character type. @param other Allocator to copy the
    /// upstream from.
    // Implicit by contract: a container rebinds through this without asking. The project
    // forbids NOLINT, and none is needed -- `google-explicit-constructor` is not among the
    // enabled checks, so a suppression here would silence nothing while looking like it
    // silenced something.
    template <typename U>
    SecureAllocator(SecureAllocator<U, Upstream> const& other) noexcept:
        _upstream { other.UpstreamRef() }
    {
    }

    /// Allocate uninitialised storage for @p n elements.
    /// @param n Element count.
    /// @return Pointer to the storage.
    [[nodiscard]] T* allocate(std::size_t n)
    {
        return std::allocator_traits<UpstreamAllocator>::allocate(_upstream, n);
    }

    /// Zero the region, then release it upstream.
    /// @param p Pointer previously returned by allocate().
    /// @param n Element count it was allocated with.
    void deallocate(T* p, std::size_t n) noexcept
    {
        SecureZero(p, n * sizeof(T));
        std::allocator_traits<UpstreamAllocator>::deallocate(_upstream, p, n);
    }

    /// The upstream this allocator draws from; public so a rebound instance can adopt it.
    /// @return The upstream allocator.
    [[nodiscard]] UpstreamAllocator const& UpstreamRef() const noexcept
    {
        return _upstream;
    }

    /// Two secure allocators are interchangeable exactly when their upstreams are.
    /// @param other Allocator to compare with.
    /// @return Whether storage from one may be released by the other.
    [[nodiscard]] bool operator==(SecureAllocator const& other) const noexcept
    {
        return _upstream == other._upstream;
    }

  private:
    UpstreamAllocator _upstream {};
};

/// An owning byte buffer whose storage is zeroed at every release, reallocation included.
///
/// This is what a credential lives in. It is deliberately a `std::vector` alias rather than
/// a bespoke class: every span-taking interface keeps working unchanged, so adopting it at
/// a holder is a type change and not a rewrite.
using SecureByteBuffer = std::vector<std::byte, SecureAllocator<std::byte>>;

} // namespace FastCache
