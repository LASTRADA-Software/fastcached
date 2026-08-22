// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

namespace FastCache
{

/// Randomness provider abstraction, and the third member of the ambient-resource
/// family beside `IClock` and `ISocket`.
///
/// Randomness is exactly as untestable as wall-clock time when it is reached for
/// directly, and for the same reason: a decision that depends on it cannot be
/// reproduced, so the test that would pin the decision is written against a
/// `sleep`-and-hope or not written at all. Every caller therefore takes an
/// `IRandomSource&` for the same reason it takes an `IClock&`.
///
/// ## Why the primitive is a bounded draw rather than raw bits
///
/// Every caller in this codebase wants a value in a range, and turning uniform
/// bits into a uniform *range* without modulo bias is a small, easy-to-get-wrong
/// computation. Exposing `NextBits()` would mean that correction living at each
/// call site, differing subtly between them — the copy-pasted-logic-differing-by-a-
/// constant that the data-driven design principle exists to forbid. Handing out
/// the range instead keeps the debiasing in one place, where the standard library
/// already does it.
class IRandomSource
{
  public:
    IRandomSource() = default;
    IRandomSource(IRandomSource const&) = delete;
    IRandomSource(IRandomSource&&) = delete;
    IRandomSource& operator=(IRandomSource const&) = delete;
    IRandomSource& operator=(IRandomSource&&) = delete;
    virtual ~IRandomSource() = default;

    /// Draw a uniformly distributed value from an inclusive range.
    ///
    /// Must be safe to call from any thread and from several at once, matching
    /// `IClock::Now()`: a source is shared by whatever collaborators were handed
    /// the same reference, and requiring each of them to know which thread the
    /// others run on would defeat the point of injecting it.
    /// @param lowInclusive Smallest value that may be returned.
    /// @param highInclusive Largest value that may be returned. Equal bounds are
    ///        legal and yield that one value; an *inverted* range is a caller bug
    ///        and is defined to collapse to `[lowInclusive, lowInclusive]`.
    /// @return A value in `[lowInclusive, max(lowInclusive, highInclusive)]`.
    [[nodiscard]] virtual std::uint64_t UniformInRange(std::uint64_t lowInclusive, std::uint64_t highInclusive) = 0;

  protected:
    /// Normalize a possibly-inverted range, so no implementation can reach the
    /// standard library with one.
    ///
    /// An inverted range is a caller bug, but `assert` alone does not make it a
    /// *diagnosed* one: `NDEBUG` is set in every shipped preset, and both
    /// `std::uniform_int_distribution` and `std::clamp` are undefined when the
    /// bounds cross. libstdc++ computes `high - low`, underflows to nearly 2^64,
    /// and hands back a value outside the range the caller asked for — so the
    /// first caller, Raft's election-timeout jitter, would draw an astronomical
    /// timeout from a config whose minimum exceeded its maximum. The node then
    /// never stands for election, the cluster never elects a leader, and every
    /// node still reports itself healthy: the "looks fine from both ends" failure
    /// this codebase already records twice on the dispatch path.
    ///
    /// Collapsing to the low bound rather than swapping is the deliberate choice.
    /// Swapping invents a range the caller never asked for and makes a
    /// configuration mistake behave plausibly, which is how it survives to
    /// production; collapsing yields a degenerate, obviously-constant draw. It is
    /// a floor, not a diagnosis — the operator-facing rejection belongs where the
    /// two bounds are read from configuration, which is the only layer that knows
    /// they came from a file and can name it.
    ///
    /// Deliberately **not** also an `assert`. Defined behaviour and an assertion
    /// contradict each other — one says the case is handled, the other that it
    /// must never arise — and the assertion is the half that is wrong here,
    /// because these bounds reach this seam from a config file. Aborting a daemon
    /// over an operator's typo is a worse answer than a degenerate timeout, and it
    /// would also make the defined behaviour untestable in the only builds that
    /// check it.
    /// @param lowInclusive Smallest value that may be returned.
    /// @param highInclusive Proposed upper bound.
    /// @return `highInclusive`, or `lowInclusive` when the range is inverted.
    [[nodiscard]] static constexpr std::uint64_t NormalizeHigh(std::uint64_t lowInclusive,
                                                               std::uint64_t highInclusive) noexcept
    {
        return std::max(lowInclusive, highInclusive);
    }
};

/// Default `IRandomSource`: a Mersenne twister seeded from `std::random_device`.
///
/// Not cryptographic, and deliberately so — the callers are timeout jitter and
/// tie-breaking, where predictability costs nothing an attacker on the build
/// network could not already do more cheaply. A caller that needs unguessable
/// bytes (a token, a nonce) needs a different seam, not a stronger engine behind
/// this one, because the two have different failure modes and this one's contract
/// promises nothing about them.
class SystemRandomSource final: public IRandomSource
{
  public:
    /// Construct seeded from the platform entropy source.
    SystemRandomSource():
        _engine { SeedFromDevice() }
    {
    }

