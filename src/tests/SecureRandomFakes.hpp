// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/ISecureRandom.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// @file SecureRandomFakes.hpp
/// The one scripted `ISecureRandom` (#1527).
///
/// Shared rather than written per file, for `NodeProofFakes.hpp`'s reason: every case that
/// reaches for this asserts either that a nonce or an id CARRIES the scripted bytes, or that a
/// draw that FAILS is refused -- and a second copy that served its bytes in another order, or
/// failed in another way, would make both kinds of case pass for the wrong reason.

/// An `ISecureRandom` that serves scripted bytes, or refuses every draw.
///
/// **Two modes and one class**, because the two questions a case asks of this seam are *did the
/// bytes come from here* and *what happens when nothing can come from here*, and both are asked
/// of the same production call sites.
///
/// Scripted bytes are served in order across calls and CYCLE once exhausted, as
/// `ScriptedRandomSource` does. That makes the fake more permissive than the thing it stands for
/// in exactly one way, stated rather than hidden: a script shorter than what a case draws
/// REPEATS, where the real source never does. A case that needs two nonces to differ scripts at
/// least two nonces' worth -- `Ascending` is the spelling for that.
class ScriptedSecureRandom final: public ISecureRandom
{
  public:
    /// Serve @p script's bytes in order, cycling once exhausted.
    /// @param script The bytes. Empty serves zeroes, which no case should rely on.
    explicit ScriptedSecureRandom(std::vector<std::byte> script = {}):
        _script { std::move(script) }
    {
    }

    /// Refuse every draw with @p failure, and serve nothing.
    /// @param failure What each draw answers.
    explicit ScriptedSecureRandom(SecureRandomError failure):
        _failure { std::move(failure) }
    {
    }

    /// What a host whose generator is unreachable answers -- seccomp denying `getrandom`, say --
    /// under a primitive name no real implementation uses, so a case can assert that a refusal
    /// carries THIS failure rather than one from somewhere else.
    /// @return The failure.
    [[nodiscard]] static SecureRandomError DeniedFailure()
    {
        return SecureRandomError { .primitive = "scripted-getrandom", .detail = "Operation not permitted (errno 1)" };
    }

    /// @p count bytes counting up from @p first and wrapping at 256, so any two 32-byte windows
    /// within the first 256 bytes differ.
    /// @param count How many bytes.
    /// @param first The first byte's value.
    /// @return The bytes.
    [[nodiscard]] static std::vector<std::byte> Ascending(std::size_t count, std::uint8_t first = 0)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(count);
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            bytes.push_back(static_cast<std::byte>((first + index) & 0xFFU));
        return bytes;
    }

    /// From now on, refuse every draw with @p failure: a generator that served and then
    /// stopped, which is how a case reaches a failure on a draw that is not the first.
    /// @param failure What each later draw answers.
    void Deny(SecureRandomError failure)
    {
        std::scoped_lock const lock { _mutex };
        _failure = std::move(failure);
    }

    /// Serve @p fills more draws, then refuse every later one with @p failure: the generator
    /// that stops in the MIDDLE of an operation drawing more than once, which `Deny` between two
    /// calls cannot reach.
    /// @param fills How many more draws succeed.
    /// @param failure What each draw after them answers.
    void DenyAfter(std::size_t fills, SecureRandomError failure)
    {
        std::scoped_lock const lock { _mutex };
        _denyFrom = _fills + fills;
        _pendingFailure = std::move(failure);
    }

    /// @copydoc ISecureRandom::Fill
    [[nodiscard]] std::expected<void, SecureRandomError> Fill(std::span<std::byte> out) override
    {
        std::scoped_lock const lock { _mutex };

        // Counted before the failure is answered, so a case can assert the seam was ASKED in
        // exactly the case where nothing came back -- the question a refusal test depends on.
        ++_fills;
        if (_denyFrom.has_value() && _fills > *_denyFrom)
            _failure = _pendingFailure;
        if (_failure.has_value())
            return std::unexpected { *_failure };

        for (auto& byte: out)
            byte = _script.empty() ? std::byte { 0 } : _script[_served++ % _script.size()];
        return {};
    }

    /// How many times `Fill` was called, failed calls included.
    /// @return The count.
    [[nodiscard]] std::size_t FillCount() const
    {
        std::scoped_lock const lock { _mutex };
        return _fills;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::byte> _script;
    std::optional<SecureRandomError> _failure;
    std::optional<std::size_t> _denyFrom;
    std::optional<SecureRandomError> _pendingFailure;
    std::size_t _served { 0 };
    std::size_t _fills { 0 };
};

} // namespace FastCache::Testing
