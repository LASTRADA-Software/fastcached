// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace FastCache
{

/// A bounds-checked walk over a byte span, carrying the declared-count guard.
///
/// `ReadCount` **cannot be called without stating the per-element bound**. That is the
/// whole point of the type: the guard stops being a step a decoder author must remember
/// and becomes part of the only call that yields a count. Why that had to become
/// structural — four instances of one defect, each written by somebody who had
/// correctly bounds-checked the *length* fields in the same function — is argued in
/// `.agent/rules/wire-and-protocol.md`, which is the canonical home for it. Read that
/// before changing anything here.
///
/// It lives in `Core/`, beside `WireFields`, under the constraint that header's own
/// note states and this one inherits: it must be includable from a header
/// `fastcache-cc` compiles **without linking `FastCache`**, so it stays header-only and
/// reaches nothing but `Core/Endian.hpp`, `Core/WireFields.hpp` and the standard
/// library. Keep it that way — a member that would pull in a new dependency belongs
/// somewhere else.
///
/// The surface is deliberately only what a caller reaches today. `CompileValue`,
/// `PrefetchGroupManifest` and `DirectManifest` still hand-roll a cursor, and
/// converting them ([#304](https://github.com/LASTRADA-Software/fastcached/issues/304))
/// is what should add the members they need — validated against a real caller rather
/// than guessed ahead of one.
///
/// A failed read leaves the cursor failed, so a decoder may check each read or check
/// `Ok()` once at the end; both are correct and the second cannot be fooled by a later
/// read appearing to succeed. Nothing advances after a failure.
class ByteCursor
{
  public:
    /// @param bytes The buffer to walk.
    /// @param offset Where to begin, for a caller that has already read a fixed
    ///        header. A past-the-end offset simply starts the cursor exhausted.
    explicit constexpr ByteCursor(std::span<std::byte const> bytes, std::size_t offset = 0) noexcept:
        _bytes { bytes },
        _offset { offset < bytes.size() ? offset : bytes.size() }
    {
    }

    /// @return True when every byte has been consumed and nothing failed.
    ///
    /// The spelling for "no trailing bytes", in preference to comparing a remaining
    /// count against zero: a *failed* cursor has no bytes remaining either, so the
    /// subtraction-based spelling reports a malformed frame as a clean one.
    [[nodiscard]] constexpr bool AtEnd() const noexcept
    {
        return _ok && _offset == _bytes.size();
    }

    /// @return False once any read has failed.
    [[nodiscard]] constexpr bool Ok() const noexcept
    {
        return _ok;
    }

    /// Read one byte.
    /// @param out Receives the value.
    /// @return True on success; false leaves the cursor failed.
    [[nodiscard]] bool ReadU8(std::uint8_t& out) noexcept
    {
        if (!Has(1))
            return false;
        out = static_cast<std::uint8_t>(_bytes[_offset]);
        _offset += 1;
        return true;
    }

    /// Read a big-endian `u64`.
    /// @param out Receives the value.
    /// @return True on success; false leaves the cursor failed.
    [[nodiscard]] bool ReadU64(std::uint64_t& out) noexcept
    {
        if (!Has(sizeof(std::uint64_t)))
            return false;
        out = ReadBigEndian<std::uint64_t>(_bytes.subspan(_offset, sizeof(std::uint64_t)));
        _offset += sizeof(std::uint64_t);
        return true;
    }

    /// Read an element count, refusing one the remaining bytes cannot supply.
    ///
    /// **The reason this type exists.** @p minBytesEach has no default and never will:
    /// a caller that does not know what one element costs on its wire does not know
    /// enough to read a count safely.
    ///
    /// @p minBytesEach is a **security bound, not a sizing hint** — the distinction a
    /// later caller will get wrong. It must be a true *lower* bound on one element's
    /// wire cost, because a count is refused when the bytes cannot supply it and an
    /// over-estimate refuses honest data. It therefore under-estimates the element
    /// count for typical data, and that is what makes it correct rather than what makes
    /// it improvable.
    ///
    /// A validated count is still not a reservation: it is bounded by
    /// `Remaining() / minBytesEach`, which is an amplifier whenever an element is larger
    /// in memory than on the wire. Reserve from something this side owns, or not at all.
    /// `.agent/rules/wire-and-protocol.md` carries the worked cases.
    ///
    /// @param out Receives the count.
    /// @param minBytesEach The fewest wire bytes one element can occupy, read off this
    ///        format's own encoder. Zero refuses every count, including a zero count.
    /// @return True when a count was read AND the frame could carry that many;
    ///         false leaves the cursor failed.
    [[nodiscard]] bool ReadCount(std::uint32_t& out, std::size_t minBytesEach) noexcept
    {
        std::uint32_t declared = 0;
        if (!ReadU32(declared))
            return false;
        if (!WireFields::DeclaredCountFits(declared, minBytesEach, Remaining()))
            return Fail();
        out = declared;
        return true;
    }

    /// Read one length-prefixed field: a `u32` length, then that many bytes.
    ///
    /// The grammar `WireFields` describes, walked one field at a time. A length is safe
    /// to trust the moment it has been checked against the bytes present, which is what
    /// separates it from a count: a length describes bytes that must be here *now*,
    /// while a count describes elements yet to be read.
    /// @param out Receives the field's bytes.
    /// @return True on success; false leaves the cursor failed.
    [[nodiscard]] bool ReadField(std::string& out)
    {
        std::uint32_t length = 0;
        if (!ReadU32(length))
            return false;
        return ReadText(length, out);
    }

  private:
    /// @return Bytes not yet consumed; zero once exhausted or failed.
    ///
    /// Private because a count is the only thing a decoder legitimately wants it for,
    /// and `ReadCount` already does that; for "did I consume everything", see `AtEnd`.
    [[nodiscard]] constexpr std::size_t Remaining() const noexcept
    {
        return _ok ? _bytes.size() - _offset : 0;
    }

    /// Read a big-endian `u32`. Private: a bare `u32` read is how a count gets taken
    /// without its bound, which is the defect this type exists to make unwritable.
    /// @param out Receives the value.
    /// @return True on success; false leaves the cursor failed.
    [[nodiscard]] bool ReadU32(std::uint32_t& out) noexcept
    {
        if (!Has(sizeof(std::uint32_t)))
            return false;
        out = ReadBigEndian<std::uint32_t>(_bytes.subspan(_offset, sizeof(std::uint32_t)));
        _offset += sizeof(std::uint32_t);
        return true;
    }

    /// Read exactly @p n bytes as text.
    /// @param n How many.
    /// @param out Receives them, replacing its contents.
    /// @return True on success; false leaves the cursor failed.
    [[nodiscard]] bool ReadText(std::size_t n, std::string& out)
    {
        if (!Has(n))
            return false;
        out.assign(reinterpret_cast<char const*>(_bytes.data() + _offset), n);
        _offset += n;
        return true;
    }

    /// Whether @p n more bytes are available, failing the cursor when they are not.
    ///
    /// Spelled as a subtraction from the size, never `_offset + n > size`: @p n is
    /// routinely a peer's `u32`, and the additive form wraps wherever `std::size_t` is
    /// 32-bit, turning the bounds check into a pass on exactly the values it exists to
    /// refuse. `_offset <= _bytes.size()` is an invariant of this class, established in
    /// the constructor and preserved by every advance.
    [[nodiscard]] bool Has(std::size_t n) noexcept
    {
        if (!_ok)
            return false;
        if (_bytes.size() - _offset < n)
            return Fail();
        return true;
    }

    /// Mark the cursor failed.
    /// @return Always false, so a caller can `return Fail();`.
    bool Fail() noexcept
    {
        _ok = false;
        return false;
    }

    std::span<std::byte const> _bytes;
    std::size_t _offset { 0 };
    bool _ok { true };
};

} // namespace FastCache