    /// Construct with a fixed seed, so a failure that depended on a particular
    /// draw sequence can be replayed outside the test double.
    /// @param seed Value to seed the engine with.
    explicit SystemRandomSource(std::uint64_t seed) noexcept:
        _engine { seed }
    {
    }

    [[nodiscard]] std::uint64_t UniformInRange(std::uint64_t lowInclusive, std::uint64_t highInclusive) override
    {
        auto const high = NormalizeHigh(lowInclusive, highInclusive);
        // The lock is not free, but a draw happens once per election timeout —
        // that is, at a rate measured in seconds — so it is not on any hot path,
        // and the alternative is a data race the contract above promises not to
        // have.
        std::scoped_lock const lock { _mutex };
        return std::uniform_int_distribution<std::uint64_t> { lowInclusive, high }(_engine);
    }

  private:
    /// A full-width seed drawn from the platform entropy source.
    ///
    /// Two draws rather than one: `std::random_device::result_type` is 32 bits on
    /// every implementation here, so seeding a 64-bit engine from a single call
    /// leaves at most 2^32 distinct streams. Harmless for a handful of nodes and
    /// free to remove, and it also makes the default constructor's seed width
    /// match the explicit one's, so the two cannot be reasoned about differently.
    /// @return A 64-bit seed.
    [[nodiscard]] static std::uint64_t SeedFromDevice()
    {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
    }

    std::mutex _mutex;
    std::mt19937_64 _engine;
};

/// Test `IRandomSource` returning a scripted sequence, cycling once exhausted.
///
/// The `ManualClock` of randomness, and named for `ScriptedSignalSource`, which
/// is the same idea applied to signals: a test states the draws it wants and the
/// behaviour under test becomes reproducible.
///
/// Each scripted value is **clamped** into the range the caller asks for, which
/// makes the two cases tests actually want expressible without knowing the range
/// at the point the script is written: `0` always draws the low bound and
/// `Highest()` always draws the high one. Clamping rather than folding with a
/// modulo is the deliberate choice — folding would map those same two values to
/// arbitrary positions, which is precisely the property a scripted source exists
/// to remove.
class ScriptedRandomSource final: public IRandomSource
{
  public:
    /// Construct with the draws to serve, in order.
    /// @param draws Values to return, clamped per call and cycled once exhausted.
    ///        An empty script always draws the low bound.
    explicit ScriptedRandomSource(std::vector<std::uint64_t> draws = {}) noexcept:
        _draws { std::move(draws) }
    {
    }

    /// A scripted value that always clamps to whatever the high bound is.
    /// @return The largest representable draw.
    [[nodiscard]] static constexpr std::uint64_t Highest() noexcept
    {
        return ~std::uint64_t { 0 };
    }

    [[nodiscard]] std::uint64_t UniformInRange(std::uint64_t lowInclusive, std::uint64_t highInclusive) override
    {
        auto const high = NormalizeHigh(lowInclusive, highInclusive);
        std::scoped_lock const lock { _mutex };

        // Counted before the empty-script shortcut, not after. `DrawCount` exists
        // to let a test assert that a decision consulted the source at all, and a
        // counter that stops counting for the default script answers that question
        // wrongly in exactly the case it is being asked.
        auto const drawIndex = _next++;
        if (_draws.empty())
            return lowInclusive;

        return std::clamp(_draws[drawIndex % _draws.size()], lowInclusive, high);
    }

    /// How many draws have been served. Lets a test assert that a decision it
    /// expected to be randomized actually consulted the source.
    /// @return The number of `UniformInRange` calls so far.
    [[nodiscard]] std::size_t DrawCount() const noexcept
    {
        std::scoped_lock const lock { _mutex };
        return _next;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::uint64_t> _draws;
    std::size_t _next { 0 };
};

} // namespace FastCache
