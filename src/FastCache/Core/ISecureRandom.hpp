// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// Why the operating system's random source could not fill a buffer.
struct SecureRandomError
{
    /// The primitive that refused, as its manual names it: `getrandom`, `getentropy` or
    /// `BCryptGenRandom` -- or whatever a test fake calls itself. Always a string literal.
    std::string_view primitive;

    /// What it answered, as text: the `errno` it set, or the `NTSTATUS` it returned.
    std::string detail;

    /// One sentence naming both, for a log line or a refusal.
    /// @return The sentence.
    [[nodiscard]] std::string ToString() const
    {
        return std::format("the operating system's random source ({}) failed: {}", primitive, detail);
    }
};

/// Bytes nobody can predict and no two draws share, from the operating system (#1527).
///
/// **A seam apart from `IRandomSource`, because the two answer different questions and fail
/// differently.** `IRandomSource` hands out values in a RANGE for election jitter and
/// tie-breaking, where a predictable or repeated draw costs a slower election and nothing
/// else, and where a fixed seed that replays a failure is the whole point. What this hands
/// out is what a handshake nonce and a minted node id are made of, and there a REPEAT is the
/// failure: a nonce drawn twice lets a recorded exchange be replayed whole, and an id minted
/// twice is two machines the cluster cannot tell apart.
///
/// **Why a seeded engine cannot stand in for this, which is what #1527 found.** Both of those
/// were drawn from `SystemRandomSource`, an `std::mt19937_64` seeded from `std::random_device`,
/// on the argument that a 64-bit-seeded engine meets a never-repeat bound. The argument
/// assumed its seed was random. On the WSL2/libstdc++ host #1507 was measured on,
/// `std::random_device` answers ZERO for 57% of draws (measured: 1652 of 2880, across 32
/// processes). Both seed halves zero is then roughly a third of processes (0.57 squared --
/// inferred, not reproduced), each drawing the same "random" stream, so an acceptor restarted
/// there can re-issue the challenges its previous run issued. A never-repeat bound needs an
/// ENTROPY SOURCE, and an engine is only ever as good as the one it was seeded from.
///
/// **A failure is a REFUSAL, and there is no fallback** -- not to `std::random_device`, not to
/// a clock, not to an engine. A fallback turns *this host cannot produce entropy* into *this
/// host produces a weak nonce and says nothing*, which is the defect this seam exists to
/// remove. Every caller that cannot draw stops what it was doing, by name.
class ISecureRandom
{
  public:
    ISecureRandom() = default;
    ISecureRandom(ISecureRandom const&) = delete;
    ISecureRandom(ISecureRandom&&) = delete;
    ISecureRandom& operator=(ISecureRandom const&) = delete;
    ISecureRandom& operator=(ISecureRandom&&) = delete;
    virtual ~ISecureRandom() = default;

    /// Fill @p out with bytes from the operating system's cryptographic generator.
    ///
    /// Safe to call from any thread and from several at once, as `IRandomSource` is: a source
    /// is shared by whatever collaborators were handed the same reference.
    /// @param out Where the bytes go. An empty span is filled trivially.
    /// @return Nothing, or why the bytes could not be drawn -- in which case what @p out holds
    ///         is unspecified and must not be used as a nonce, an id or anything else.
    [[nodiscard]] virtual std::expected<void, SecureRandomError> Fill(std::span<std::byte> out) = 0;
};

/// The production `ISecureRandom`: the operating system's CSPRNG, read directly.
///
/// One primitive per platform, and the .cpp says why each: `getrandom(2)` on Linux,
/// `getentropy(2)` on macOS and the other POSIX hosts, `BCryptGenRandom` with the
/// system-preferred generator on Windows. Holds no state, so one instance may serve a whole
/// process.
class SystemSecureRandom final: public ISecureRandom
{
  public:
    /// @copydoc ISecureRandom::Fill
    [[nodiscard]] std::expected<void, SecureRandomError> Fill(std::span<std::byte> out) override;
};

} // namespace FastCache
