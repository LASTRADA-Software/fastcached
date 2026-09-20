// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
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
/// hypothetical here — an `Ed25519KeyPair` is an ordinary copyable value whose secret half
/// is one of these, so a node's identity key exists in as many buffers as its key pair has
/// copies, with independent lifetimes, and each one's block is wiped when that buffer
/// dies. A destructor-wiping wrapper gets this right too, but only for holders somebody
/// remembered to wrap.
///
/// And it covers REALLOCATION, where a destructor-based design cannot: a vector that grows
/// moves its elements to a new block and frees the old one long before it dies, leaving a
/// copy the destructor will never see. **Two holders in this tree grow one**, and both
/// arrived with #1125: `ReadSecretFile` in `fastcache-cli/main.cpp` and in
/// `fastcache-compile-node/AdminEndpoint.cpp` each `insert` chunk after chunk into an
/// unreserved `SecureCharBuffer`, so a secret larger than one chunk is reallocated while
/// it is being read and every intermediate block is freed before the buffer dies. This
/// paragraph said there was no such holder, which was true when it was written and was
/// falsified by the commit that added it — a claim of the form *nothing does this yet* is
/// only ever as current as its last reader, and the two readers are named here so the next
/// change to them meets the reason instead of the count.
///
/// **What it does not cover, stated rather than left to be discovered.** A container that
/// stores small values inside its own footprint never calls an allocator for them, so this
/// seam cannot reach them. `std::vector` has no such optimisation and is fully covered.
/// `std::basic_string` does, and the threshold is a property of the STANDARD LIBRARY rather
/// than of the language: measured, an inline capacity of **15 on libstdc++ 14** and **22 on
/// libc++ 22**, so a 16-to-22-character secret is on the heap on Linux and inline on macOS.
/// One secret, one build, two answers.
///
/// **That is what `SecureString` below is for, and it resolves the question this paragraph
/// used to leave open** ([#1125](https://github.com/LASTRADA-Software/fastcached/issues/1125)).
/// The sentence here predicted *"a secret string needs this allocator AND an inline wipe"*.
/// The second half turned out to be avoidable rather than necessary, and avoiding it is the
/// stronger answer: a wipe of the inline footprint cannot be made correct, because a string
/// that GROWS past the threshold moves its characters to the heap and leaves the inline
/// bytes behind, and nothing can wipe them at that moment. Backing the text in a
/// `std::vector`, which has no such optimisation at all, removes the storage the wipe would
/// have had to chase — so there is one mechanism here, not two, which is the condition
/// #1125 set for itself.
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

/// An owning TEXT credential whose storage is zeroed at every release — with no bytes of
/// the secret anywhere inside the object itself.
///
/// **What this is for.** `SecureByteBuffer` covers a credential that arrives as bytes. The
/// ones that arrive as TEXT — `--requirepass`, the dashboard credential, the contents of
/// every `*-token-file` — were plain `std::string`s, released to the general allocator with
/// their characters intact
/// ([#1125](https://github.com/LASTRADA-Software/fastcached/issues/1125)).
///
/// **Why it is not `std::basic_string` with `SecureAllocator` in the third slot**, which is
/// the one-line version, is the obvious shape, and is what #1125 expected to be. There are
/// TWO gaps, they are not the same gap, and only the second one decides this:
///
///  - **Small-string optimisation.** Such a string is covered only while its characters are
///    on the heap, and whether they are is a property of the standard library rather than of
///    the program: 15 characters fit inline on libstdc++ 14 and MSVC STL 19.51, 22 on
///    libc++ 22. The same secret in the same source is heap-allocated on Linux and Windows
///    and inline on macOS.
///  - **The inline-to-heap transition, which has no answer at all.** A string that GROWS
///    past the threshold copies its characters to the heap and leaves the old ones sitting
///    in the inline buffer. A destructor-time wipe reaches the storage the object is using
///    NOW, never the residue of storage it has already left, and no allocator is called for
///    the buffer it abandoned. There is no hook at that moment.
///
/// **Read those in that order and stop at the first, and you will write the wrapper**: SSO
/// alone looks answerable, because an inline wipe answers it. It is the second that cannot
/// be wiped, only avoided — which is the same argument as *the wipe is an allocator, not a
/// destructor*, one step further along. A `std::vector` has no inline footprint to leave
/// behind and no transition to miss, so both gaps close by construction rather than by
/// anybody remembering.
///
/// So the characters live in a `std::vector`, which has no small-object optimisation, and
/// **every** byte of the secret is therefore in a block the allocator above releases — on
/// destruction, on assignment and on reallocation alike, identically on every platform. One
/// mechanism, which is the condition #1125 set for itself: this is `SecureAllocator`
/// instantiated over characters, not a second seam beside it.
///
/// **Not a `std::string` replacement, deliberately.** It carries `empty()`, `size()`, a
/// `std::string_view` and equality — which is the whole of what a credential is asked for
/// in this tree, and is why adopting it at a holder is a type change rather than a rewrite.
/// It grows no concatenation, no `c_str()` and no formatting: every one of those spells a
/// way to make a copy the allocator does not own.
///
/// @tparam Upstream Allocator the storage comes from, for the reason `SecureAllocator`
///                  states — a test injects an arena it OWNS so the wipe is observable
///                  without reading freed memory. Production substitutes `std::allocator`.
template <typename Upstream = std::allocator<char>>
class BasicSecureString
{
  public:
    /// The allocator this string's characters come from.
    using allocator_type = SecureAllocator<char, Upstream>;

    BasicSecureString() = default;

    /// Copy any text in.
    ///
    /// **Implicit, and the direction is the argument.** This conversion moves a secret
    /// OUT of ordinary storage and into the protected kind, so every place it fires
    /// silently is a place that got safer -- which is what lets the fifty-odd existing
    /// `AuthPolicy` and `AdminCredential` constructions keep compiling while their
    /// members change type. The reverse direction has no implicit path at all: there is
    /// no `operator std::string_view`, precisely so that handing a credential BACK to
    /// plain storage is a line somebody had to write (#1125).
    ///
    /// **A constrained template rather than a `std::string_view` parameter**, which is
    /// what this was first written as and what does not work: `AdminCredential { "tok" }`
    /// and `AuthPolicy { {}, std::string { "tok" } }` would each need TWO user-defined
    /// conversions -- to `std::string_view` and then to this -- and the language allows
    /// one. Spelling three overloads instead is the same rule written three times; the
    /// constraint says it once, and excluding this type is what keeps copy and move
    /// construction out of it.
    ///
    /// @tparam Text Anything a `std::string_view` can be built from -- `std::string`, a
    ///              literal, another view.
    /// @param text  The secret.
    // `google-explicit-constructor` is not among the enabled checks, so a suppression
    // here would silence nothing while looking like it silenced something.
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, BasicSecureString>
                 && std::convertible_to<Text const&, std::string_view>)
    BasicSecureString(Text const& text)
    {
        auto const view = std::string_view { text };
        _chars.assign(view.begin(), view.end());
    }

    /// Copy @p text into storage from @p allocator.
    /// @param text      The secret.
    /// @param allocator The allocator the characters are taken from.
    BasicSecureString(std::string_view text, allocator_type allocator):
        _chars { text.begin(), text.end(), std::move(allocator) }
    {
    }

    // **No `operator=(std::string_view)`, deliberately.** The converting constructor above
    // already makes `secret = someString` work -- it builds a temporary and move-assigns --
    // and declaring the assignment as well made that expression AMBIGUOUS between the two,
    // which broke every `AssignFrom<&Config::requirePass, ParseText>()` in the option
    // table. The move-assignment path is also the one that wipes: it releases this string's
    // current block through the allocator before taking the temporary's (#1125).

    /// @return True when no secret is held, which every holder reads as "auth disabled".
    [[nodiscard]] bool empty() const noexcept
    {
        return _chars.empty();
    }

    /// @return The secret's length in characters.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return _chars.size();
    }

    /// The secret, for a constant-time comparison to read.
    ///
    /// A VIEW rather than a string: the point of this type is that the characters exist in
    /// exactly one block, and a function returning `std::string` would hand out a second.
    /// @return A view over the characters; empty when none are held.
    /// The empty arm is deliberate: `std::vector::data()` may return null when the vector
    /// is empty, and `string_view { nullptr, 0 }` is only conditionally blessed -- the
    /// standard asks `[s, s + count)` to be a valid range even though the constructor reads
    /// nothing. This costs one predictable branch and removes the question.
    [[nodiscard]] std::string_view View() const noexcept
    {
        return _chars.empty() ? std::string_view {} : std::string_view { _chars.data(), _chars.size() };
    }

    /// Value equality, for the configuration table's `FieldEq` reload comparator.
    ///
    /// **Not constant-time, and that is right here**: it compares two values this process
    /// already holds — the running configuration against a reload candidate — so there is
    /// no attacker supplying either side and nothing a timing difference could leak.
    /// A secret compared against something a CLIENT sent goes through
    /// `ConstantTimeEquals`, never this.
    /// @param other The string to compare with.
    /// @return Whether both hold the same characters.
    [[nodiscard]] bool operator==(BasicSecureString const& other) const noexcept
    {
        return View() == other.View();
    }

  private:
    std::vector<char, allocator_type> _chars;
};

/// The production text credential: `BasicSecureString` over the general allocator.
using SecureString = BasicSecureString<>;

/// Character storage for a credential being ASSEMBLED, before there is a `SecureString` to
/// hold it.
///
/// The same allocator, spelled for the one job `SecureString` deliberately refuses to grow
/// an API for: a file reader needs mutable, resizable `char` storage to read INTO. Reading a
/// secret through a `std::ostringstream` and taking `.str()` is what the two `ReadSecretFile`
/// implementations used to do, and it leaves the secret in two containers nothing wipes --
/// the stream's own buffer and the string it hands back -- so changing only the RETURN type
/// would have been a half-fix that looked complete (#1125).
using SecureCharBuffer = std::vector<char, SecureAllocator<char>>;

} // namespace FastCache
